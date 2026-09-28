#pragma once

#include <array>
#include <complex>
#include <vector>

/// Auto DJ 2.0 plus Video Mixing: finds beat "1" of the bar.
///
/// Mixxx's beat grid has no downbeat: bars and phrases are counted from its
/// first beat line, which is often not beat 1 (a pickup, an intro that
/// starts on beat 2, a false first beat). Most of this music has a kick on
/// every beat, so the kick cannot tell; but the bass note and the chords
/// usually change on beat 1, and beat 1 is often hit harder. For each of the
/// four places in the bar this adds up those changes over the whole song;
/// the clear winner is beat 1. Only a clear winner that holds in both halves
/// of the song is used ("sure"); otherwise the grid is left as it is.
/// Checked by ear in a blind test on real songs before use. Pure maths, unit
/// tested (downbeat_test.cpp).
namespace downbeat {

/// Music features of a song, frame by frame (about 43 per second).
class Features {
  public:
    explicit Features(double sampleRate);
    /// Mono audio, in order, any block size.
    void process(const float* pMono, int count);

    double frameSec() const; ///< time between frames
    double frameTime(int frame) const; ///< centre of a frame, seconds
    int frameCount() const {
        return static_cast<int>(m_flux.size());
    }

    std::vector<std::array<float, 12>> bass;    ///< bass notes (35-200 Hz) by pitch
    std::vector<std::array<float, 12>> harmony; ///< chords (200-2000 Hz) by pitch

    const std::vector<float>& flux() const {
        return m_flux;
    }

    static constexpr int kFftSize = 2048;
    static constexpr int kHop = 256;
    static constexpr double kTargetRate = 11025.0;

  private:
    void frame();

    int m_decimate = 1;
    double m_rate = kTargetRate; ///< after decimation
    double m_sum = 0.0;
    int m_summed = 0;
    std::vector<float> m_ring; ///< last kFftSize samples (circular)
    int m_pos = 0;             ///< where the next sample goes
    long long m_filled = 0; ///< samples taken in (after decimation)
    std::vector<float> m_window;
    std::vector<int> m_bassPitch; ///< per FFT bin: pitch class, or -1
    std::vector<int> m_harmonyPitch;
    std::vector<float> m_lastLog; ///< log magnitudes of the last frame
    std::vector<float> m_flux;    ///< how much new sound starts, per frame
};

struct Result {
    int phase = 0;          ///< grid beat index % 4 of beat 1 (0 = the grid is right)
    double margin = 0.0;    ///< how clearly it won (score difference)
    bool halvesAgree = false;
    bool steady = false;    ///< the same with the beat lines a little early or late
    bool sure = false;      ///< clear and steady enough to use
};

/// Two blind listening tests (8 songs): inside Mixxx every song found with
/// 0.8 or more was right by ear; lower (0.2-0.45) included two wrong ones.
constexpr double kMinMargin = 0.6;
constexpr int kMinBeats = 64;

/// @param beatTimes the grid's beats (seconds), beat index 0 = its first line
Result find(const Features& features, const std::vector<double>& beatTimes);

/// The same beats counted from beat 1: the first `phase` beats are dropped.
std::vector<double> fromBeatOne(const std::vector<double>& beatTimes, int phase);

} // namespace downbeat
