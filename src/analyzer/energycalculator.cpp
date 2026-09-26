#include "analyzer/energycalculator.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBlockSeconds = 0.02; // 20 ms analysis blocks
constexpr int kBlocksPerWindow = 50;   // 1 s loudness windows
constexpr double kLowCutoffHz = 150.0;
constexpr double kHighCutoffHz = 2500.0;
constexpr double kSilenceDb = -60.0;
constexpr double kMinSoundSeconds = 5.0;

// Onset detection: a block this much louder than the average of the
// previous blocks, above a floor, and not too soon after the last one.
constexpr int kOnsetHistoryBlocks = 8; // 160 ms
constexpr double kOnsetRiseDb = 3.0;
constexpr double kOnsetFloorDb = -45.0;
constexpr int kOnsetMinGapBlocks = 3; // 60 ms

// Ranges mapped to 0..1 (starting points, to be tuned by ear).
constexpr double kLoudnessMinDb = -30.0;
constexpr double kLoudnessMaxDb = -8.0;
constexpr double kBrightRatioMin = 0.01;
constexpr double kBrightRatioMax = 0.30;
constexpr double kOnsetsMin = 1.0; // per second
constexpr double kOnsetsMax = 6.0;

constexpr double kWeightLoudness = 0.45;
constexpr double kWeightBrightness = 0.30;
constexpr double kWeightBusyness = 0.25;

double onePoleCoefficient(double cutoffHz, double sampleRate) {
    return 1.0 - std::exp(-2.0 * kPi * cutoffHz / sampleRate);
}

double clamp01(double x) {
    return std::clamp(x, 0.0, 1.0);
}

double powerToDb(double meanSquare) {
    return meanSquare > 1e-12 ? 10.0 * std::log10(meanSquare) : -120.0;
}

} // namespace

EnergyCalculator::EnergyCalculator(double sampleRate, int channelCount)
        : m_sampleRate(sampleRate > 0.0 ? sampleRate : 44100.0),
          m_channelCount(std::max(1, channelCount)),
          m_blockFrames(std::max(1, static_cast<int>(m_sampleRate * kBlockSeconds))) {
    m_low1.a = m_low2.a = onePoleCoefficient(kLowCutoffHz, m_sampleRate);
    m_high1.a = m_high2.a = onePoleCoefficient(kHighCutoffHz, m_sampleRate);
}

void EnergyCalculator::process(const float* pInterleaved, std::int64_t sampleCount) {
    const std::int64_t frames = sampleCount / m_channelCount;
    for (std::int64_t f = 0; f < frames; ++f) {
        // Mix all channels to mono.
        double x = 0.0;
        const float* pFrame = pInterleaved + f * m_channelCount;
        for (int c = 0; c < m_channelCount; ++c) {
            x += pFrame[c];
        }
        x /= m_channelCount;

        const double low = m_low2.step(m_low1.step(x));
        const double high = x - m_high2.step(m_high1.step(x));

        m_blockSumFull += x * x;
        m_blockSumLow += low * low;
        m_blockSumHigh += high * high;
        if (++m_framesInBlock >= m_blockFrames) {
            endBlock();
        }
    }
}

void EnergyCalculator::endBlock() {
    const double meanFull = m_blockSumFull / m_framesInBlock;
    const double db = powerToDb(meanFull);
    m_blockDb.push_back(static_cast<float>(db));
    if (db > kSilenceDb) {
        m_totalFull += m_blockSumFull;
        m_totalLow += m_blockSumLow;
        m_totalHigh += m_blockSumHigh;
    }
    m_framesInBlock = 0;
    m_blockSumFull = 0.0;
    m_blockSumLow = 0.0;
    m_blockSumHigh = 0.0;
}

// static
double EnergyCalculator::combine(double loudness01, double brightness01, double busyness01) {
    const double mix = kWeightLoudness * clamp01(loudness01) +
            kWeightBrightness * clamp01(brightness01) +
            kWeightBusyness * clamp01(busyness01);
    return 1.0 + 9.0 * clamp01(mix);
}

bool EnergyCalculator::finish(Result* pResult) const {
    const int blockCount = static_cast<int>(m_blockDb.size());
    int soundBlocks = 0;
    for (float db : m_blockDb) {
        if (db > kSilenceDb) {
            ++soundBlocks;
        }
    }
    const double soundSeconds = soundBlocks * kBlockSeconds;
    if (soundSeconds < kMinSoundSeconds || m_totalFull <= 0.0) {
        return false;
    }

    // Loudness: 90th percentile of 1 s window levels (ignoring silence).
    std::vector<double> windowDb;
    for (int start = 0; start + kBlocksPerWindow <= blockCount; start += kBlocksPerWindow) {
        double sumPower = 0.0;
        for (int b = start; b < start + kBlocksPerWindow; ++b) {
            sumPower += std::pow(10.0, m_blockDb[b] / 10.0);
        }
        const double db = powerToDb(sumPower / kBlocksPerWindow);
        if (db > kSilenceDb) {
            windowDb.push_back(db);
        }
    }
    if (windowDb.empty()) {
        return false;
    }
    std::sort(windowDb.begin(), windowDb.end());
    const auto p90Index = static_cast<std::size_t>(0.9 * (windowDb.size() - 1));
    const double loudnessDb = windowDb[p90Index];

    // Busyness: count onsets.
    int onsets = 0;
    int lastOnset = -kOnsetMinGapBlocks;
    for (int i = kOnsetHistoryBlocks; i < blockCount; ++i) {
        const double db = m_blockDb[i];
        if (db < kOnsetFloorDb || i - lastOnset < kOnsetMinGapBlocks) {
            continue;
        }
        double history = 0.0;
        for (int h = i - kOnsetHistoryBlocks; h < i; ++h) {
            history += std::max<double>(m_blockDb[h], -90.0);
        }
        history /= kOnsetHistoryBlocks;
        if (db - history > kOnsetRiseDb) {
            ++onsets;
            lastOnset = i;
        }
    }
    const double onsetsPerSec = onsets / soundSeconds;

    Result r;
    r.loudnessDb = loudnessDb;
    r.brightRatio = m_totalHigh / m_totalFull;
    r.bassRatio = m_totalLow / m_totalFull;
    r.onsetsPerSec = onsetsPerSec;
    r.loudness01 = clamp01((loudnessDb - kLoudnessMinDb) / (kLoudnessMaxDb - kLoudnessMinDb));
    r.brightness01 = r.brightRatio > 0.0
            ? clamp01((std::log10(r.brightRatio) - std::log10(kBrightRatioMin)) /
                      (std::log10(kBrightRatioMax) - std::log10(kBrightRatioMin)))
            : 0.0;
    r.busyness01 = clamp01((onsetsPerSec - kOnsetsMin) / (kOnsetsMax - kOnsetsMin));
    r.energy = combine(r.loudness01, r.brightness01, r.busyness01);
    *pResult = r;
    return true;
}
