#include "library/autodj/smart/phrasealign.h"

#include <algorithm>
#include <cmath>

namespace phrasealign {

namespace {
// Allow a few milliseconds of rounding when comparing times.
constexpr double kEpsSec = 0.005;
} // namespace

int barsForSeconds(double wantedSec, double beatSec) {
    if (!(beatSec > 0.0) || !(wantedSec > 0.0)) {
        return kBarsPerPhrase;
    }
    const double wantedBars = wantedSec / (beatSec * kBeatsPerBar);
    const int phrases = std::max(1, static_cast<int>(std::lround(wantedBars / kBarsPerPhrase)));
    return phrases * kBarsPerPhrase;
}

std::optional<Plan> plan(const Grid& from,
        const Grid& to,
        double fromNowSec,
        double fromLimitSec,
        double toEarliestSec,
        int bars) {
    if (!from.isValid() || !to.isValid() || bars <= 0) {
        return std::nullopt;
    }
    const double fadeBeats = static_cast<double>(bars) * kBeatsPerBar;

    // Outgoing track: the LAST phrase start from which a whole fade still
    // ends before the limit, so as much of the track as possible plays.
    const double lastStartBeat = (fromLimitSec - from.firstBeatSec) / from.beatSec - fadeBeats;
    if (lastStartBeat < 0.0) {
        return std::nullopt;
    }
    const double phraseStartBeat =
            std::floor((lastStartBeat + kEpsSec / from.beatSec) / kBeatsPerPhrase) *
            kBeatsPerPhrase;
    const double fadeBegin = from.beatTime(phraseStartBeat);
    if (fadeBegin + kEpsSec < fromNowSec) {
        return std::nullopt; // that phrase has already passed
    }

    // Incoming track: the first phrase start at or after its earliest start.
    // Up to one beat early is fine: the first sound is often a few
    // milliseconds after the grid's first beat.
    double toPhraseBeat = std::ceil(
            ((toEarliestSec - to.firstBeatSec) / to.beatSec - 1.0) / kBeatsPerPhrase);
    toPhraseBeat = std::max(0.0, toPhraseBeat) * kBeatsPerPhrase;

    Plan p;
    p.fromFadeBeginSec = fadeBegin;
    p.fromFadeEndSec = from.beatTime(phraseStartBeat + fadeBeats);
    p.toStartSec = to.beatTime(toPhraseBeat);
    p.bars = bars;
    return p;
}

} // namespace phrasealign
