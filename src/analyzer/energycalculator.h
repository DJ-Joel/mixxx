#pragma once

#include <cstdint>
#include <vector>

/// Auto DJ 2.0: computes a 1..10 "energy" score for a whole track.
///
/// Pure C++ with no Mixxx or Qt dependencies, so it can be unit-tested with
/// synthetic audio. AnalyzerEnergy feeds it the decoded samples.
///
/// Three ingredients, each scaled to 0..1:
///  - loudness:   how loud the loud parts are (90th percentile of 1-second
///                RMS levels, before ReplayGain)
///  - brightness: share of energy above ~2.5 kHz (hats, synths, noise)
///  - busyness:   onsets per second (how often a new hit starts)
/// energy = 1 + 9 * (0.45 * loudness + 0.30 * brightness + 0.25 * busyness)
///
/// The weights and ranges are a starting point, to be tuned by ear.
class EnergyCalculator {
  public:
    /// Bump when the formula changes, so tracks get re-analysed.
    /// v2: adds bodyStartSec / bodyEndSec.
    /// v3: body end uses the stricter kBodyEndDropDb.
    /// v4: same numbers; re-run so the analyzer sets the Intro End and
    ///     Outro Start markers on tracks analysed before that existed.
    static constexpr int kVersion = 4;

    struct Result {
        double energy = 0.0;       ///< 1..10
        double loudness01 = 0.0;   ///< 0..1 part of the score
        double brightness01 = 0.0; ///< 0..1 part of the score
        double busyness01 = 0.0;   ///< 0..1 part of the score
        double loudnessDb = 0.0;   ///< raw: p90 of 1 s RMS, dBFS
        double brightRatio = 0.0;  ///< raw: high-band / full-band energy
        double onsetsPerSec = 0.0; ///< raw: onsets per second of sound
        double bassRatio = 0.0;    ///< raw: low-band / full-band energy (kept for tuning)
        /// The "body" of the track: from where it first gets going (the beat
        /// kicks in after a quiet intro) to where it starts to fade out.
        /// Seconds from the start of the track. Found by loudness: the body
        /// starts where the level first gets within kBodyStartDropDb of the
        /// track's p90, and ends where it last stays within kBodyEndDropDb.
        double bodyStartSec = 0.0;
        double bodyEndSec = 0.0;
    };

    /// How far below the track's usual loud level still counts as "body".
    /// Start: generous, so a slightly quieter first verse still counts.
    /// End: strict, so a mix is over before the track's own fade-out has
    /// taken the energy away (the dancers must never lose the beat).
    static constexpr double kBodyStartDropDb = 6.0;
    static constexpr double kBodyEndDropDb = 3.0;

    EnergyCalculator(double sampleRate, int channelCount);

    /// Feeds interleaved samples. `sampleCount` counts samples, not frames.
    void process(const float* pInterleaved, std::int64_t sampleCount);

    /// Returns false if there was too little non-silent audio (< 5 s).
    bool finish(Result* pResult) const;

    /// Maps the three 0..1 parts to the 1..10 score.
    static double combine(double loudness01, double brightness01, double busyness01);

  private:
    struct OnePole {
        double a = 0.0;
        double y = 0.0;
        double step(double x) {
            y += a * (x - y);
            return y;
        }
    };

    void endBlock();

    const double m_sampleRate;
    const int m_channelCount;
    const int m_blockFrames; // 20 ms

    // Two cascaded one-pole low-pass filters per band (12 dB/octave).
    OnePole m_low1, m_low2;   // < 150 Hz
    OnePole m_high1, m_high2; // subtracted to get > 2.5 kHz

    // Current block accumulators.
    int m_framesInBlock = 0;
    double m_blockSumFull = 0.0;
    double m_blockSumLow = 0.0;
    double m_blockSumHigh = 0.0;

    // Per-block results.
    std::vector<float> m_blockDb; // full-band level of each 20 ms block

    // Totals over non-silent blocks.
    double m_totalFull = 0.0;
    double m_totalLow = 0.0;
    double m_totalHigh = 0.0;
};
