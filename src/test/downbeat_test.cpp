#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "library/autodj/smart/downbeat.h"

namespace {

constexpr double kRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

/// A made-up song: 120 BPM, beats every 0.5 s from 1 s. Bars start on grid
/// beat `phase`: there the bass note and the chord change and the kick is
/// harder. `changes` false: the same note and chord all the way, all kicks
/// equal (nothing tells beat 1).
std::vector<float> song(int phase, bool changes, std::vector<double>* pBeats) {
    const int beats = 200;
    std::vector<float> audio(static_cast<std::size_t>((1.0 + beats * 0.5 + 1.0) * kRate), 0.0f);
    const double bassNotes[] = {55.0, 73.42, 82.41, 65.41};
    const double chordRoots[] = {261.6, 349.2, 392.0, 220.0};
    for (int b = 0; b < beats; ++b) {
        const double t0 = 1.0 + b * 0.5;
        pBeats->push_back(t0);
        const int bar = b >= phase ? (b - phase) / 4 : -1;
        const bool one = (b - phase) % 4 == 0 && b >= phase;
        const int k = changes ? ((bar % 4) + 4) % 4 : 0;
        const double bass = bassNotes[k];
        const double root = chordRoots[k];
        const std::size_t from = static_cast<std::size_t>(t0 * kRate);
        const std::size_t to = static_cast<std::size_t>((t0 + 0.5) * kRate);
        for (std::size_t i = from; i < to && i < audio.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double since = t - t0;
            double s = 0.3 * std::sin(2 * kPi * bass * t);
            s += 0.1 * (std::sin(2 * kPi * root * t) + std::sin(2 * kPi * root * 1.26 * t) +
                               std::sin(2 * kPi * root * 1.5 * t));
            // A click on every beat, harder on beat 1 (when it tells).
            const double hit = (changes && one) ? 0.8 : 0.4;
            s += hit * std::exp(-since / 0.01) * std::sin(2 * kPi * 3000.0 * t);
            audio[i] = static_cast<float>(s);
        }
    }
    return audio;
}

downbeat::Result run(int phase, bool changes) {
    std::vector<double> beats;
    const std::vector<float> audio = song(phase, changes, &beats);
    downbeat::Features features(kRate);
    for (std::size_t i = 0; i < audio.size(); i += 4096) {
        const int n = static_cast<int>(std::min<std::size_t>(4096, audio.size() - i));
        features.process(audio.data() + i, n);
    }
    return downbeat::find(features, beats);
}

} // namespace

TEST(DownbeatTest, FindsBeatOneWhereTheBassAndChordsChange) {
    for (int phase : {0, 1, 2, 3}) {
        const downbeat::Result r = run(phase, true);
        EXPECT_EQ(phase, r.phase) << "bars start on grid beat " << phase;
        EXPECT_TRUE(r.sure) << "phase " << phase << " margin " << r.margin;
        EXPECT_TRUE(r.halvesAgree);
    }
}

TEST(DownbeatTest, NotSureWhenNothingTells) {
    const downbeat::Result r = run(1, false);
    EXPECT_FALSE(r.sure) << "margin " << r.margin;
}

TEST(DownbeatTest, TooShortIsNotSure) {
    downbeat::Features features(kRate);
    std::vector<float> silence(44100, 0.0f);
    features.process(silence.data(), static_cast<int>(silence.size()));
    const std::vector<double> beats{0.1, 0.6};
    EXPECT_FALSE(downbeat::find(features, beats).sure);
}

TEST(DownbeatTest, CountsFromBeatOne) {
    const std::vector<double> beats{0.04, 0.35, 0.92, 1.49, 2.06};
    const auto moved = downbeat::fromBeatOne(beats, 1);
    ASSERT_EQ(4u, moved.size());
    EXPECT_DOUBLE_EQ(0.35, moved.front());
    EXPECT_EQ(beats, downbeat::fromBeatOne(beats, 0));
    EXPECT_EQ(beats, downbeat::fromBeatOne(beats, 9)); // nonsense: unchanged
}

// Beat 1 from the stem parts (AnalyzerStemDownbeat): the finder hears only
// the bass and the chords.
TEST(DownbeatTest, PartsGiveTheBassAndChordsOnly) {
    // One frame of stem audio: drums, bass, other, vocals (left, right).
    const float frame[8] = {9.0f, 9.0f, 0.2f, 0.4f, 0.6f, 0.8f, 7.0f, 7.0f};
    float mono = 0.0f;
    downbeat::bassAndChordsFromParts(frame, 1, &mono);
    EXPECT_FLOAT_EQ(0.5f, mono); // (0.2 + 0.4 + 0.6 + 0.8) / 4: no drums, no vocals
}

TEST(DownbeatTest, FindsBeatOneOnTheParts) {
    // The bars start on grid beat 2 (the bass and chords change there). The
    // drums hit hardest on beat 0 of every four and the singer's lines start
    // on beat 3; the finder must not hear those (on real songs they hide the
    // bass and chords; this made-up song is too simple to show that).
    constexpr int kPhase = 2;
    std::vector<double> beats;
    const std::vector<float> music = song(kPhase, true, &beats);
    std::vector<float> parts(music.size() * 8, 0.0f);
    for (std::size_t i = 0; i < music.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double beat = (t - 1.0) / 0.5;
        const int b = static_cast<int>(std::floor(beat));
        const double since = (beat - b) * 0.5;
        float drums = 0.0f;
        float vocals = 0.0f;
        if (b >= 0) {
            const bool accent = b % 4 == 0;
            drums = static_cast<float>((accent ? 1.5 : 0.3) * std::exp(-since / 0.02) *
                    std::sin(2 * kPi * 80.0 * t));
            const int line = (b + 1) / 4; // lines start on beat 3 of four
            const double note = 440.0 * std::pow(2.0, (line % 5) / 12.0);
            vocals = static_cast<float>(0.5 * std::sin(2 * kPi * note * t));
        }
        float* pFrame = &parts[i * 8];
        pFrame[0] = pFrame[1] = drums;
        pFrame[2] = pFrame[3] = music[i]; // bass
        pFrame[4] = pFrame[5] = music[i]; // other (chords)
        pFrame[6] = pFrame[7] = vocals;
    }
    downbeat::Features features(kRate);
    std::vector<float> mono(4096);
    for (std::size_t i = 0; i < music.size(); i += 4096) {
        const int n = static_cast<int>(std::min<std::size_t>(4096, music.size() - i));
        downbeat::bassAndChordsFromParts(&parts[i * 8], n, mono.data());
        features.process(mono.data(), n);
    }
    const downbeat::Result r = downbeat::find(features, beats);
    EXPECT_EQ(kPhase, r.phase);
    EXPECT_TRUE(r.sure) << "margin " << r.margin;
}
