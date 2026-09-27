#include <gtest/gtest.h>

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
