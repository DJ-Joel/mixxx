#include "video/videomix.h"

#include <algorithm>
#include <cmath>

namespace videomix {

QVector<double> weights(const QVector<DeckInput>& decks) {
    QVector<double> result(decks.size(), 0.0);
    const auto heard = [](const DeckInput& deck) {
        return std::clamp(deck.volume, 0.0, 1.0) * std::clamp(deck.xfaderGain, 0.0, 1.0);
    };
    double sum = 0.0;
    for (int i = 0; i < decks.size(); ++i) {
        if (decks[i].loaded && decks[i].playing) {
            result[i] = heard(decks[i]);
            sum += result[i];
        }
    }
    if (sum <= 0.0) {
        // Nothing is heard: keep showing what would be heard.
        for (int i = 0; i < decks.size(); ++i) {
            result[i] = decks[i].loaded ? heard(decks[i]) : 0.0;
            sum += result[i];
        }
    }
    if (sum <= 0.0) {
        return QVector<double>(decks.size(), 0.0);
    }
    for (double& w : result) {
        w /= sum;
    }
    return result;
}

QVector<double> layerOpacities(const QVector<double>& weightsInDrawOrder) {
    QVector<double> result(weightsInDrawOrder.size(), 0.0);
    double sum = 0.0;
    for (int i = 0; i < weightsInDrawOrder.size(); ++i) {
        const double w = std::max(0.0, weightsInDrawOrder[i]);
        if (w <= 0.0) {
            continue;
        }
        sum += w;
        // Blend so far = sum of (w_j * picture_j) / sum. Drawing this layer
        // with opacity w/sum keeps that true.
        result[i] = w / sum;
    }
    return result;
}

QRect fitRect(QSize image, QSize canvas) {
    if (image.isEmpty() || canvas.isEmpty()) {
        return {};
    }
    const QSize fitted = image.scaled(canvas, Qt::KeepAspectRatio);
    return QRect(QPoint((canvas.width() - fitted.width()) / 2,
                         (canvas.height() - fitted.height()) / 2),
            fitted);
}

int mainDeck(const QVector<double>& weights, int current) {
    int loudest = -1;
    for (int i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0.0 && (loudest < 0 || weights[i] > weights[loudest])) {
            loudest = i;
        }
    }
    if (loudest < 0) {
        return -1; // nothing is heard
    }
    if (current >= 0 && current < weights.size() && weights[current] > 0.0 &&
            weights[loudest] < weights[current] + kCutMargin) {
        return current; // not clearly louder: keep the picture
    }
    return loudest;
}

double titleOpacity(double seconds) {
    if (!(seconds >= 0.0) || seconds >= kTitleSeconds) {
        return 0.0;
    }
    if (seconds < kTitleFadeInSec) {
        return seconds / kTitleFadeInSec;
    }
    const double left = kTitleSeconds - seconds;
    if (left < kTitleFadeOutSec) {
        return left / kTitleFadeOutSec;
    }
    return 1.0;
}

double beatPulse(double secondsSinceBeat) {
    if (!(secondsSinceBeat >= 0.0) || secondsSinceBeat > 10.0 * kPulseDecaySec) {
        return 0.0;
    }
    return kPulseAmount * std::exp(-secondsSinceBeat / kPulseDecaySec);
}

namespace {

inline std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

void bgraToNv12(const std::uint8_t* bgra,
        int width,
        int height,
        int strideBytes,
        std::uint8_t* nv12) {
    // BT.709 TV range, multiplied by 256 (each row of weights sums to the
    // right total, so grey stays exactly grey).
    std::uint8_t* pY = nv12;
    std::uint8_t* pUV = nv12 + static_cast<std::size_t>(width) * height;
    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* row0 = bgra + static_cast<std::size_t>(y) * strideBytes;
        const std::uint8_t* row1 = row0 + strideBytes;
        std::uint8_t* y0 = pY + static_cast<std::size_t>(y) * width;
        std::uint8_t* y1 = y0 + width;
        std::uint8_t* uv = pUV + static_cast<std::size_t>(y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;
            const std::uint8_t* pixels[4] = {
                    row0 + x * 4, row0 + x * 4 + 4, row1 + x * 4, row1 + x * 4 + 4};
            std::uint8_t* outY[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int k = 0; k < 4; ++k) {
                const int b = pixels[k][0];
                const int g = pixels[k][1];
                const int r = pixels[k][2];
                *outY[k] = clampByte(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8));
                sumR += r;
                sumG += g;
                sumB += b;
            }
            // Average of the 2 x 2 block (sums are 4 x the colour).
            uv[x] = clampByte(128 + ((-26 * sumR - 86 * sumG + 112 * sumB + 512) >> 10));
            uv[x + 1] = clampByte(128 + ((112 * sumR - 102 * sumG - 10 * sumB + 512) >> 10));
        }
    }
}

void floatToPcm16(const float* in, std::size_t count, std::int16_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        const float value = std::clamp(in[i], -1.0f, 1.0f) * 32767.0f;
        out[i] = static_cast<std::int16_t>(std::lround(value));
    }
}

long long videoFramesBefore(double seconds, int fps) {
    if (seconds <= 0.0 || fps <= 0) {
        return 0;
    }
    // Tiny tolerance, so a time exactly on a picture does not count it.
    return static_cast<long long>(std::ceil(seconds * fps - 1e-9));
}

} // namespace videomix
