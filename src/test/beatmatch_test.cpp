#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "library/autodj/smart/beatmatch.h"

TEST(BeatmatchTest, MatchesCloseTempos) {
    // 120 BPM track under a 124 BPM track plays 3.3% faster.
    const auto r = beatmatch::matchRatio(124.0, 120.0, 5.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(124.0 / 120.0, *r, 1e-9);
    EXPECT_NEAR(1.0, *beatmatch::matchRatio(128.0, 128.0, 5.0), 1e-9);
}

TEST(BeatmatchTest, RefusesTooBigAChange) {
    // The Promise (118.1) -> Rock The Casbah (129.5): about 9%.
    EXPECT_FALSE(beatmatch::matchRatio(118.1, 129.5, 5.0).has_value());
    EXPECT_FALSE(beatmatch::matchRatio(0.0, 120.0, 5.0).has_value());
    EXPECT_FALSE(beatmatch::matchRatio(120.0, 0.0, 5.0).has_value());
}

TEST(BeatmatchTest, UsesHalfAndDoubleTime) {
    // Cupid Shuffle (71.8) under Smalltown Boy at 144 BPM: plays at double
    // time, so the ratio is small, not ~2.
    const auto r = beatmatch::matchRatio(144.0, 71.8, 5.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(144.0 / (2.0 * 71.8), *r, 1e-9);
    const auto r2 = beatmatch::matchRatio(71.8, 144.0, 5.0);
    ASSERT_TRUE(r2.has_value());
    EXPECT_NEAR(2.0 * 71.8 / 144.0, *r2, 1e-9);
}

TEST(BeatmatchTest, GlidesBackToOwnTempo) {
    EXPECT_DOUBLE_EQ(1.04, beatmatch::glideRatio(1.04, 0.0, 30.0));
    EXPECT_NEAR(1.02, beatmatch::glideRatio(1.04, 15.0, 30.0), 1e-9);
    EXPECT_DOUBLE_EQ(1.0, beatmatch::glideRatio(1.04, 30.0, 30.0));
    EXPECT_DOUBLE_EQ(1.0, beatmatch::glideRatio(0.97, 99.0, 30.0));
    // Speed of change: at most 5% over 30 s = 0.17% per second.
    const double perSecond = std::abs(beatmatch::glideRatio(1.05, 1.0, 30.0) - 1.05);
    EXPECT_LT(perSecond, 0.002);
}

TEST(BeatmatchTest, BassSwapsHardAtTheMiddle) {
    EXPECT_TRUE(beatmatch::bassSwap(0.0).toLowKilled);
    EXPECT_FALSE(beatmatch::bassSwap(0.0).fromLowKilled);
    EXPECT_TRUE(beatmatch::bassSwap(0.49).toLowKilled);
    EXPECT_FALSE(beatmatch::bassSwap(0.5).toLowKilled);
    EXPECT_TRUE(beatmatch::bassSwap(0.5).fromLowKilled);
    EXPECT_TRUE(beatmatch::bassSwap(1.0).fromLowKilled);
    // Never both basses at once, never both cut.
    for (double p = 0.0; p <= 1.0; p += 0.05) {
        const auto s = beatmatch::bassSwap(p);
        EXPECT_NE(s.fromLowKilled, s.toLowKilled) << p;
    }
}

TEST(BeatmatchTest, EqBlendsMidsAndHighsGradually) {
    using beatmatch::eqBlend;
    using beatmatch::kEqBlendFloor;
    // Start: the outgoing track as the DJ set it, the incoming one held back.
    EXPECT_DOUBLE_EQ(1.0, eqBlend(0.0).fromMidHigh);
    EXPECT_DOUBLE_EQ(kEqBlendFloor, eqBlend(0.0).toMidHigh);
    // Middle: both at full.
    EXPECT_DOUBLE_EQ(1.0, eqBlend(0.5).fromMidHigh);
    EXPECT_DOUBLE_EQ(1.0, eqBlend(0.5).toMidHigh);
    // End: the outgoing track held back, the incoming one at full.
    EXPECT_DOUBLE_EQ(kEqBlendFloor, eqBlend(1.0).fromMidHigh);
    EXPECT_DOUBLE_EQ(1.0, eqBlend(1.0).toMidHigh);
    // Gradual, never a jump, never outside floor..1.
    double lastFrom = 1.0;
    double lastTo = 0.0;
    for (int i = 0; i <= 100; ++i) {
        const auto e = eqBlend(i / 100.0);
        EXPECT_LE(e.fromMidHigh, lastFrom);
        EXPECT_GE(e.toMidHigh, lastTo);
        EXPECT_LT(lastFrom - e.fromMidHigh, 0.02);
        EXPECT_LT(e.toMidHigh - lastTo, i == 0 ? 1.0 : 0.02);
        EXPECT_GE(e.fromMidHigh, kEqBlendFloor);
        EXPECT_LE(e.toMidHigh, 1.0);
        lastFrom = e.fromMidHigh;
        lastTo = e.toMidHigh;
    }
    // Out-of-range progress is clamped.
    EXPECT_DOUBLE_EQ(kEqBlendFloor, eqBlend(-1.0).toMidHigh);
    EXPECT_DOUBLE_EQ(kEqBlendFloor, eqBlend(2.0).fromMidHigh);
}

TEST(BeatmatchTest, BeatLockKeepsTheSameBeatLength) {
    // Outgoing beat 0.48 s long right now, incoming beat 0.5 s at its own
    // speed: play the incoming 0.5 / 0.48 faster. In line: no nudge.
    double slip = 1.0;
    EXPECT_NEAR(0.5 / 0.48, beatmatch::followRatio(100.25, 0.48, 7.25, 0.5, &slip), 1e-9);
    EXPECT_NEAR(0.0, slip, 1e-9);
    // A tiny slip is left alone.
    EXPECT_NEAR(0.5 / 0.48, beatmatch::followRatio(100.255, 0.48, 7.25, 0.5), 1e-9);
}

TEST(BeatmatchTest, BeatLockPullsASlippedBeatBack) {
    // The incoming beat is 0.1 beat late: a bit faster (2.5 %, capped at 2 %).
    double slip = 0.0;
    const double faster = beatmatch::followRatio(100.1, 0.5, 7.0, 0.5, &slip);
    EXPECT_NEAR(0.1, slip, 1e-9);
    EXPECT_NEAR(1.0 + beatmatch::kMaxLockNudge, faster, 1e-9);
    // 0.04 beat early: 1 % slower.
    EXPECT_NEAR(0.99, beatmatch::followRatio(100.96, 0.5, 8.0, 0.5), 1e-9);
    // Beat numbers far apart do not matter, only where in the beat.
    EXPECT_NEAR(0.99, beatmatch::followRatio(5.96, 0.5, 300.0, 0.5), 1e-9);
    // Unknown beat lengths: leave the speed alone.
    EXPECT_DOUBLE_EQ(1.0, beatmatch::followRatio(1.0, 0.0, 1.0, 0.5));
}

TEST(BeatmatchTest, BeatLockFollowsADrummerWhoSpeedsUp) {
    // Simulate 60 s of a mix, in 20 ms steps (how often Auto DJ updates).
    // Outgoing: a live drummer, 120 BPM speeding up to about 126 BPM.
    // Incoming: steady 122 BPM. Starts in line.
    const auto fromBeatLen = [](double t) { return 0.5 / (1.0 + 0.0008 * t); };
    double fromBeat = 0.0;
    double toBeat = 0.0;
    const double toLen = 60.0 / 122.0;
    double worst = 0.0;
    for (double t = 0.0; t < 60.0; t += 0.02) {
        // The beat length Auto DJ measures is an average over a few beats,
        // so it lags about 2 s behind.
        const double measured = fromBeatLen(std::max(0.0, t - 2.0));
        const double ratio = beatmatch::followRatio(fromBeat, measured, toBeat, toLen);
        fromBeat += 0.02 / fromBeatLen(t);
        toBeat += 0.02 * ratio / toLen;
        double slip = fromBeat - toBeat;
        slip -= std::round(slip);
        worst = std::max(worst, std::fabs(slip));
    }
    // Never more than 2% of a beat apart (10 ms): nobody hears that.
    EXPECT_LT(worst, 0.02);
}
