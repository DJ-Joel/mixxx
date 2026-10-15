#include "library/autodj/smart/beatcheck.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace beatcheck {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 (Kaiser window).
double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half = x / 2.0;
    for (int k = 1; k < 60; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-12) {
            break;
        }
    }
    return sum;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}

// The typical beat length of `beats` between lo and hi seconds (the
// middle gap, then the mean of the gaps within 10% of it: one missed or
// extra beat does not count), and the share of [lo, hi] those gaps cover.
struct Typical {
    double beatSec = 0.0;
    double coverage = 0.0;
};
Typical typical(const std::vector<double>& beats, double lo, double hi) {
    Typical t;
    std::vector<double> gaps;
    for (std::size_t i = 0; i + 1 < beats.size(); ++i) {
        if (beats[i] >= lo && beats[i + 1] <= hi) {
            gaps.push_back(beats[i + 1] - beats[i]);
        }
    }
    if (gaps.size() < 4 || !(hi > lo)) {
        return t;
    }
    const double mid = median(gaps);
    double sum = 0.0;
    int count = 0;
    for (double g : gaps) {
        if (std::fabs(g / mid - 1.0) < 0.1) {
            sum += g;
            ++count;
        }
    }
    if (count == 0) {
        return t;
    }
    t.beatSec = sum / count;
    t.coverage = std::min(1.0, sum / (hi - lo));
    return t;
}

// Share of the gaps between lo and hi within 10% of `beatSec`.
double steadyShare(const std::vector<double>& beats, double lo, double hi, double beatSec) {
    int all = 0;
    int steady = 0;
    for (std::size_t i = 0; i + 1 < beats.size(); ++i) {
        if (beats[i] >= lo && beats[i + 1] <= hi) {
            ++all;
            steady += std::fabs((beats[i + 1] - beats[i]) / beatSec - 1.0) < 0.1 ? 1 : 0;
        }
    }
    return all > 0 ? static_cast<double>(steady) / all : 0.0;
}

// Index of the value in sorted `values` nearest to `x` (-1 if empty).
int nearest(const std::vector<double>& values, double x) {
    if (values.empty()) {
        return -1;
    }
    const auto it = std::lower_bound(values.begin(), values.end(), x);
    if (it == values.begin()) {
        return 0;
    }
    if (it == values.end()) {
        return static_cast<int>(values.size()) - 1;
    }
    const int i = static_cast<int>(it - values.begin());
    return (x - values[i - 1] <= values[i] - x) ? i - 1 : i;
}

// The local beat length around beat i (the middle of up to 8 gaps).
double localBeatSec(const std::vector<double>& beats, int i, double fallback) {
    std::vector<double> gaps;
    const int n = static_cast<int>(beats.size());
    for (int j = std::max(0, i - 4); j < std::min(n - 1, i + 4); ++j) {
        gaps.push_back(beats[j + 1] - beats[j]);
    }
    const double m = median(gaps);
    return m > 0.0 ? m : fallback;
}

// The beat check's beats at the level the song is counted in: as heard
// (1), every other beat (0.5: it heard double time), or with a beat in
// between (2). Beat 1 stays on beat 1.
Found atLevel(const Found& found, double level, double beatSec) {
    if (level == 1.0) {
        return found;
    }
    Found out;
    const auto& b = found.beats;
    const auto& d = found.downbeats;
    if (level == 0.5) {
        // Keep the beats an even number of (heard) beats from a downbeat.
        for (int i = 0; i < static_cast<int>(b.size()); ++i) {
            const int k = nearest(d, b[i]);
            if (k < 0) {
                out.beats.push_back(b[i]);
                continue;
            }
            const double local = localBeatSec(b, i, beatSec);
            const long steps = std::lround((b[i] - d[k]) / local);
            if (steps % 2 == 0) {
                out.beats.push_back(b[i]);
            }
        }
        // A bar of four (heard) beats is half a bar: every other downbeat.
        for (std::size_t k = 0; k < d.size(); k += 2) {
            out.downbeats.push_back(d[k]);
        }
        return out;
    }
    // level 2: a beat between each two (where none is missing).
    for (std::size_t i = 0; i < b.size(); ++i) {
        out.beats.push_back(b[i]);
        if (i + 1 < b.size() && b[i + 1] - b[i] < 1.5 * beatSec) {
            out.beats.push_back(0.5 * (b[i] + b[i + 1]));
        }
    }
    for (std::size_t k = 0; k < d.size(); ++k) {
        out.downbeats.push_back(d[k]);
        if (k + 1 < d.size() && d[k + 1] - d[k] < 6.0 * beatSec) {
            out.downbeats.push_back(0.5 * (d[k] + d[k + 1]));
        }
    }
    return out;
}

