#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "util/performancetimer.h"
#include "waveform/visualplayposition.h"

// Auto DJ 2.0 plus Video Mixing: the timing of the beat lock and the bass
// swap. They compare two decks, so both positions must be for the same
// moment. The "playposition" control is only updated 15 times a second,
// each deck at its own moment (up to 67 ms apart); the engine position
// used now is stamped with the audio buffer time shared by all decks and
// moved on to "now".

using namespace std::chrono_literals;

namespace {

// One audio buffer of 10 ms moves the track on by 0.001 of its length.
constexpr double kBufferMicros = 10000.0;
constexpr double kStep = 0.001;

void setPosition(VisualPlayPosition* pPosition, double playPos, double rate = 1.0) {
    pPosition->set(playPos,
            rate,
            kStep,
            playPos,
            rate,
            SlipModeState::Disabled,
            false,
            false,
            false,
            0.0,
            0.0,
            200.0,
            kBufferMicros);
}

} // namespace

TEST(EnginePositionTest, NothingBeforeTheEngineHasAPosition) {
    VisualPlayPosition position(QStringLiteral("[EnginePositionTest0]"));
    double now = -1.0;
    EXPECT_FALSE(position.getEnginePlayPosNow(&now));
}

TEST(EnginePositionTest, MovesOnWithTheTimeSinceTheBuffer) {
    PerformanceTimer bufferStart;
    bufferStart.start();
    VisualPlayPosition::setCallbackEntryToDacSecs(0.0, bufferStart);
    VisualPlayPosition position(QStringLiteral("[EnginePositionTest1]"));
    setPosition(&position, 0.5);
    std::this_thread::sleep_for(5ms); // at least half a buffer later
    double now = 0.0;
    ASSERT_TRUE(position.getEnginePlayPosNow(&now));
    EXPECT_GE(now, 0.5 + 0.5 * kStep);
    EXPECT_LE(now, 0.5 + 2.0 * kStep + 1e-12); // never more than 2 buffers on
}

TEST(EnginePositionTest, NeverMoreThanTwoBuffersOn) {
    // The engine stopped sending (e.g. the audio device stalled): the
    // position does not run away.
    PerformanceTimer bufferStart;
    bufferStart.start();
    VisualPlayPosition::setCallbackEntryToDacSecs(0.0, bufferStart);
    VisualPlayPosition position(QStringLiteral("[EnginePositionTest2]"));
    setPosition(&position, 0.25);
    std::this_thread::sleep_for(40ms);
    double now = 0.0;
    ASSERT_TRUE(position.getEnginePlayPosNow(&now));
    EXPECT_NEAR(0.25 + 2.0 * kStep, now, 1e-12);
}

TEST(EnginePositionTest, TwoDecksAreComparedAtTheSameMoment) {
    // Both decks were stamped with the same buffer and are read one after
    // the other, a while after it. Their difference must stay what it was
    // in the engine, however late they are read (the beat lock measures its
    // slip from it). With the old readings it could be up to 67 ms off.
    PerformanceTimer bufferStart;
    bufferStart.start();
    VisualPlayPosition::setCallbackEntryToDacSecs(0.0, bufferStart);
    VisualPlayPosition deck1(QStringLiteral("[EnginePositionTest3]"));
    VisualPlayPosition deck2(QStringLiteral("[EnginePositionTest4]"));
    setPosition(&deck1, 0.600);
    setPosition(&deck2, 0.100);
    std::this_thread::sleep_for(8ms);
    double first = 0.0;
    ASSERT_TRUE(deck1.getEnginePlayPosNow(&first));
    double second = 0.0;
    ASSERT_TRUE(deck2.getEnginePlayPosNow(&second));
    EXPECT_GT(first, 0.600 + 0.5 * kStep); // both moved on with the time...
    // ... by the same amount (0.00001 of the track = 0.1 ms here).
    EXPECT_NEAR(0.500, first - second, 1e-5);
}

TEST(EnginePositionTest, FollowsTheDeckSpeed) {
    PerformanceTimer bufferStart;
    bufferStart.start();
    VisualPlayPosition::setCallbackEntryToDacSecs(0.0, bufferStart);
    VisualPlayPosition position(QStringLiteral("[EnginePositionTest5]"));
    setPosition(&position, 0.5, 0.0); // stopped
    std::this_thread::sleep_for(5ms);
    double now = 0.0;
    ASSERT_TRUE(position.getEnginePlayPosNow(&now));
    EXPECT_DOUBLE_EQ(0.5, now);
}
