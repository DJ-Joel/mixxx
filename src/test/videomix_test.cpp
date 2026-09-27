#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "video/videomix.h"

using videomix::DeckInput;

namespace {

DeckInput deck(bool playing, double volume, double xfaderGain) {
    DeckInput d;
    d.loaded = true;
    d.playing = playing;
    d.volume = volume;
    d.xfaderGain = xfaderGain;
    return d;
}

} // namespace

TEST(VideoMixTest, PictureFollowsTheSound) {
    // Deck 1 fully heard, deck 2 cued but crossfader away from it.
    auto w = videomix::weights({deck(true, 1.0, 1.0), deck(false, 1.0, 0.0)});
    EXPECT_DOUBLE_EQ(1.0, w[0]);
    EXPECT_DOUBLE_EQ(0.0, w[1]);
    // Halfway through a crossfade: both heard the same.
    w = videomix::weights({deck(true, 1.0, 0.7), deck(true, 1.0, 0.7)});
    EXPECT_DOUBLE_EQ(0.5, w[0]);
    EXPECT_DOUBLE_EQ(0.5, w[1]);
    // A quieter volume fader counts for less.
    w = videomix::weights({deck(true, 1.0, 1.0), deck(true, 0.25, 1.0)});
    EXPECT_DOUBLE_EQ(0.8, w[0]);
    EXPECT_DOUBLE_EQ(0.2, w[1]);
}

TEST(VideoMixTest, PausedDeckIsNotShownWhileAnotherPlays) {
    // Deck 2 paused with its faders up: it is silent, so not shown.
    const auto w = videomix::weights({deck(true, 1.0, 1.0), deck(false, 1.0, 1.0)});
    EXPECT_DOUBLE_EQ(1.0, w[0]);
    EXPECT_DOUBLE_EQ(0.0, w[1]);
}

TEST(VideoMixTest, EverythingPausedKeepsThePicture) {
    const auto w = videomix::weights({deck(false, 1.0, 1.0), deck(false, 1.0, 0.0)});
    EXPECT_DOUBLE_EQ(1.0, w[0]);
    EXPECT_DOUBLE_EQ(0.0, w[1]);
}

TEST(VideoMixTest, NothingLoadedOrAllFadersDownShowsNothing) {
    DeckInput empty;
    auto w = videomix::weights({empty, empty});
    EXPECT_DOUBLE_EQ(0.0, w[0]);
    EXPECT_DOUBLE_EQ(0.0, w[1]);
    w = videomix::weights({deck(true, 0.0, 1.0), deck(true, 1.0, 0.0)});
    EXPECT_DOUBLE_EQ(0.0, w[0]);
    EXPECT_DOUBLE_EQ(0.0, w[1]);
}

TEST(VideoMixTest, LayersBlendByWeight) {
    // 70/30: first drawn fully, second on top at 30%.
    auto o = videomix::layerOpacities({0.7, 0.3});
    EXPECT_DOUBLE_EQ(1.0, o[0]);
    EXPECT_DOUBLE_EQ(0.3, o[1]);
    // Three equal layers: 1, 1/2, 1/3 gives a third of each.
    o = videomix::layerOpacities({1.0 / 3, 1.0 / 3, 1.0 / 3});
    EXPECT_DOUBLE_EQ(1.0, o[0]);
    EXPECT_NEAR(0.5, o[1], 1e-12);
    EXPECT_NEAR(1.0 / 3, o[2], 1e-12);
    // Zero weights are skipped, and do not dilute the others.
    o = videomix::layerOpacities({0.0, 1.0, 0.0});
    EXPECT_DOUBLE_EQ(0.0, o[0]);
    EXPECT_DOUBLE_EQ(1.0, o[1]);
    EXPECT_DOUBLE_EQ(0.0, o[2]);
}

TEST(VideoMixTest, FitKeepsTheShape) {
    // 16:9 video on a 16:9 screen fills it.
    EXPECT_EQ(QRect(0, 0, 1920, 1080), videomix::fitRect(QSize(1280, 720), QSize(1920, 1080)));
    // 4:3 video: black bars left and right.
    EXPECT_EQ(QRect(240, 0, 1440, 1080), videomix::fitRect(QSize(640, 480), QSize(1920, 1080)));
    // Square cover art in a wide screen.
    EXPECT_EQ(QRect(420, 0, 1080, 1080), videomix::fitRect(QSize(500, 500), QSize(1920, 1080)));
    EXPECT_TRUE(videomix::fitRect(QSize(), QSize(1920, 1080)).isNull());
}

