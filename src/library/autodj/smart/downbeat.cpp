#include "library/autodj/smart/downbeat.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace downbeat {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// In-place radix-2 FFT (size a power of two).
void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const std::complex<float> step(static_cast<float>(std::cos(angle)),
                static_cast<float>(std::sin(angle)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

std::vector<int> pitchClasses(double rate, double lowHz, double highHz) {
    std::vector<int> pitch(Features::kFftSize / 2 + 1, -1);
    for (std::size_t k = 1; k < pitch.size(); ++k) {
        const double f = static_cast<double>(k) * rate / Features::kFftSize;
        if (f >= lowHz && f <= highHz) {
            const int note = static_cast<int>(std::lround(12.0 * std::log2(f / 440.0)));
            pitch[k] = ((note % 12) + 12) % 12;
        }
    }
    return pitch;
}

double cosineDistance(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (int i = 0; i < 12; ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    return 1.0 - dot / (std::sqrt(na) * std::sqrt(nb) + 1e-9);
}

/// How far each value is from the average, in spreads. `minSpread`: a
/// spread smaller than this is not a real difference (a song with no bass
/// or chord changes must not get a "clear" beat 1 from rounding noise).
std::vector<double> zScores(const std::vector<double>& v, double minSpread) {
    if (v.empty()) {
        return v;
    }
    const double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    double var = 0.0;
    for (double x : v) {
        var += (x - mean) * (x - mean);
    }
    const double sd = std::max(std::sqrt(var / static_cast<double>(v.size())), minSpread) + 1e-9;
    std::vector<double> z(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        z[i] = (v[i] - mean) / sd;
    }
    return z;
}

} // namespace

Features::Features(double sampleRate) {
    m_decimate = std::max(1, static_cast<int>(std::lround(sampleRate / kTargetRate)));
    m_rate = sampleRate / m_decimate;
    m_ring.assign(kFftSize, 0.0f);
    m_window.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i) {
        m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / (kFftSize - 1)));
    }
    m_bassPitch = pitchClasses(m_rate, 35.0, 200.0);
    m_harmonyPitch = pitchClasses(m_rate, 200.0, 2000.0);
}

double Features::frameSec() const {
    return kHop / m_rate;
}

double Features::frameTime(int frame) const {
    return (static_cast<double>(frame) * kHop + kFftSize / 2.0) / m_rate;
}

void Features::process(const float* pMono, int count) {
    for (int i = 0; i < count; ++i) {
        m_sum += pMono[i];
        if (++m_summed < m_decimate) {
            continue;
        }
        const float sample = static_cast<float>(m_sum / m_summed);
        m_sum = 0.0;
        m_summed = 0;
        m_ring[m_pos] = sample;
        m_pos = (m_pos + 1) % kFftSize; // now the oldest sample
        // Frame i covers samples i*kHop .. i*kHop + kFftSize.
        ++m_filled;
        if (m_filled >= kFftSize && (m_filled - kFftSize) % kHop == 0) {
            frame();
        }
    }
}

void Features::frame() {
    std::vector<std::complex<float>> spectrum(kFftSize);
    for (int i = 0; i < kFftSize; ++i) {
        spectrum[i] = std::complex<float>(m_ring[(m_pos + i) % kFftSize] * m_window[i], 0.0f);
    }
    fft(spectrum);
    std::array<float, 12> b{};
    std::array<float, 12> h{};
    const int bins = kFftSize / 2 + 1;
    std::vector<float> logMag(bins);
    double flux = 0.0;
    for (int k = 0; k < bins; ++k) {
        const float mag = std::abs(spectrum[k]);
        if (m_bassPitch[k] >= 0) {
            b[m_bassPitch[k]] += mag;
        }
        if (m_harmonyPitch[k] >= 0) {
            h[m_harmonyPitch[k]] += mag;
        }
        logMag[k] = std::log1p(mag);
        if (!m_lastLog.empty()) {
            flux += std::max(0.0f, logMag[k] - m_lastLog[k]);
        }
    }
    m_lastLog.swap(logMag);
    bass.push_back(b);
    harmony.push_back(h);
    m_flux.push_back(static_cast<float>(flux));
}

