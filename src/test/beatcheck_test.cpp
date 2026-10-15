#include "library/autodj/smart/beatcheck.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "analyzer/energycalculator.h"

// Auto DJ 2.0 plus Video Mixing beat check (beatcheck.h). The expected
// values for the model input and output handling come from the Beat This
// Python code (split_piece, aggregate_prediction, Postprocessor
// "minimal"); the rules use made-up songs whose right answer is known.

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double hz, double rate, double seconds) {
    std::vector<float> x(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate));
    }
    return x;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

// Beats every beatSec from first to end, a downbeat every 4.
beatcheck::Found heard(double beatSec, double first, double end) {
    beatcheck::Found f;
    int n = 0;
    for (double t = first; t < end; t += beatSec, ++n) {
        f.beats.push_back(t);
        if (n % 4 == 0) {
            f.downbeats.push_back(t);
        }
    }
    return f;
}

phrasealign::Grid steadyGrid(double bpm, double first) {
    phrasealign::Grid g;
    g.beatSec = 60.0 / bpm;
    g.firstBeatSec = first;
    return g;
}

beatcheck::Song song(const phrasealign::Grid& grid, double seconds = 240.0) {
    beatcheck::Song s;
    s.grid = grid;
    s.durationSec = seconds;
    return s;
}

} // namespace

TEST(BeatCheckTest, ResampleKeepsTheMusicAndRemovesWhatCannotFit) {
    // 1 kHz stays (same loudness, same pitch).
    const auto low = beatcheck::resample(sine(1000.0, 44100.0, 1.0), 44100.0, 22050.0);
    ASSERT_EQ(22050u, low.size());
    EXPECT_NEAR(std::sqrt(0.5), rms(low, 2000, 20000), 0.01);
    int crossings = 0;
    for (std::size_t i = 2001; i < 20000; ++i) {
        crossings += (low[i - 1] < 0.0f) != (low[i] < 0.0f) ? 1 : 0;
    }
    EXPECT_NEAR(2.0 * 1000.0 * (18000.0 / 22050.0), crossings, 4.0);
    // 15 kHz cannot exist at 22050 Hz: it is removed, not folded down.
    const auto high = beatcheck::resample(sine(15000.0, 44100.0, 1.0), 44100.0, 22050.0);
    EXPECT_LT(rms(high, 2000, 20000), 0.001);
    // 48000 Hz works too, and the same rate is left as it is.
    EXPECT_EQ(22050u, beatcheck::resample(sine(440.0, 48000.0, 1.0), 48000.0, 22050.0).size());
    EXPECT_EQ(100u, beatcheck::resample(std::vector<float>(100, 0.5f), 22050.0, 22050.0).size());
}

TEST(BeatCheckTest, ChunksAreSplitLikeBeatThis) {
    EXPECT_EQ(std::vector<int>({-6}), beatcheck::chunkStarts(100));
    EXPECT_EQ(std::vector<int>({-6}), beatcheck::chunkStarts(1488));
    EXPECT_EQ(std::vector<int>({-6, -5}), beatcheck::chunkStarts(1489));
    EXPECT_EQ(std::vector<int>({-6, 1482, 1506}), beatcheck::chunkStarts(3000));
    EXPECT_EQ(std::vector<int>({-6, 1482, 2970, 3006}), beatcheck::chunkStarts(4500));
    // Padded with silence at the start, and at the end of a short song.
    std::vector<float> spect(100 * beatcheck::kMelBands, 1.0f);
    std::vector<float> chunk;
    EXPECT_EQ(112, beatcheck::chunkInput(spect, 100, -6, &chunk));
    EXPECT_EQ(0.0f, chunk[5 * beatcheck::kMelBands]);
    EXPECT_EQ(1.0f, chunk[6 * beatcheck::kMelBands]);
    EXPECT_EQ(0.0f, chunk[106 * beatcheck::kMelBands]);
    std::vector<float> longSpect(3000 * beatcheck::kMelBands, 1.0f);
    EXPECT_EQ(1500, beatcheck::chunkInput(longSpect, 3000, 1506, &chunk));
}