// One tempo through all the beats? A straight line through them (each
// beat given its number on that line), and how far the beats are from it.
struct Fit {
    double first = 0.0;   // time of beat number 0
    double beatSec = 0.0; // one beat
    double medianMiss = 1.0;
    double missP90 = 1.0;
};
Fit fitLine(const std::vector<double>& beats, double beatSec) {
    Fit fit;
    if (beats.size() < 8 || !(beatSec > 0.0)) {
        return fit;
    }
    double a = beats.front();
    double b = beatSec;
    std::vector<double> index(beats.size());
    for (int round = 0; round < 4; ++round) {
        for (std::size_t i = 0; i < beats.size(); ++i) {
            index[i] = std::round((beats[i] - a) / b);
        }
        const double n = static_cast<double>(beats.size());
        const double mx = std::accumulate(index.begin(), index.end(), 0.0) / n;
        const double my = std::accumulate(beats.begin(), beats.end(), 0.0) / n;
        double sxy = 0.0;
        double sxx = 0.0;
        for (std::size_t i = 0; i < beats.size(); ++i) {
            sxy += (index[i] - mx) * (beats[i] - my);
            sxx += (index[i] - mx) * (index[i] - mx);
        }
        if (!(sxx > 0.0)) {
            return fit;
        }
        b = sxy / sxx;
        a = my - b * mx;
    }
    std::vector<double> miss(beats.size());
    for (std::size_t i = 0; i < beats.size(); ++i) {
        miss[i] = std::fabs(beats[i] - (a + b * index[i]));
    }
    fit.first = a;
    fit.beatSec = b;
    fit.medianMiss = median(miss);
    std::sort(miss.begin(), miss.end());
    fit.missP90 = miss[static_cast<std::size_t>(0.9 * static_cast<double>(miss.size() - 1))];
    return fit;
}

// A beat map from the heard beats: missing beats filled in, the 20 ms
// steps of the beat check smoothed (a short straight line through each
// beat and its neighbours).
std::vector<double> smoothMap(const std::vector<double>& beats, double beatSec) {
    std::vector<double> filled;
    for (std::size_t i = 0; i < beats.size(); ++i) {
        filled.push_back(beats[i]);
        if (i + 1 < beats.size()) {
            const double local = localBeatSec(beats, static_cast<int>(i), beatSec);
            const double gap = beats[i + 1] - beats[i];
            const long n = std::lround(gap / local);
            if (n >= 2 && n <= 8) {
                for (long k = 1; k < n; ++k) {
                    filled.push_back(beats[i] + gap * static_cast<double>(k) / n);
                }
            }
        }
    }
    std::vector<double> out(filled.size());
    const int n = static_cast<int>(filled.size());
    for (int i = 0; i < n; ++i) {
        // Neighbours on both sides, within one run (no long gap between).
        int lo = i;
        int hi = i;
        while (lo > 0 && i - lo < 4 && filled[lo] - filled[lo - 1] < 1.5 * beatSec) {
            --lo;
        }
        while (hi < n - 1 && hi - i < 4 && filled[hi + 1] - filled[hi] < 1.5 * beatSec) {
            ++hi;
        }
        if (hi - lo < 2) {
            out[i] = filled[i];
            continue;
        }
        double mx = 0.0;
        double my = 0.0;
        for (int j = lo; j <= hi; ++j) {
            mx += j;
            my += filled[j];
        }
        mx /= (hi - lo + 1);
        my /= (hi - lo + 1);
        double sxy = 0.0;
        double sxx = 0.0;
        for (int j = lo; j <= hi; ++j) {
            sxy += (j - mx) * (filled[j] - my);
            sxx += (j - mx) * (j - mx);
        }
        out[i] = my + (sxx > 0.0 ? sxy / sxx : 0.0) * (i - mx);
    }
    // Strictly increasing (a beat map must be).
    for (int i = 1; i < n; ++i) {
        out[i] = std::max(out[i], out[i - 1] + 0.05);
    }
    return out;
}

} // namespace