namespace {

Result findAt(const Features& features, const std::vector<double>& beatTimes) {
    Result result;
    const int frames = features.frameCount();
    const int beats = static_cast<int>(beatTimes.size());
    if (frames < 16 || beats < kMinBeats) {
        return result;
    }
    // First frame at or after each beat.
    std::vector<int> first(beats);
    int f = 0;
    for (int i = 0; i < beats; ++i) {
        while (f < frames && features.frameTime(f) < beatTimes[i]) {
            ++f;
        }
        first[i] = f;
    }
    const int n = beats - 1;
    std::vector<bool> valid(n, false);
    std::vector<std::array<float, 12>> bass(n);
    std::vector<std::array<float, 12>> harmony(n);
    std::vector<double> accent(n, 0.0);
    const int reach = std::max(1, static_cast<int>(0.05 / features.frameSec()));
    for (int i = 0; i < n; ++i) {
        const int a = first[i];
        const int z = first[i + 1];
        if (z <= a + 1 || z >= frames) {
            continue;
        }
        valid[i] = true;
        std::array<float, 12> bs{};
        std::array<float, 12> hs{};
        for (int k = a; k < z; ++k) {
            for (int p = 0; p < 12; ++p) {
                bs[p] += features.bass[k][p];
                hs[p] += features.harmony[k][p];
            }
        }
        bass[i] = bs;
        harmony[i] = hs;
        float peak = 0.0f;
        for (int k = std::max(0, a - reach); k < std::min(frames, a + reach); ++k) {
            peak = std::max(peak, features.flux()[k]);
        }
        accent[i] = peak;
    }
    std::vector<double> bassChange(n, 0.0);
    std::vector<double> chordChange(n, 0.0);
    for (int i = 1; i < n; ++i) {
        if (valid[i] && valid[i - 1]) {
            bassChange[i] = cosineDistance(bass[i], bass[i - 1]);
            chordChange[i] = cosineDistance(harmony[i], harmony[i - 1]);
        }
    }
    // Changes are 0..1 (0 = the same notes); hits: at least 5% of their
    // average level must differ to count.
    const auto zb = zScores(bassChange, 0.02);
    const auto zc = zScores(chordChange, 0.02);
    const double meanAccent =
            std::accumulate(accent.begin(), accent.end(), 0.0) / std::max(1, n);
    const auto za = zScores(accent, 0.05 * meanAccent);
    std::vector<double> score(n);
    for (int i = 0; i < n; ++i) {
        score[i] = zb[i] + zc[i] + 0.5 * za[i];
    }
    auto phaseScores = [&score](int from, int to) {
        std::array<double, 4> sum{};
        std::array<int, 4> count{};
        for (int i = from; i < to; ++i) {
            sum[i % 4] += score[i];
            ++count[i % 4];
        }
        for (int p = 0; p < 4; ++p) {
            sum[p] = count[p] > 0 ? sum[p] / count[p] : 0.0;
        }
        return sum;
    };
    auto best = [](const std::array<double, 4>& s) {
        return static_cast<int>(std::max_element(s.begin(), s.end()) - s.begin());
    };
    const auto all = phaseScores(1, n);
    const auto firstHalf = phaseScores(1, n / 2);
    const auto secondHalf = phaseScores(n / 2, n);
    result.phase = best(all);
    std::array<double, 4> sorted = all;
    std::sort(sorted.begin(), sorted.end(), std::greater<double>());
    result.margin = sorted[0] - sorted[1];
    result.halvesAgree = best(firstHalf) == result.phase && best(secondHalf) == result.phase;
    result.sure = result.margin >= kMinMargin && result.halvesAgree;
    return result;
}

} // namespace

Result find(const Features& features, const std::vector<double>& beatTimes) {
    Result result = findAt(features, beatTimes);
    if (!result.sure) {
        return result;
    }
    // The answer must not hang on the exact timing: with the beat lines a
    // little earlier or later (as a slightly early or late grid would be) it
    // must stay the same. A test on real songs: moved 0.1-0.15 s, one song
    // gave a clear but wrong beat 1; this check catches that.
    for (double shift : {-0.06, -0.03, 0.03}) {
        std::vector<double> moved(beatTimes);
        for (double& t : moved) {
            t += shift;
        }
        const Result other = findAt(features, moved);
        if (other.phase != result.phase || other.margin < kMinMargin / 2.0) {
            result.sure = false;
            result.steady = false;
            return result;
        }
    }
    result.steady = true;
    return result;
}

std::vector<double> fromBeatOne(const std::vector<double>& beatTimes, int phase) {
    if (phase <= 0 || phase >= static_cast<int>(beatTimes.size())) {
        return beatTimes;
    }
    return std::vector<double>(beatTimes.begin() + phase, beatTimes.end());
}

} // namespace downbeat
