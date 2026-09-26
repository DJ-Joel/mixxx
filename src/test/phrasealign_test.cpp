#include <gtest/gtest.h>

#include <cmath>

#include "library/autodj/smart/phrasealign.h"

using phrasealign::Grid;

namespace {
// 120 BPM: one beat = 0.5 s, one bar = 2 s, one 8-bar phrase = 16 s.
const Grid k120{0.0, 0.5};
} // namespace

TEST(PhraseAlignTest, BarsAreWholePhrases) {
    EXPECT_EQ(8, phrasealign::barsForSeconds(16.0, 0.5));  // 8 bars = 16 s
    EXPECT_EQ(8, phrasealign::barsForSeconds(5.0, 0.5));   // at least 1 phrase
    EXPECT_EQ(16, phrasealign::barsForSeconds(30.0, 0.5)); // ~15 bars -> 16
    EXPECT_EQ(8, phrasealign::barsForSeconds(16.0, 0.0));  // no grid: default
}

TEST(PhraseAlignTest, FadeStartsOnLastPhraseThatFits) {
    // Track ends (last sound) at 200 s. An 8-bar fade (16 s) must start on a
    // phrase start (multiple of 16 s) and end by 200 s: 176 -> 192.
    const auto p = phrasealign::plan(k120, k120, 0.0, 200.0, 0.0, 8);
    ASSERT_TRUE(p.has_value());
    EXPECT_DOUBLE_EQ(176.0, p->fromFadeBeginSec);
    EXPECT_DOUBLE_EQ(192.0, p->fromFadeEndSec);
    EXPECT_DOUBLE_EQ(0.0, p->toStartSec);
    EXPECT_EQ(8, p->bars);
}

TEST(PhraseAlignTest, FollowsTheGridOffset) {
    // Grid starts 0.3 s in; phrases are 0.3 + 16 k.
    const Grid from{0.3, 0.5};
    const auto p = phrasealign::plan(from, k120, 0.0, 200.0, 0.0, 8);
    ASSERT_TRUE(p.has_value());
    EXPECT_NEAR(176.3, p->fromFadeBeginSec, 1e-9);
    EXPECT_NEAR(192.3, p->fromFadeEndSec, 1e-9);
}

TEST(PhraseAlignTest, IncomingStartsOnAPhraseStart) {
    // Incoming grid starts at 0.5 s; first sound at 0.52 s (just after the
    // first beat) -> start at the first beat, not a whole phrase later.
    const Grid to{0.5, 0.5};
    const auto p = phrasealign::plan(k120, to, 0.0, 200.0, 0.52, 8);
    ASSERT_TRUE(p.has_value());
    EXPECT_NEAR(0.5, p->toStartSec, 1e-9);
    // Intro marked at 20 s: phrases start at 0.5, 16.5, 32.5 s -> 32.5.
    const auto p2 = phrasealign::plan(k120, to, 0.0, 200.0, 20.0, 8);
    ASSERT_TRUE(p2.has_value());
    EXPECT_NEAR(32.5, p2->toStartSec, 1e-9);
}

TEST(PhraseAlignTest, GivesUpWhenNothingFits) {
    // Already past the last phrase that fits.
    EXPECT_FALSE(phrasealign::plan(k120, k120, 180.0, 200.0, 0.0, 8).has_value());
    // Track shorter than the fade.
    EXPECT_FALSE(phrasealign::plan(k120, k120, 0.0, 10.0, 0.0, 8).has_value());
    // No grid.
    EXPECT_FALSE(phrasealign::plan(Grid{}, k120, 0.0, 200.0, 0.0, 8).has_value());
}

TEST(PhraseAlignTest, BassSwapLandsOnABarLine) {
    // The fade is a whole number of bars, so its middle is a bar line.
    const auto p = phrasealign::plan(k120, k120, 0.0, 200.0, 0.0, 8);
    ASSERT_TRUE(p.has_value());
    const double middle = (p->fromFadeBeginSec + p->fromFadeEndSec) / 2.0;
    const double bars = middle / (4 * 0.5);
    EXPECT_DOUBLE_EQ(bars, std::round(bars));
}