TEST(BeatCheckTest, AggregatorKeepsTheEarlierPieceWithoutEdges) {
    beatcheck::Aggregator agg(20);
    std::vector<float> first(20, 1.0f);
    std::vector<float> second(20, 2.0f);
    agg.add(-6, first.data(), first.data(), 20);  // frames 0..7 (edges dropped)
    agg.add(0, second.data(), second.data(), 20); // frames 6..13
    EXPECT_EQ(1.0f, agg.beat()[0]);
    EXPECT_EQ(1.0f, agg.beat()[7]);  // the earlier piece wins the overlap
    EXPECT_EQ(2.0f, agg.beat()[8]);
    EXPECT_EQ(2.0f, agg.beat()[13]);
    EXPECT_EQ(-1000.0f, agg.beat()[14]); // nothing there
}

TEST(BeatCheckTest, PeaksAreFoundLikeBeatThis) {
    std::vector<float> b(60, -5.0f);
    std::vector<float> d(60, -5.0f);
    b[10] = 2.0f;
    b[11] = 2.0f; // two equal neighbours: one beat between them
    b[20] = 0.5f;
    b[22] = 1.0f; // the higher one within 3 frames
    b[30] = -0.1f; // below a probability of 0.5
    b[40] = 3.0f;
    b[42] = 3.0f; // two apart: two beats
    b[50] = 1.0f;
    d[11] = 1.0f;
    d[41] = 2.0f; // as near to 0.80 as to 0.84: the first
    d[49] = 1.0f;
    const beatcheck::Found f = beatcheck::peaks(b, d);
    const std::vector<double> beats{0.21, 0.44, 0.80, 0.84, 1.00};
    const std::vector<double> downbeats{0.21, 0.80, 1.00};
    ASSERT_EQ(beats.size(), f.beats.size());
    for (std::size_t i = 0; i < beats.size(); ++i) {
        EXPECT_NEAR(beats[i], f.beats[i], 1e-9);
    }
    ASSERT_EQ(downbeats.size(), f.downbeats.size());
    for (std::size_t i = 0; i < downbeats.size(); ++i) {
        EXPECT_NEAR(downbeats[i], f.downbeats[i], 1e-9);
    }
}

TEST(BeatCheckTest, ARightGridLooksRight) {
    // Mixxx's lines 40 ms before the beats heard (common): not a problem.
    const auto r = beatcheck::review(heard(60.0 / 120.0, 0.54, 240.0), song(steadyGrid(120.0, 0.5)));
    ASSERT_TRUE(r.checked);
    EXPECT_FALSE(r.anyProblem());
    EXPECT_FALSE(r.anyFix());
    EXPECT_NEAR(120.0, r.songBpm, 0.01);
    EXPECT_NEAR(0.04, r.lineOffsetSec, 0.001); // the beats 40 ms after the lines
    // Heard in double time: the same tempo.
    const auto d = beatcheck::review(heard(60.0 / 240.0, 0.5, 240.0), song(steadyGrid(120.0, 0.5)));
    EXPECT_FALSE(d.tempoWrong);
    EXPECT_NEAR(120.0, d.songBpm, 0.01);
}

TEST(BeatCheckTest, ADottedRhythmReadAsTheBeatGetsTheRightTempo) {
    // Night Drive "After Dark": Mixxx 113.3, heard at 170 (= 2 x 85), beat 1 at 0.207 s.
    const auto r = beatcheck::review(heard(60.0 / 170.0, 0.207, 205.0),
            song(steadyGrid(113.33, 0.21), 205.0));
    ASSERT_TRUE(r.checked);
    EXPECT_TRUE(r.tempoWrong);
    EXPECT_NEAR(85.0, r.songBpm, 0.01);
    ASSERT_TRUE(r.newGrid);
    EXPECT_TRUE(r.steady);
    EXPECT_NEAR(85.0, r.newBpm, 0.001);
    EXPECT_NEAR(0.207, r.newFirstBeatSec, 0.001); // on beat 1
    EXPECT_FALSE(r.replacesLock);
    // A locked BPM: the same grid, marked as replacing the lock (only done
    // if the DJ picks it; "Fix all" leaves it).
    auto locked = song(steadyGrid(113.33, 0.21), 205.0);
    locked.bpmLocked = true;
    const auto l = beatcheck::review(heard(60.0 / 170.0, 0.207, 205.0), locked);
    EXPECT_TRUE(l.tempoWrong);
    EXPECT_TRUE(l.newGrid);
    EXPECT_TRUE(l.replacesLock);
    // A grid that failed the check against the music is not offered.
    auto failed = song(steadyGrid(113.33, 0.21), 205.0);
    failed.allowNewGrid = false;
    const auto f = beatcheck::review(heard(60.0 / 170.0, 0.207, 205.0), failed);
    EXPECT_TRUE(f.tempoWrong);
    EXPECT_FALSE(f.newGrid);
}