std::vector<float> resample(const std::vector<float>& input, double inRate, double outRate) {
    if (input.empty() || !(inRate > 0.0) || !(outRate > 0.0)) {
        return {};
    }
    if (std::fabs(inRate - outRate) < 1e-9) {
        return input;
    }
    const double ratio = outRate / inRate;
    // Cutoff in cycles per input sample, a little below the lower Nyquist.
    const double cutoff = 0.5 * std::min(1.0, ratio) * 0.97;
    constexpr int kZeroCrossings = 32;
    constexpr double kBeta = 8.6;
    constexpr int kTableSteps = 512; // per input sample
    const double halfWidth = kZeroCrossings / (2.0 * cutoff); // in input samples
    const int tableSize = static_cast<int>(std::ceil(halfWidth * kTableSteps)) + 2;
    std::vector<double> table(tableSize);
    const double i0Beta = besselI0(kBeta);
    for (int i = 0; i < tableSize; ++i) {
        const double x = static_cast<double>(i) / kTableSteps;
        const double u = x / halfWidth;
        if (u >= 1.0) {
            table[i] = 0.0;
            continue;
        }
        const double arg = 2.0 * cutoff * x;
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
        table[i] = 2.0 * cutoff * sinc * besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0Beta;
    }
    const auto kernel = [&table](double x) {
        const double pos = std::fabs(x) * kTableSteps;
        const auto i = static_cast<std::size_t>(pos);
        if (i + 1 >= table.size()) {
            return 0.0;
        }
        const double frac = pos - static_cast<double>(i);
        return table[i] + (table[i + 1] - table[i]) * frac;
    };
    const auto outCount = static_cast<std::size_t>(std::floor(input.size() * ratio));
    std::vector<float> out(outCount);
    const long last = static_cast<long>(input.size()) - 1;
    for (std::size_t n = 0; n < outCount; ++n) {
        const double t = static_cast<double>(n) / ratio;
        const long from = std::max(0L, static_cast<long>(std::ceil(t - halfWidth)));
        const long to = std::min(last, static_cast<long>(std::floor(t + halfWidth)));
        double sum = 0.0;
        for (long k = from; k <= to; ++k) {
            sum += input[static_cast<std::size_t>(k)] * kernel(t - static_cast<double>(k));
        }
        out[n] = static_cast<float>(sum);
    }
    return out;
}

std::vector<int> chunkStarts(int frames) {
    std::vector<int> starts;
    const int step = kChunkFrames - 2 * kBorderFrames;
    for (int s = -kBorderFrames; s < frames - kBorderFrames; s += step) {
        starts.push_back(s);
    }
    if (starts.empty()) {
        starts.push_back(-kBorderFrames);
    }
    if (frames > step) {
        starts.back() = frames - (kChunkFrames - kBorderFrames);
    }
    return starts;
}