TEST(VideoMixTest, CutShowsTheLoudestDeckWithoutFlicker) {
    // Deck 0 alone.
    EXPECT_EQ(0, videomix::mainDeck({1.0, 0.0}, -1));
    // During a fade: deck 0 stays until deck 1 is clearly louder.
    EXPECT_EQ(0, videomix::mainDeck({0.52, 0.48}, 0));
    EXPECT_EQ(0, videomix::mainDeck({0.48, 0.52}, 0));
    EXPECT_EQ(1, videomix::mainDeck({0.4, 0.6}, 0));
    // Then it stays on deck 1.
    EXPECT_EQ(1, videomix::mainDeck({0.52, 0.48}, 1));
    // The shown deck went silent: the other one at once.
    EXPECT_EQ(1, videomix::mainDeck({0.0, 0.3}, 0));
    // Nothing heard.
    EXPECT_EQ(-1, videomix::mainDeck({0.0, 0.0}, 1));
    EXPECT_EQ(-1, videomix::mainDeck({}, -1));
}

TEST(VideoMixTest, TitleFadesInAndOut) {
    EXPECT_DOUBLE_EQ(0.0, videomix::titleOpacity(-1.0));
    EXPECT_DOUBLE_EQ(0.0, videomix::titleOpacity(0.0));
    EXPECT_DOUBLE_EQ(0.5, videomix::titleOpacity(videomix::kTitleFadeInSec / 2));
    EXPECT_DOUBLE_EQ(1.0, videomix::titleOpacity(3.0));
    EXPECT_NEAR(0.5, videomix::titleOpacity(videomix::kTitleSeconds - 0.5), 1e-9);
    EXPECT_DOUBLE_EQ(0.0, videomix::titleOpacity(videomix::kTitleSeconds));
    EXPECT_DOUBLE_EQ(0.0, videomix::titleOpacity(100.0));
}

TEST(VideoMixTest, PulseOnTheBeatSettlesBack) {
    EXPECT_DOUBLE_EQ(videomix::kPulseAmount, videomix::beatPulse(0.0));
    EXPECT_LT(videomix::beatPulse(0.2), videomix::beatPulse(0.1));
    EXPECT_LT(videomix::beatPulse(0.5), 0.1 * videomix::kPulseAmount);
    EXPECT_DOUBLE_EQ(0.0, videomix::beatPulse(5.0));
    EXPECT_DOUBLE_EQ(0.0, videomix::beatPulse(-1.0));
}

TEST(VideoMixTest, RecordingColoursAreRight) {
    // 4 x 2 picture: left 2 x 2 block black, right 2 x 2 block white.
    const int width = 4;
    const int height = 2;
    std::vector<std::uint8_t> bgra(width * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 2; x < 4; ++x) {
            for (int c = 0; c < 4; ++c) {
                bgra[(y * width + x) * 4 + c] = 255;
            }
        }
    }
    std::vector<std::uint8_t> nv12(width * height * 3 / 2, 0);
    videomix::bgraToNv12(bgra.data(), width, height, width * 4, nv12.data());
    // Brightness: black 16, white 235 (TV range).
    EXPECT_EQ(16, nv12[0]);
    EXPECT_EQ(16, nv12[width + 1]);
    EXPECT_EQ(235, nv12[2]);
    EXPECT_EQ(235, nv12[width + 3]);
    // Colour: none for black and white (128).
    for (int i = width * height; i < width * height * 3 / 2; ++i) {
        EXPECT_EQ(128, nv12[i]);
    }
    // Pure red: U low, V high.
    std::vector<std::uint8_t> red(2 * 2 * 4, 0);
    for (int i = 0; i < 4; ++i) {
        red[i * 4 + 2] = 255;
    }
    std::vector<std::uint8_t> redOut(6, 0);
    videomix::bgraToNv12(red.data(), 2, 2, 8, redOut.data());
    EXPECT_NEAR(63, redOut[0], 1);
    EXPECT_NEAR(102, redOut[4], 1);
    EXPECT_NEAR(240, redOut[5], 1);
}

TEST(VideoMixTest, RecordingSoundAndPictureTimes) {
    const float in[5] = {0.0f, 1.0f, -1.0f, 2.0f, 0.5f};
    std::int16_t out[5] = {};
    videomix::floatToPcm16(in, 5, out);
    EXPECT_EQ(0, out[0]);
    EXPECT_EQ(32767, out[1]);
    EXPECT_EQ(-32767, out[2]);
    EXPECT_EQ(32767, out[3]); // clipped
    EXPECT_EQ(16384, out[4]);
    // 30 pictures a second: pictures 0..29 start in the first second.
    EXPECT_EQ(0, videomix::videoFramesBefore(0.0, 30));
    EXPECT_EQ(1, videomix::videoFramesBefore(0.01, 30));
    EXPECT_EQ(30, videomix::videoFramesBefore(1.0, 30));
    EXPECT_EQ(31, videomix::videoFramesBefore(1.001, 30));
    EXPECT_EQ(0, videomix::videoFramesBefore(-2.0, 30));
}
