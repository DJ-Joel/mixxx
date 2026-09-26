#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "analyzer/energycalculator.h"

namespace {

constexpr double kRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

// Builds `seconds` of stereo audio from a mono generator f(t in seconds).
template<typename F>
std::vector<float> stereo(double seconds, F f) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(f(i / kRate));
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    return out;
}

bool analyse(const std::vector<float>& samples, EnergyCalculator::Result* pResult, int channels = 2) {
    EnergyCalculator calc(kRate, channels);
    // Feed in chunks like the analyzer thread does.
    constexpr std::size_t kChunk = 4096 * 2;
    for (std::size_t i = 0; i < samples.size(); i += kChunk) {
        const std::size_t n = std::min(kChunk, samples.size() - i);
        calc.process(samples.data() + i, static_cast<std::int64_t>(n));
    }
    return calc.finish(pResult);
}

} // namespace

TEST(EnergyCalculatorTest, SilenceHasNoResult) {
    EnergyCalculator::Result r;
    EXPECT_FALSE(analyse(stereo(20, [](double) { return 0.0; }), &r));
}

TEST(EnergyCalculatorTest, TooShortHasNoResult) {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> noise(-0.3, 0.3);
    EnergyCalculator::Result r;
    EXPECT_FALSE(analyse(stereo(3, [&](double) { return noise(rng); }), &r));
}

TEST(EnergyCalculatorTest, CombineStaysInRangeAndRises) {
    EXPECT_DOUBLE_EQ(1.0, EnergyCalculator::combine(0, 0, 0));
    EXPECT_DOUBLE_EQ(10.0, EnergyCalculator::combine(1, 1, 1));
    EXPECT_DOUBLE_EQ(10.0, EnergyCalculator::combine(5, 5, 5)); // clamped
    EXPECT_LT(EnergyCalculator::combine(0.2, 0.5, 0.5), EnergyCalculator::combine(0.8, 0.5, 0.5));
    EXPECT_LT(EnergyCalculator::combine(0.5, 0.2, 0.5), EnergyCalculator::combine(0.5, 0.8, 0.5));
    EXPECT_LT(EnergyCalculator::combine(0.5, 0.5, 0.2), EnergyCalculator::combine(0.5, 0.5, 0.8));
}

TEST(EnergyCalculatorTest, CountsOnsetsOfAClickTrack) {
    // 4 hits per second (quarter notes at 240 BPM) over a quiet noise floor.
    std::mt19937 rng(2);
    std::uniform_real_distribution<double> noise(-0.01, 0.01);
    const auto audio = stereo(30, [&](double t) {
        const double sinceHit = std::fmod(t, 0.25);
        const double hit = sinceHit < 0.03 ? 0.6 * std::exp(-sinceHit * 80.0) : 0.0;
        return noise(rng) + hit * std::sin(2 * kPi * 100 * t);
    });
    EnergyCalculator::Result r;
    ASSERT_TRUE(analyse(audio, &r));
    EXPECT_NEAR(4.0, r.onsetsPerSec, 0.5);
}

TEST(EnergyCalculatorTest, NoiseIsBrighterThanBass) {
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> noise(-0.3, 0.3);
    EnergyCalculator::Result white;
    EnergyCalculator::Result bass;
    ASSERT_TRUE(analyse(stereo(20, [&](double) { return noise(rng); }), &white));
    ASSERT_TRUE(analyse(stereo(20, [](double t) { return 0.3 * std::sin(2 * kPi * 60 * t); }), &bass));
    EXPECT_GT(white.brightRatio, 0.3);
    EXPECT_LT(bass.brightRatio, 0.01);
    EXPECT_GT(bass.bassRatio, 0.6); // gentle 12 dB/oct filters: 60 Hz keeps ~74%
    EXPECT_GT(white.brightness01, bass.brightness01);
}

