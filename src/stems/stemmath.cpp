#include "stems/stemmath.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace stems {

// ---------------------------------------------------------------------------
// Pieces and blending
// ---------------------------------------------------------------------------

std::vector<Piece> planPieces(std::int64_t length, int segment, double overlap) {
    std::vector<Piece> pieces;
    if (length <= 0 || segment <= 0) {
        return pieces;
    }
    const std::int64_t stride = std::max<std::int64_t>(
            1, static_cast<std::int64_t>((1.0 - overlap) * segment));
    for (std::int64_t offset = 0; offset < length; offset += stride) {
        Piece piece;
        piece.offset = offset;
        piece.size = std::min<std::int64_t>(segment, length - offset);
        // A short last piece is centred in the model input (like Demucs).
        piece.start = offset - (segment - piece.size) / 2;
        pieces.push_back(piece);
    }
    return pieces;
}

Blender::Blender(std::int64_t length, int segment)
        : m_length(length),
          m_segment(segment),
          m_weight(segment),
          m_sum(static_cast<std::size_t>(kStemCount) * 2 * segment, 0.0f),
          m_total(segment, 0.0f) {
    // Triangle: 1, 2, ... up to the middle and back down, scaled to 1.
    const int half = segment / 2;
    for (int i = 0; i < segment; ++i) {
        m_weight[i] = static_cast<float>(i < half ? i + 1 : segment - i);
    }
    const float largest = *std::max_element(m_weight.begin(), m_weight.end());
    for (float& weight : m_weight) {
        weight /= largest;
    }
}

void Blender::fillInput(const Piece& piece,
        const std::vector<float>& planar,
        std::vector<float>* pInput) const {
    pInput->assign(static_cast<std::size_t>(2) * m_segment, 0.0f);
    const std::int64_t from = std::max<std::int64_t>(piece.start, 0);
    const std::int64_t to = std::min<std::int64_t>(piece.start + m_segment, m_length);
    for (int channel = 0; channel < 2; ++channel) {
        const float* pSong = planar.data() + channel * m_length;
        float* pOut = pInput->data() + static_cast<std::size_t>(channel) * m_segment;
        for (std::int64_t frame = from; frame < to; ++frame) {
            pOut[frame - piece.start] = pSong[frame];
        }
    }
}

void Blender::add(const Piece& piece, const float* pOutput) {
    const std::int64_t lead = piece.offset - piece.start;
    const std::int64_t base = piece.offset - m_base;
    for (int stem = 0; stem < kStemCount; ++stem) {
        for (int channel = 0; channel < 2; ++channel) {
            const float* pIn = pOutput +
                    (static_cast<std::size_t>(stem) * 2 + channel) * m_segment + lead;
            float* pSum = m_sum.data() +
                    (static_cast<std::size_t>(stem) * 2 + channel) * m_segment + base;
            for (std::int64_t i = 0; i < piece.size; ++i) {
                pSum[i] += m_weight[i] * pIn[i];
            }
        }
    }
    for (std::int64_t i = 0; i < piece.size; ++i) {
        m_total[base + i] += m_weight[i];
    }
}

std::int64_t Blender::flush(std::int64_t upTo, float mean, float std, std::vector<float>* pOut) {
    upTo = std::clamp<std::int64_t>(upTo, m_base, m_length);
    const std::int64_t count = upTo - m_base;
    pOut->assign(static_cast<std::size_t>(kStemCount) * 2 * count, 0.0f);
    for (int part = 0; part < kStemCount * 2; ++part) {
        float* pSum = m_sum.data() + static_cast<std::size_t>(part) * m_segment;
        float* pDest = pOut->data() + static_cast<std::size_t>(part) * count;
        for (std::int64_t i = 0; i < count; ++i) {
            const float total = m_total[i];
            pDest[i] = (total > 0.0f ? pSum[i] / total : 0.0f) * std + mean;
        }
        // Move the unfinished rest to the front.
        std::copy(pSum + count, pSum + m_segment, pSum);
        std::fill(pSum + m_segment - count, pSum + m_segment, 0.0f);
    }
    std::copy(m_total.begin() + count, m_total.end(), m_total.begin());
    std::fill(m_total.end() - count, m_total.end(), 0.0f);
    m_base = upTo;
    return count;
}