TEST(BeatCheckTest, AnIntroMisreadGetsANewGrid) {
    // Mixxx: 150 BPM for 20 s of intro, then 112. Heard: 112 throughout.
    std::vector<double> map;
    double t = 0.5;
    for (int i = 0; i < 50; ++i, t += 60.0 / 150.0) {
        map.push_back(t);
    }
    const double mainStart = t;
    for (; t < 240.0; t += 60.0 / 112.0) {
        map.push_back(t);
    }
    beatcheck::Song s = song(phrasealign::Grid::fromBeats(map));
    const auto r = beatcheck::review(heard(60.0 / 112.0, mainStart - 40 * 60.0 / 112.0, 240.0), s);
    EXPECT_TRUE(r.introMisread);
    EXPECT_NEAR(150.0, r.introBpm, 0.5);
    EXPECT_TRUE(r.newGrid);
    EXPECT_TRUE(r.introOnly);
    EXPECT_NEAR(112.0, r.newBpm, 0.01);
    // Mixxx's lines in the main part are kept exactly.
    ASSERT_TRUE(r.steady);
    const double k = (mainStart - r.newFirstBeatSec) / (60.0 / r.newBpm);
    EXPECT_NEAR(std::round(k), k, 0.002);
}

TEST(BeatCheckTest, AWobblyBeatCheckDoesNotSpoilARightMainPart) {
    // Ward Brothers "Madness Of It All": Mixxx reads the intro at double
    // time, the main part right (101.1); the beat check hears 101 but
    // wobbles (beats missing, a stretch where it counts 4 beats in 3).
    const double beat = 60.0 / 101.1;
    std::vector<double> map;
    double t = 0.3;
    for (int i = 0; i < 64; ++i, t += beat / 2.0) {
        map.push_back(t); // the intro at 202
    }
    const double mainStart = t;
    for (; t < 260.0; t += beat) {
        map.push_back(t);
    }
    beatcheck::Found found;
    std::mt19937 rng(7);
    std::normal_distribution<double> jitter(0.0, 0.008);
    int n = 0;
    for (double b = mainStart - 31 * beat; b < 260.0; b += beat, ++n) {
        if (b > 40.0 && b < 70.0) {
            continue; // a stretch it hears differently...
        }
        if (n % 9 == 4) {
            continue; // ...and beats it misses
        }
        found.beats.push_back(b + 0.02 + jitter(rng));
        if (n % 4 == 0) {
            found.downbeats.push_back(found.beats.back());
        }
    }
    for (double b = 40.0; b < 70.0; b += beat * 0.75) {
        found.beats.push_back(b);
    }
    std::sort(found.beats.begin(), found.beats.end());
    const auto r = beatcheck::review(found, song(phrasealign::Grid::fromBeats(map), 270.0));
    ASSERT_TRUE(r.introMisread);
    ASSERT_TRUE(r.newGrid);
    EXPECT_TRUE(r.introOnly);
    EXPECT_NEAR(101.1, r.newBpm, 0.05);
    // Every main-part line where Mixxx had it.
    const phrasealign::Grid g = r.steady
            ? phrasealign::Grid{r.newFirstBeatSec, 60.0 / r.newBpm}
            : phrasealign::Grid::fromBeats(r.newBeats);
    for (double m = mainStart; m < 250.0; m += 37 * beat) {
        const double b = g.beatAt(m);
        EXPECT_NEAR(std::round(b), b, 0.01) << m;
    }
}