int chunkInput(const std::vector<float>& spect, int frames, int start, std::vector<float>* pOut) {
    const int from = std::max(start, 0);
    const int to = std::min(start + kChunkFrames, frames);
    const int left = std::max(0, -start);
    const int right = std::max(0, std::min(kBorderFrames, start + kChunkFrames - frames));
    const int length = left + std::max(0, to - from) + right;
    pOut->assign(static_cast<std::size_t>(length) * kMelBands, 0.0f);
    for (int f = from; f < to; ++f) {
        std::copy_n(spect.begin() + static_cast<std::ptrdiff_t>(f) * kMelBands,
                kMelBands,
                pOut->begin() + static_cast<std::ptrdiff_t>(left + f - from) * kMelBands);
    }
    return length;
}

Aggregator::Aggregator(int frames)
        : m_beat(std::max(0, frames), -1000.0f),
          m_downbeat(std::max(0, frames), -1000.0f),
          m_set(std::max(0, frames), false) {
}

void Aggregator::add(int start, const float* pBeat, const float* pDownbeat, int length) {
    // Without the untrusted edges; where an earlier piece already gave an
    // answer, it stays (Beat This: "keep_first").
    const int frames = static_cast<int>(m_beat.size());
    for (int i = kBorderFrames; i < length - kBorderFrames; ++i) {
        const int target = start + i;
        if (target < 0 || target >= frames || m_set[target]) {
            continue;
        }
        m_beat[target] = pBeat[i];
        m_downbeat[target] = pDownbeat[i];
        m_set[target] = true;
    }
}

namespace {
// Frames that are the highest within +-3 frames and above logit 0, with
// neighbouring frames merged into their mean (Beat This: max_pool1d(7, 1,
// 3), then deduplicate_peaks(width=1)).
std::vector<double> peakFrames(const std::vector<float>& logits) {
    const int n = static_cast<int>(logits.size());
    std::vector<int> frames;
    for (int i = 0; i < n; ++i) {
        float best = logits[i];
        for (int j = std::max(0, i - 3); j <= std::min(n - 1, i + 3); ++j) {
            best = std::max(best, logits[j]);
        }
        if (logits[i] == best && logits[i] > 0.0f) {
            frames.push_back(i);
        }
    }
    std::vector<double> out;
    if (frames.empty()) {
        return out;
    }
    double p = frames[0];
    int c = 1;
    for (std::size_t k = 1; k < frames.size(); ++k) {
        const int p2 = frames[k];
        if (p2 - p <= 1.0) {
            ++c;
            p += (p2 - p) / c;
        } else {
            out.push_back(p);
            p = p2;
            c = 1;
        }
    }
    out.push_back(p);
    return out;
}
} // namespace

Found peaks(const std::vector<float>& beatLogits, const std::vector<float>& downbeatLogits) {
    Found found;
    const auto beatFrames = peakFrames(beatLogits);
    for (double f : beatFrames) {
        found.beats.push_back(f / kFramesPerSecond);
    }
    for (double f : peakFrames(downbeatLogits)) {
        double d = f / kFramesPerSecond;
        if (!beatFrames.empty()) {
            // The nearest beat (the first of two equally near), measured in
            // whole frames as Beat This does: in seconds, a tie came out
            // differently on Windows and Linux (rounding).
            std::size_t best = 0;
            for (std::size_t i = 1; i < beatFrames.size(); ++i) {
                if (std::fabs(beatFrames[i] - f) < std::fabs(beatFrames[best] - f)) {
                    best = i;
                }
            }
            d = found.beats[best];
        }
        found.downbeats.push_back(d);
    }
    std::sort(found.downbeats.begin(), found.downbeats.end());
    found.downbeats.erase(std::unique(found.downbeats.begin(), found.downbeats.end()),
            found.downbeats.end());
    return found;
}

