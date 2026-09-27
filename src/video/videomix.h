#pragma once

#include <QRect>
#include <QSize>
#include <QVector>
#include <cstddef>
#include <cstdint>

/// Auto DJ 2.0 plus Video Mixing: how the decks' pictures are mixed and
/// cut, titles and beat pulses. Pure logic, so it
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

/// "Cut on the beat": the deck whose picture is shown alone (index), or -1
/// when nothing is heard. The loudest deck, but the `current` one is kept
/// until another is clearly louder (by kCutMargin), so the picture does not
/// flicker when two decks are about equal. During an Auto DJ fade the
/// crossfader passes the middle exactly at the bass swap, so the picture
/// cuts where the new song takes over.
constexpr double kCutMargin = 0.1;
int mainDeck(const QVector<double>& weights, int current);

/// How long a cut may wait for the new deck's next beat (seconds). A cut
/// normally lands on a beat of the new song; after this long it happens
/// anyway (e.g. a song with no beat grid).
constexpr double kCutWaitSec = 1.0;

/// Song titles: "Artist - Title" is shown for this long when a new song
/// takes over the screen, fading in over kTitleFadeInSec and out over
/// kTitleFadeOutSec.
constexpr double kTitleSeconds = 7.0;
constexpr double kTitleFadeInSec = 0.5;
constexpr double kTitleFadeOutSec = 1.0;
/// Opacity (0..1) of the song title `seconds` after it appeared.
double titleOpacity(double seconds);

/// Moving pictures for songs without video: the cover art grows a little on
/// every beat and settles back, like a speaker cone. Extra size (0 ..
/// kPulseAmount, e.g. 0.04 = 4% bigger) `secondsSinceBeat` after a beat.
constexpr double kPulseAmount = 0.04;
constexpr double kPulseDecaySec = 0.15;
double beatPulse(double secondsSinceBeat);

// ---- Video recording ----

/// Converts a picture (Qt's 32-bit "B, G, R, A" bytes, `strideBytes` per
/// row) to NV12, the format video encoders take: a full-size brightness
/// (Y) plane followed by a half-size colour plane (U and V side by side).
/// HD colours (BT.709), TV range (black = 16, white = 235). Width and
/// height must be even. `nv12` must hold width * height * 3 / 2 bytes.
void bgraToNv12(const std::uint8_t* bgra,
        int width,
        int height,
        int strideBytes,
        std::uint8_t* nv12);

/// Mixxx's sound samples (-1..1) to 16-bit samples, clipped.
void floatToPcm16(const float* in, std::size_t count, std::int16_t* out);

/// A recording has one picture every 1/fps seconds. The number of those
/// pictures that start before `seconds` (so picture number n starts at
/// n / fps).
long long videoFramesBefore(double seconds, int fps);

} // namespace videomix
