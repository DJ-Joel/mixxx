#pragma once

#include <QRect>
#include <QSize>
#include <QVector>

/// Auto DJ 2.0 video: how the decks' pictures are mixed. Pure logic, so it
/// can be unit-tested.
namespace videomix {

/// What the video mixer knows about one deck.
struct DeckInput {
    bool loaded = false;
    bool playing = false;
    double volume = 0.0;     ///< the deck's volume fader, 0..1
    double xfaderGain = 0.0; ///< its gain from the crossfader, 0..1
};

/// How much of each deck's picture to show (0..1, summing to 1, or all 0
/// for "show nothing"). The picture follows the sound: a deck counts as
/// much as it is heard (volume x crossfader), and a paused deck is silent.
/// When nothing is heard at all (everything paused), the decks are
/// weighted as if playing, so the screen keeps the paused picture instead
/// of going black.
QVector<double> weights(const QVector<DeckInput>& decks);

/// Draw order and opacity for layering pictures with the given weights:
/// the first picture is drawn fully, each next one on top with the
/// returned opacity, so the result is the weighted blend. Pictures with
/// weight 0 get opacity 0 (skip them).
QVector<double> layerOpacities(const QVector<double>& weightsInDrawOrder);

/// The largest rectangle with the image's shape that fits the canvas,
/// centred (black bars top/bottom or left/right).
QRect fitRect(QSize image, QSize canvas);

} // namespace videomix