Review review(const Found& found, const Song& song) {
    Review r;
    if (!song.grid.isValid() || found.beats.size() < 16 || !(song.durationSec > 0.0)) {
        return r;
    }
    // The main part of the song: not its first or last 20 s.
    double lo = 20.0;
    double hi = song.durationSec - 20.0;
    if (hi - lo < 30.0) {
        lo = 0.0;
        hi = song.durationSec;
    }
    const Typical heard = typical(found.beats, lo, hi);
    if (!(heard.beatSec > 0.0) || heard.coverage < kMinCoverage) {
        return r; // too few beats heard to judge
    }
    r.checked = true;
    const double heardBpm = 60.0 / heard.beatSec;
    const double mixxxBeatSec =
            song.grid.isMap() ? typical(song.grid.beats, lo, hi).beatSec : song.grid.beatSec;
    if (!(mixxxBeatSec > 0.0)) {
        r.checked = false;
        return r;
    }
    r.mixxxBpm = 60.0 / mixxxBeatSec;

    // The same tempo, its double or its half: counted at Mixxx's level.
    double level = 0.0;
    for (double k : {1.0, 2.0, 0.5}) {
        if (std::fabs(heardBpm * k / r.mixxxBpm - 1.0) <= kTempoTolerance) {
            level = k;
            break;
        }
    }
    if (level == 0.0) {
        r.tempoWrong = true;
        level = heardBpm >= kMaxBpm ? 0.5 : (heardBpm < kMinBpm ? 2.0 : 1.0);
    }
    const Found counted = atLevel(found, level, heard.beatSec);
    const Typical songTempo = typical(counted.beats, lo, hi);
    const double beatSec = songTempo.beatSec > 0.0 ? songTempo.beatSec : heard.beatSec / level;
    r.songBpm = 60.0 / beatSec;

    // Do Mixxx's beat lines sit on the beats heard? Only judged where the
    // beat check counts steadily itself; a steady offset does not count.
    const bool countedSteadily = steadyShare(counted.beats, lo, hi, beatSec) >= kMinSteadyGaps;
    if (!r.tempoWrong && countedSteadily) {
        std::vector<double> misses; // heard beat minus Mixxx line, in seconds
        std::vector<double> halves; // ... in Mixxx's beats there, -1 where its tempo differs
        for (double t : counted.beats) {
            if (t < lo || t > hi) {
                continue;
            }
            const double beat = song.grid.beatAt(t);
            const double local = song.grid.beatSecAt(t);
            misses.push_back((beat - std::round(beat)) * local);
            halves.push_back(std::fabs(local / beatSec - 1.0) < 0.1 ? local : -1.0);
        }
        std::vector<double> closeMisses;
        for (double m : misses) {
            if (std::fabs(m) < beatSec / 3.0) {
                closeMisses.push_back(m);
            }
        }
        r.lineOffsetSec = closeMisses.size() >= misses.size() / 3 ? median(closeMisses) : 0.0;
        int on = 0;
        for (double m : misses) {
            on += std::fabs(m - r.lineOffsetSec) <= kOnBeatSec ? 1 : 0;
        }
        r.linesOnBeat = misses.empty() ? 1.0 : static_cast<double>(on) / misses.size();
        r.linesOff = !misses.empty() && r.linesOnBeat < kMinLinesOnBeat;
        if (r.linesOff) {
            int off = 0;
            int half = 0;
            for (std::size_t i = 0; i < misses.size(); ++i) {
                const double miss = std::fabs(misses[i] - r.lineOffsetSec);
                if (miss > kOnBeatSec) {
                    ++off;
                    const double beats = halves[i] > 0.0 ? miss / halves[i] : 0.0;
                    half += (beats > 0.35 && beats < 0.65) ? 1 : 0;
                }
            }
            r.halfBeat = off > 0 && half >= 0.6 * off;
        }
    }

    // An intro Mixxx reads at another tempo, but the beat check hears at
    // the song's tempo (most such intros: a made-up tempo where there is
    // no kick). One the beat check also hears slower is left alone.
    const double mainSec = phrasealign::mainBeatStart(song.grid);
    if (mainSec > 0.0 && !song.grid.beats.empty()) {
        const double introFrom = song.grid.beats.front();
        const Typical mixxxIntro = typical(song.grid.beats, introFrom, mainSec);
        const Typical heardIntro = typical(counted.beats, introFrom, mainSec);
        if (mixxxIntro.beatSec > 0.0 && heardIntro.beatSec > 0.0 && heardIntro.coverage >= 0.6 &&
                std::fabs(heardIntro.beatSec / beatSec - 1.0) <= 0.04) {
            r.introMisread = true;
            r.introBpm = 60.0 / mixxxIntro.beatSec;
        }
    }

    // The new beat grid, from the beats heard (not for a locked BPM).
    if ((r.tempoWrong || r.linesOff || r.introMisread) && !song.bpmLocked) {
        r.newGrid = true;
        const Fit fit = fitLine(counted.beats, beatSec);
        r.steady = fit.beatSec > 0.0 && fit.medianMiss < 0.012 && fit.missP90 < 0.03;
        if (r.steady) {
            r.newBpm = 60.0 / fit.beatSec;
            // Its first line on a bar line (beat 1), at or after the start.
            double bar = fit.first;
            if (!counted.downbeats.empty()) {
                const int k = nearest(counted.downbeats, std::max(lo, counted.downbeats.front()));
                const double d = counted.downbeats[k];
                bar = fit.first + fit.beatSec * std::round((d - fit.first) / fit.beatSec);
            }
            const double barSec = phrasealign::kBeatsPerBar * fit.beatSec;
            r.newFirstBeatSec = bar - std::floor(bar / barSec) * barSec;
        } else {
            std::vector<double> map = smoothMap(counted.beats, beatSec);
            // Starting on a bar line, so Mixxx's beat 1 is beat 1.
            if (!counted.downbeats.empty() && !map.empty()) {
                const int d = nearest(map, counted.downbeats.front());
                const int drop = d % phrasealign::kBeatsPerBar;
                map.erase(map.begin(), map.begin() + drop);
            }
            r.newBeats = std::move(map);
            r.newBpm = r.songBpm;
        }
    }

    // Beat 1 of the bar: the Intro End marker on a heard bar line? Only
    // where the beat check counts the song's beats (not half beats) there.
    if (song.introEndSec >= 0.0 && !counted.downbeats.empty()) {
        const int k = nearest(counted.downbeats, song.introEndSec);
        const double d = counted.downbeats[k];
        const double off = (song.introEndSec - d) / beatSec;
        const Typical around = typical(counted.beats, song.introEndSec - 4.0, song.introEndSec + 8.0);
        const bool countedHere =
                around.beatSec > 0.0 && std::fabs(around.beatSec / beatSec - 1.0) < 0.1;
        if (countedHere && !r.halfBeat && std::fabs(off) <= kBeatOneMaxBeats) {
            r.beatOneOffBeats = off;
            r.beatOneOff = std::fabs(off) > kBeatOneTolerance;
        }
        if (!song.introEndByDj) {
            double target = -1.0;
            if (r.beatOneOff) {
                target = d; // onto beat 1
            } else if (r.newGrid) {
                target = song.introEndSec; // keep it, on a line of the new grid
            }
            if (target >= 0.0 && r.newGrid) {
                if (r.steady) {
                    const double newBeatSec = 60.0 / r.newBpm;
                    target = r.newFirstBeatSec +
                            std::round((target - r.newFirstBeatSec) / newBeatSec) * newBeatSec;
                } else if (!r.newBeats.empty()) {
                    target = r.newBeats[nearest(r.newBeats, target)];
                }
            }
            if (target >= 0.0 && std::fabs(target - song.introEndSec) > 0.005) {
                r.newIntroEndSec = target;
            }
        }
    }
    return r;
}

} // namespace beatcheck
