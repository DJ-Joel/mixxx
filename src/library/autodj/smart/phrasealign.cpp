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

double entryBeat(const Grid& grid, double bodyStartSec, bool marked) {
    const double bodyBeat = (bodyStartSec - grid.firstBeatSec) / grid.beatSec;
    if (marked) {
        // The DJ marked it by ear: trust it, just land on a beat.
        return std::max(0.0, std::round(bodyBeat));
    }
    // Body start is measured in 1 s steps (about 1 s early to 2 s late), so
    // a phrase start that close is taken as the real entry; otherwise the
    // nearest bar line.
    constexpr double kEarlySec = 1.0;
    constexpr double kLateSec = 2.0;
    const double nearestPhrase =
            std::max(0.0, std::round(bodyBeat / kBeatsPerPhrase) * kBeatsPerPhrase);
    const double phraseTime = grid.beatTime(nearestPhrase);
    if (phraseTime >= bodyStartSec - kLateSec && phraseTime <= bodyStartSec + kEarlySec) {
        return nearestPhrase;
    }
    return std::max(0.0, std::round(bodyBeat / kBeatsPerBar) * kBeatsPerBar);
}

double bodyEndBarSec(const Grid& grid, double bodyEndSec) {
    const double beat = (bodyEndSec - grid.firstBeatSec) / grid.beatSec;
    const double bar = std::floor((beat + kEpsSec / grid.beatSec) / kBeatsPerBar) * kBeatsPerBar;
    return grid.beatTime(std::max(0.0, bar));
}

std::optional<Plan> plan(const Grid& from,
        const Grid& to,
        double fromNowSec,
        double fromLimitSec,
        double toEarliestSec,
        int bars,
        double toBodyStartSec,
        bool toBodyMarked) {
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
    // Auto DJ re-plans on every tempo step (e.g. while a track glides back
    // to its own tempo), and a re-plan can land just after the phrase start
    // has passed. Up to one bar late, keep the plan: the fade starts now and
    // the incoming track starts further in by the same time, so beats and
    // phrases still line up. More than that (the DJ jumped ahead): give up.
    const double lateSec = std::max(0.0, fromNowSec - fadeBegin);
    if (lateSec > kLateStartBeats * from.beatSec + kEpsSec) {
        return std::nullopt; // that phrase has already passed
    }

    // Incoming track: the first phrase start at or after its earliest start.
    // Up to one beat early is fine: the first sound is often a few
    // milliseconds after the grid's first beat.
    double toPhraseBeat = std::ceil(
            ((toEarliestSec - to.firstBeatSec) / to.beatSec - 1.0) / kBeatsPerPhrase);
    toPhraseBeat = std::max(0.0, toPhraseBeat) * kBeatsPerPhrase;

    if (toBodyStartSec >= 0.0) {
        // Where the beat kicks in, snapped to the grid.
        const double entry = entryBeat(to, toBodyStartSec, toBodyMarked);
        // Start half a fade before it, so the beat kicks in at the bass swap.
        // Never earlier than the intro start found above.
        toPhraseBeat = std::max(toPhraseBeat, entry - fadeBeats / 2.0);
    }

    Plan p;
    p.fromFadeBeginSec = fadeBegin;
    p.fromFadeEndSec = from.beatTime(phraseStartBeat + fadeBeats);
    p.toStartSec = to.beatTime(toPhraseBeat) + lateSec;
    p.bars = bars;
    return p;
}

} // namespace phrasealign