TEST(EnergyCalculatorTest, LoudBusyBrightBeatsQuietCalm) {
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    // "Peak-time": loud, bright noise with a hit every 0.25 s.
    const auto peak = stereo(30, [&](double t) {
        const double sinceHit = std::fmod(t, 0.25);
        const double hit = sinceHit < 0.03 ? std::exp(-sinceHit * 80.0) : 0.0;
        return 0.15 * noise(rng) + 0.7 * hit * std::sin(2 * kPi * 60 * t);
    });
    // "Ambient": a quiet, soft chord with no hits.
    const auto ambient = stereo(30, [](double t) {
        return 0.02 * (std::sin(2 * kPi * 220 * t) + std::sin(2 * kPi * 277 * t));
    });
    EnergyCalculator::Result rPeak;
    EnergyCalculator::Result rAmbient;
    ASSERT_TRUE(analyse(peak, &rPeak));
    ASSERT_TRUE(analyse(ambient, &rAmbient));
    EXPECT_GT(rPeak.energy, rAmbient.energy + 3.0)
            << "peak " << rPeak.energy << " ambient " << rAmbient.energy;
    EXPECT_GE(rAmbient.energy, 1.0);
    EXPECT_LE(rPeak.energy, 10.0);
}

TEST(EnergyCalculatorTest, MonoAndStereoAgree) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> noise(-0.2, 0.2);
    std::vector<float> mono(static_cast<std::size_t>(20 * kRate));
    for (auto& s : mono) {
        s = static_cast<float>(noise(rng));
    }
    std::vector<float> both(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        both[2 * i] = both[2 * i + 1] = mono[i];
    }
    EnergyCalculator::Result rMono;
    EnergyCalculator::Result rStereo;
    ASSERT_TRUE(analyse(mono, &rMono, 1));
    ASSERT_TRUE(analyse(both, &rStereo, 2));
    EXPECT_NEAR(rMono.energy, rStereo.energy, 1e-6);
}

TEST(EnergyCalculatorTest, FindsTheBodyBetweenQuietIntroAndFadeOut) {
    // 16 s quiet pad intro, 60 s loud and busy, then a 20 s fade-out.
    std::mt19937 rng(6);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    const auto audio = stereo(96, [&](double t) {
        if (t < 16.0) {
            return 0.02 * std::sin(2 * kPi * 220 * t); // pad, no drums
        }
        const double sinceHit = std::fmod(t, 0.5);
        const double hit = sinceHit < 0.03 ? std::exp(-sinceHit * 80.0) : 0.0;
        double level = 1.0;
        if (t > 76.0) {
            level = std::max(0.0, 1.0 - (t - 76.0) / 20.0); // linear fade-out
        }
        return level * (0.15 * noise(rng) + 0.7 * hit * std::sin(2 * kPi * 60 * t));
    });
    EnergyCalculator::Result r;
    ASSERT_TRUE(analyse(audio, &r));
    EXPECT_NEAR(16.0, r.bodyStartSec, 1.5) << r.bodyStartSec;
    // Body end uses -3 dB (0.71 of the amplitude): about 6 s into the 20 s
    // fade (~82 s), well before the track has faded to nothing.
    EXPECT_GT(r.bodyEndSec, 79.0) << r.bodyEndSec;
    EXPECT_LT(r.bodyEndSec, 85.0) << r.bodyEndSec;
}

TEST(EnergyCalculatorTest, WholeTrackIsBodyWhenLevelIsSteady) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-0.3, 0.3);
    EnergyCalculator::Result r;
    ASSERT_TRUE(analyse(stereo(40, [&](double) { return noise(rng); }), &r));
    EXPECT_NEAR(0.0, r.bodyStartSec, 1e-9);
    EXPECT_NEAR(40.0, r.bodyEndSec, 1.0);
}

TEST(EnergyCalculatorTest, FindsTheDrumsAfterALoudIntroWithoutBass) {
    // 20.3 s of loud synth noise with no bass (as loud as the rest), then
    // the same noise plus a bass kick every 0.5 s. Loudness alone would say
    // the body starts at 0; the bass says 20.3 s (the first kick).
    std::mt19937 rng(8);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    double hp = 0.0;
    const auto audio = stereo(80, [&](double t) {
        // crude high-pass: noise minus its smoothed version
        const double n = noise(rng);
        hp += 0.05 * (n - hp);
        const double synth = 0.25 * (n - hp);
        if (t < 20.3) {
            return synth;
        }
        const double sinceHit = std::fmod(t - 20.3, 0.5);
        const double hit = sinceHit < 0.08 ? std::exp(-sinceHit * 30.0) : 0.0;
        return synth + 0.6 * hit * std::sin(2 * kPi * 55 * t);
    });
    EnergyCalculator::Result r;
    ASSERT_TRUE(analyse(audio, &r));
    EXPECT_NEAR(20.3, r.bodyStartSec, 0.1) << r.bodyStartSec;
}