void meanAndStd(const std::vector<float>& samples, float* pMean, float* pStd) {
    if (samples.empty()) {
        *pMean = 0.0f;
        *pStd = 1.0f;
        return;
    }
    double sum = 0.0;
    for (float sample : samples) {
        sum += sample;
    }
    const double mean = sum / samples.size();
    double squares = 0.0;
    for (float sample : samples) {
        squares += (sample - mean) * (sample - mean);
    }
    // Demucs uses the unbiased standard deviation.
    const double std = std::sqrt(squares / std::max<std::size_t>(1, samples.size() - 1));
    *pMean = static_cast<float>(mean);
    *pStd = static_cast<float>(std) + 1e-8f;
}

std::vector<float> toPlanar(const float* pInterleaved, std::int64_t frames) {
    std::vector<float> planar(static_cast<std::size_t>(frames) * 2);
    for (std::int64_t i = 0; i < frames; ++i) {
        planar[i] = pInterleaved[2 * i];
        planar[frames + i] = pInterleaved[2 * i + 1];
    }
    return planar;
}

void toInterleaved(const float* pPlanar, std::int64_t frames, float* pInterleaved) {
    for (std::int64_t i = 0; i < frames; ++i) {
        pInterleaved[2 * i] = pPlanar[i];
        pInterleaved[2 * i + 1] = pPlanar[frames + i];
    }
}

// ---------------------------------------------------------------------------
// Sample rate conversion
// ---------------------------------------------------------------------------

namespace {

constexpr int kZeroCrossings = 32;
constexpr double kKaiserBeta = 9.0;
constexpr double kPassband = 0.93; // of the lower Nyquist frequency
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) {
            break;
        }
    }
    return sum;
}

} // namespace

std::int64_t resampledLength(std::int64_t frames, int fromRate, int toRate) {
    if (fromRate <= 0 || toRate <= 0) {
        return 0;
    }
    return (frames * toRate + fromRate - 1) / fromRate;
}

