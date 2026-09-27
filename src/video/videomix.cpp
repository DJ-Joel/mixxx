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

} // namespace videomix
