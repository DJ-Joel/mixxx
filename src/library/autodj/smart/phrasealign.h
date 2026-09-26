#pragma once

#include <optional>

/// Auto DJ 2.0 Phase 2: line a transition up with musical phrases.
///
/// Assumes 4/4 time, a constant tempo, and that the first beat of the beat
/// grid is a downbeat (bar 1). Phrases are counted in 8-bar blocks from
/// there. If Mixxx's beat grid does not start on a real downbeat, phrases
/// will be off by a whole number of beats; the DJ can move the grid.
/// Pure maths, no Mixxx types, so it can be unit-tested.
namespace phrasealign {

constexpr int kBeatsPerBar = 4;
constexpr int kBarsPerPhrase = 8;
constexpr int kBeatsPerPhrase = kBeatsPerBar * kBarsPerPhrase; // 32

/// A constant-tempo beat grid, in seconds of the track (at its own speed).
struct Grid {
    double firstBeatSec = 0.0; ///< first beat of the grid = start of bar 1
    double beatSec = 0.0;      ///< length of one beat; 0 = no grid

    bool isValid() const {
        return beatSec > 0.0;
    }
    /// Time of beat number `n` (0 = the first beat).
    double beatTime(double n) const {
        return firstBeatSec + n * beatSec;
    }
};

/// What the transition should be.
struct Plan {
    double fromFadeBeginSec = 0.0; ///< outgoing track: fade starts (a phrase start)
    double fromFadeEndSec = 0.0;   ///< outgoing track: fade ends
    double toStartSec = 0.0;       ///< incoming track: starts here (a phrase start)
    int bars = 0;                  ///< length of the fade in bars
};

/// Fade length in bars for a wanted length in seconds: a whole number of
/// 8-bar phrases (at least one), measured at the outgoing track's tempo.
int barsForSeconds(double wantedSec, double beatSec);

/// Plans a phrase-aligned transition, or nullopt if it cannot be done
/// (no grid, or no phrase fits before `fromLimitSec`).
///
/// @param from outgoing track grid
/// @param to incoming track grid
/// @param fromNowSec where the outgoing track is now (the fade cannot start
///        earlier than this)
/// @param fromLimitSec the fade must be over by here (outro end / last sound)
/// @param toEarliestSec the incoming track should not start before this
///        (its intro start / first sound)
/// @param bars fade length in bars
std::optional<Plan> plan(const Grid& from,
        const Grid& to,
        double fromNowSec,
        double fromLimitSec,
        double toEarliestSec,
        int bars);

} // namespace phrasealign