std::vector<float> resample(const float* pIn, std::int64_t frames, int fromRate, int toRate) {
    if (fromRate == toRate) {
        return std::vector<float>(pIn, pIn + frames);
    }
    const std::int64_t divisor = std::gcd(fromRate, toRate);
    const std::int64_t up = toRate / divisor;  // output steps per ...
    const std::int64_t down = fromRate / divisor; // ... this many input steps
    // Cut-off in cycles per input sample.
    const double cutoff = 0.5 * std::min(1.0, static_cast<double>(toRate) / fromRate) * kPassband;
    const double halfWidth = kZeroCrossings / (2.0 * cutoff); // input samples
    const int taps = static_cast<int>(std::ceil(halfWidth)) * 2 + 1;
    const int reach = taps / 2;
    const double i0Beta = besselI0(kKaiserBeta);

    // One filter per output phase (the rates share a small common step).
    const bool useTable = up <= 4096;
    std::vector<float> table;
    auto filter = [&](double fraction, float* pTaps) {
        // Tap k sits at input sample (floor + k - reach); fraction = position
        // of the output between floor and floor + 1.
        double norm = 0.0;
        for (int k = 0; k < taps; ++k) {
            const double x = (k - reach) - fraction;
            double value = 0.0;
            if (std::abs(x) < halfWidth) {
                const double arg = 2.0 * cutoff * x;
                const double sinc = std::abs(arg) < 1e-12
                        ? 1.0
                        : std::sin(kPi * arg) / (kPi * arg);
                const double r = x / halfWidth;
                const double window = besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0Beta;
                value = 2.0 * cutoff * sinc * window;
            }
            pTaps[k] = static_cast<float>(value);
            norm += value;
        }
        // Exactly unity gain for steady signals.
        for (int k = 0; k < taps; ++k) {
            pTaps[k] = static_cast<float>(pTaps[k] / norm);
        }
    };
    if (useTable) {
        table.resize(static_cast<std::size_t>(up) * taps);
        for (std::int64_t phase = 0; phase < up; ++phase) {
            filter(static_cast<double>(phase) / up, table.data() + phase * taps);
        }
    }

    const std::int64_t outFrames = resampledLength(frames, fromRate, toRate);
    std::vector<float> out(outFrames);
    std::vector<float> scratch(taps);
    for (std::int64_t n = 0; n < outFrames; ++n) {
        // Input position n * down / up = whole + phase / up.
        const std::int64_t position = n * down;
        const std::int64_t whole = position / up;
        const std::int64_t phase = position % up;
        const float* pTaps;
        if (useTable) {
            pTaps = table.data() + phase * taps;
        } else {
            filter(static_cast<double>(phase) / up, scratch.data());
            pTaps = scratch.data();
        }
        double sum = 0.0;
        const std::int64_t first = whole - reach;
        const int kFrom = static_cast<int>(std::max<std::int64_t>(0, -first));
        const int kTo = static_cast<int>(std::min<std::int64_t>(taps, frames - first));
        for (int k = kFrom; k < kTo; ++k) {
            sum += pTaps[k] * pIn[first + k];
        }
        out[n] = static_cast<float>(sum);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The stem manifest (NI stem format, as Mixxx reads it)
// ---------------------------------------------------------------------------

QByteArray stemManifest() {
    const char* names[kStemCount] = {"Drums", "Bass", "Other", "Vocals"};
    const char* colors[kStemCount] = {"#FD4A4A", "#FFFF00", "#00E8E8", "#FF00FF"};
    QJsonArray parts;
    for (int i = 0; i < kStemCount; ++i) {
        parts.append(QJsonObject{{"name", names[i]}, {"color", colors[i]}});
    }
    const QJsonObject off{{"enabled", false}};
    QJsonObject manifest{
            {"version", 1},
            {"mastering_dsp", QJsonObject{{"compressor", off}, {"limiter", off}}},
            {"stems", parts},
    };
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

namespace {

quint32 readBoxSize(const QByteArray& data, qsizetype at) {
    return qFromBigEndian<quint32>(data.constData() + at);
}

QByteArray box(const char* type, const QByteArray& payload) {
    QByteArray result(8, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(8 + payload.size()), result.data());
    std::copy(type, type + 4, result.data() + 4);
    return result + payload;
}

} // namespace

QByteArray moovWithManifest(const QByteArray& moov, const QByteArray& manifest) {
    if (moov.size() < 8 || moov.mid(4, 4) != "moov" ||
            readBoxSize(moov, 0) != static_cast<quint32>(moov.size())) {
        return QByteArray();
    }
    const QByteArray stem = box("stem", manifest);
    // Look for udta among moov's children.
    qsizetype at = 8;
    while (at + 8 <= moov.size()) {
        const quint32 size = readBoxSize(moov, at);
        if (size < 8 || at + size > static_cast<quint32>(moov.size())) {
            return QByteArray(); // 64-bit or broken boxes: leave the file alone
        }
        if (moov.mid(at + 4, 4) == "udta") {
            QByteArray result = moov.left(at + size) + stem + moov.mid(at + size);
            qToBigEndian<quint32>(size + stem.size(), result.data() + at);
            qToBigEndian<quint32>(static_cast<quint32>(result.size()), result.data());
            return result;
        }
        at += size;
    }
    QByteArray result = moov + box("udta", stem);
    qToBigEndian<quint32>(static_cast<quint32>(result.size()), result.data());
    return result;
}

bool addStemManifestToFile(const QString& path, const QByteArray& manifest) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return false;
    }
    const qint64 fileSize = file.size();
    qint64 at = 0;
    qint64 moovAt = -1;
    qint64 moovSize = 0;
    while (at + 8 <= fileSize) {
        file.seek(at);
        const QByteArray header = file.read(16);
        if (header.size() < 8) {
            return false;
        }
        quint64 size = qFromBigEndian<quint32>(header.constData());
        if (size == 1 && header.size() == 16) {
            size = qFromBigEndian<quint64>(header.constData() + 8);
        } else if (size == 0) {
            size = fileSize - at; // to the end of the file
        }
        if (size < 8) {
            return false;
        }
        if (header.mid(4, 4) == "moov") {
            moovAt = at;
            moovSize = static_cast<qint64>(size);
        }
        at += static_cast<qint64>(size);
    }
    if (moovAt < 0 || moovAt + moovSize != fileSize) {
        return false; // moov must be the last box
    }
    file.seek(moovAt);
    const QByteArray moov = moovWithManifest(file.read(moovSize), manifest);
    if (moov.isEmpty()) {
        return false;
    }
    file.seek(moovAt);
    return file.write(moov) == moov.size();
}

} // namespace stems
