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

TEST(PhraseAlignTest, IncomingBeatKicksInAtTheBassSwap) {
    // Incoming beat kicks in at 64 s (4 phrases in); fade is 8 bars = 16 s.
    // Start at 56 s so the beat arrives at the middle of the fade (8 s in),
    // exactly when the bass swaps. The long intro before 56 s is skipped.
    const auto p = phrasealign::plan(k120, k120, 0.0, 200.0, 0.0, 8, 64.0);
    ASSERT_TRUE(p.has_value());
    EXPECT_DOUBLE_EQ(56.0, p->toStartSec);
    const double half = (p->fromFadeEndSec - p->fromFadeBeginSec) / 2.0;
    EXPECT_DOUBLE_EQ(64.0, p->toStartSec + half);
    // Body measured a little late (65.2 s): still the 64 s phrase.
    const auto p2 = phrasealign::plan(k120, k120, 0.0, 200.0, 0.0, 8, 65.2);
    ASSERT_TRUE(p2.has_value());
    EXPECT_DOUBLE_EQ(56.0, p2->toStartSec);
}

TEST(PhraseAlignTest, ShortIntroStartsAtTheBeginning) {
    // Beat kicks in at 6 s, before the swap point: start at the first beat.
    const auto p = phrasealign::plan(k120, k120, 0.0, 200.0, 0.0, 8, 6.0);
    ASSERT_TRUE(p.has_value());
    EXPECT_DOUBLE_EQ(0.0, p->toStartSec);
}

TEST(PhraseAlignTest, BeatOnABarThatIsNotAPhraseStart) {
    // Beat kicks in at 26 s = bar 14 (not a phrase start): the entry snaps
    // to that bar, and the track starts 4 bars (8 s) earlier, at 18 s.
    const auto p = phrasealign::plan(k120, k120, 0.0, 200.0, 0.0, 8, 26.4);
    ASSERT_TRUE(p.has_value());
    EXPECT_DOUBLE_EQ(18.0, p->toStartSec);
}

TEST(PhraseAlignTest, FadeEndsBeforeTheOutgoingFadeOut) {
    // The caller passes min(outro end, body end) as the limit: a song whose
    // own fade-out starts at 180 s must be mixed out before then.
    const auto p = phrasealign::plan(k120, k120, 0.0, 180.0, 0.0, 8);
    ASSERT_TRUE(p.has_value());
    EXPECT_LE(p->fromFadeEndSec, 180.0);
    EXPECT_DOUBLE_EQ(160.0, p->fromFadeBeginSec);
}

TEST(PhraseAlignTest, NothingStaysBeatLandsInTheMiddle) {
    // Real track: 111.68 BPM, first beat 0.272 s, beat kicks in at 34.66 s
    // (measured body start 34 s). With the intro start (0.28 s) as the
    // earliest start, the track starts 4 bars (16 beats) before the beat,
    // so the beat lands at the middle of an 8-bar fade.
    phrasealign::Grid ns;
    ns.firstBeatSec = 0.272;
    ns.beatSec = 60.0 / 111.68;
    const auto p = phrasealign::plan(k120, ns, 0.0, 250.0, 0.28, 8, 34.0);
    ASSERT_TRUE(p.has_value());
    const double beatAt = ns.beatTime(64);
    EXPECT_NEAR(34.66, beatAt, 0.01);
    EXPECT_NEAR(beatAt - 16 * ns.beatSec, p->toStartSec, 1e-9);
    // Planning again from the same intro start gives the same answer.
    const auto again = phrasealign::plan(k120, ns, 0.0, 250.0, 0.28, 8, 34.0);
    ASSERT_TRUE(again.has_value());
    EXPECT_DOUBLE_EQ(p->toStartSec, again->toStartSec);
}

TEST(PhraseAlignTest, MarkedBeatIsTrustedToTheBeat) {
    // Lack of Sense: 120.39 BPM, first beat 0.394 s. Measured body start
    // 15.0 s would snap to the bar at beat 28 (14.35 s). The DJ marked the
    // beat at 15.71 s: that is beat 30.7, so the nearest beat, 31 (15.84 s),
    // lands at the middle of the fade and the start is 16 beats earlier.
    phrasealign::Grid ls;
    ls.firstBeatSec = 0.394;
    ls.beatSec = 60.0 / 120.387;
    const auto measured = phrasealign::plan(k120, ls, 0.0, 250.0, 0.39, 8, 15.0);
    ASSERT_TRUE(measured.has_value());
    EXPECT_NEAR(ls.beatTime(28 - 16), measured->toStartSec, 1e-9);
    const auto marked = phrasealign::plan(k120, ls, 0.0, 250.0, 0.39, 8, 15.71, true);
    ASSERT_TRUE(marked.has_value());
    EXPECT_NEAR(ls.beatTime(31 - 16), marked->toStartSec, 1e-9);
}