TEST(BeatCheckTest, KeptLevelsGiveTheSameGridCheck) {
    // A kick every 0.5 s for 200 s, checked straight and from the kept
    // (packed) levels, with a right and a slightly wrong grid.
    constexpr double kRate = 22050.0;
    std::vector<float> mono(static_cast<std::size_t>(200 * kRate));
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double since = std::fmod(i / kRate - 0.3 + 10.0, 0.5);
        mono[i] = static_cast<float>(0.6 * std::sin(2 * kPi * 55 * since) * std::exp(-since / 0.06));
    }
    EnergyCalculator calc(kRate, 1);
    calc.process(mono.data(), static_cast<std::int64_t>(mono.size()));
    EnergyCalculator::Result result;
    ASSERT_TRUE(calc.finish(&result));
    const auto levels = calc.gridLevels();
    EnergyCalculator::GridLevels kept;
    kept.blockSec = levels.blockSec;
    const auto low = beatcheck::packLevels(levels.lowDb);
    const auto high = beatcheck::packLevels(levels.highDb);
    kept.lowDb = beatcheck::unpackLevels(low.data(), low.size());
    kept.highDb = beatcheck::unpackLevels(high.data(), high.size());
    for (double beatSec : {0.5, 0.5 * 1.002}) {
        const phrasealign::Grid grid{0.3, beatSec};
        const double straight = calc.gridDriftBeats(grid, result.bodyStartSec, result.bodyEndSec);
        const double fromKept = EnergyCalculator::gridDriftBeats(
                kept, grid, result.bodyStartSec, result.bodyEndSec);
        EXPECT_NEAR(straight, fromKept, 0.01) << beatSec;
    }
    EXPECT_LT(EnergyCalculator::gridDriftBeats(
                      kept, phrasealign::Grid{0.3, 0.5}, result.bodyStartSec, result.bodyEndSec),
            EnergyCalculator::kGridMaxDriftBeats);
    EXPECT_GT(EnergyCalculator::gridDriftBeats(kept,
                      phrasealign::Grid{0.3, 0.5 * 1.002},
                      result.bodyStartSec,
                      result.bodyEndSec),
            EnergyCalculator::kGridMaxDriftBeats);
}

TEST(BeatCheckTest, BeatOneOffMovesAMarkerTheDjDidNotSet) {
    const double beat = 60.0 / 120.0;
    auto s = song(steadyGrid(120.0, 0.5));
    s.introEndSec = 16.5 + beat; // one beat after a heard downbeat (16.5 s)
    const auto r = beatcheck::review(heard(beat, 0.5, 240.0), s);
    EXPECT_TRUE(r.beatOneOff);
    EXPECT_NEAR(1.0, r.beatOneOffBeats, 0.01);
    EXPECT_NEAR(16.5, r.newIntroEndSec, 0.001);
    // The DJ's marker is reported, not moved.
    s.introEndByDj = true;
    const auto dj = beatcheck::review(heard(beat, 0.5, 240.0), s);
    EXPECT_TRUE(dj.beatOneOff);
    EXPECT_LT(dj.newIntroEndSec, 0.0);
    // Two beats off (half a bar) is left alone: the beat check itself is
    // unsure there.
    s.introEndByDj = false;
    s.introEndSec = 16.5 + 2.0 * beat;
    EXPECT_FALSE(beatcheck::review(heard(beat, 0.5, 240.0), s).beatOneOff);
}

TEST(BeatCheckTest, HalfABeatApartIsMarkedForListening) {
    // Same tempo, but the beats heard fall between Mixxx's lines.
    const auto r = beatcheck::review(heard(60.0 / 114.0, 0.5 + 0.5 * 60.0 / 114.0, 240.0),
            song(steadyGrid(114.0, 0.5)));
    EXPECT_TRUE(r.linesOff);
    EXPECT_TRUE(r.halfBeat);
    EXPECT_TRUE(r.newGrid); // the DJ can still choose the beat check's grid
}

TEST(BeatCheckTest, TooFewBeatsIsNotJudged) {
    // An ambient piece: beats in only a quarter of it.
    const auto r = beatcheck::review(heard(0.5, 100.0, 160.0), song(steadyGrid(120.0, 0.5)));
    EXPECT_FALSE(r.checked);
    EXPECT_FALSE(r.anyProblem());
}
