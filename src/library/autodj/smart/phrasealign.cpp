#include "library/autodj/smart/phrasealign.h"

#include <algorithm>
#include <cmath>

namespace phrasealign {

namespace {
// Allow a few milliseconds of rounding when comparing times.
constexpr double kEpsSec = 0.005;
} // namespace

double Grid::beatTime(double n) const {
    if (!isMap()) {
        return firstBeatSec + n * beatSec;
    }
    const int last = static_cast<int>(beats.size()) - 1;
    if (n <= 0.0) {
        return beats[0] + n * (beats[1] - beats[0]);
    }
    if (n >= last) {
        return beats[last] + (n - last) * (beats[last] - beats[last - 1]);
    }
    const int i = static_cast<int>(std::floor(n));
    return beats[i] + (n - i) * (beats[i + 1] - beats[i]);
}

double Grid::beatAt(double sec) const {
    if (!isMap()) {
        return (sec - firstBeatSec) / beatSec;
    }
    const int last = static_cast<int>(beats.size()) - 1;
    if (sec <= beats[0]) {
        return (sec - beats[0]) / (beats[1] - beats[0]);
    }
    if (sec >= beats[last]) {
        return last + (sec - beats[last]) / (beats[last] - beats[last - 1]);
    }
    const int i = static_cast<int>(
                          std::upper_bound(beats.begin(), beats.end(), sec) - beats.begin()) -
            1;
    return i + (sec - beats[i]) / (beats[i + 1] - beats[i]);
}

double Grid::beatSecAt(double sec, int span) const {
    if (!isMap()) {
        return beatSec;
    }
    const int last = static_cast<int>(beats.size()) - 1;
    span = std::max(1, span);
    const int centre = static_cast<int>(std::lround(std::clamp(beatAt(sec), 0.0, 1.0 * last)));
    const int to = std::min(last, std::max(centre + span, 2 * span));
    const int from = std::max(0, to - 2 * span);
    // The middle beat length, not the average: one false or missed beat
    // (a pickup note before the first real beat) must not change the tempo.
    std::vector<double> lengths;
    lengths.reserve(to - from);
    for (int i = from; i < to; ++i) {
        lengths.push_back(beats[i + 1] - beats[i]);
    }
    const std::size_t mid = lengths.size() / 2;
    std::nth_element(lengths.begin(), lengths.begin() + mid, lengths.end());
    if (lengths.size() % 2 == 1) {
        return lengths[mid];
    }
    const double upper = lengths[mid];
    const double lower = *std::max_element(lengths.begin(), lengths.begin() + mid);
    return 0.5 * (lower + upper);
}

double Grid::steadyBeatSecAt(double sec) const {
    if (!isMap()) {
        return beatSec;
    }
    const int last = static_cast<int>(beats.size()) - 1;
    if (last < 17) {
        return beatSecAt(sec);
    }
    // The middle beat length from beat `a` to beat `b`.
    auto middle = [this](int a, int b) {
        std::vector<double> lengths;
        for (int i = a; i < b; ++i) {
            lengths.push_back(beats[i + 1] - beats[i]);
        }
        const std::size_t mid = lengths.size() / 2;
        std::nth_element(lengths.begin(), lengths.begin() + mid, lengths.end());
        return lengths[mid];
    };
    const double main = middle(0, last);
    const int centre = static_cast<int>(std::lround(std::clamp(beatAt(sec), 0.0, 1.0 * last)));
    for (int back = 0; back <= 128; back += 4) {
        const int i = centre - back;
        if (i - 8 < 0) {
            break;
        }
        const double local = middle(i - 8, std::min(last, i + 8));
        const bool nearMain = std::fabs(local / main - 1.0) <= 0.1;
        const bool held = i - 64 >= 0 && std::fabs(middle(i - 64, i - 8) / local - 1.0) <= 0.03;
        if (nearMain || held) {
            return local;
        }
    }
    return main;
}

Grid Grid::atSpeed(double rateRatio) const {
    Grid g = *this;
    if (!(rateRatio > 0.0)) {
        return g;
    }
    g.firstBeatSec /= rateRatio;
    g.beatSec /= rateRatio;
    for (double& t : g.beats) {
        t /= rateRatio;
    }
    return g;
}

Grid Grid::fromBeats(std::vector<double> beatTimes) {
    std::sort(beatTimes.begin(), beatTimes.end());
    // Two beats at the same moment would divide by zero.
    beatTimes.erase(std::unique(beatTimes.begin(),
                            beatTimes.end(),
                            [](double a, double b) { return b - a < 1e-4; }),
            beatTimes.end());
    Grid g;
    if (beatTimes.size() < 2) {
        return g; // not valid
    }
    g.firstBeatSec = beatTimes.front();
    g.beatSec = (beatTimes.back() - beatTimes.front()) / (beatTimes.size() - 1);
    g.beats = std::move(beatTimes);
    return g;
}

Grid steadyIntro(const Grid& grid, double beatOneSec, double* pIntroBpm, double* pSongBpm) {
    if (!grid.isMap() || !(beatOneSec > 0.0)) {
        return grid;
    }
    const int last = static_cast<int>(grid.beats.size()) - 1;
    const int one = static_cast<int>(std::lround(grid.beatAt(beatOneSec)));
    if (one < 8 || one + 16 > last ||
            std::fabs(grid.beats[one] - beatOneSec) > 0.25 * grid.beatSecAt(beatOneSec)) {
        return grid;
    }
    // The middle beat length (one odd beat does not count).
    auto middle = [&grid](int a, int b) {
        std::vector<double> lengths;
        for (int i = a; i < b; ++i) {
            lengths.push_back(grid.beats[i + 1] - grid.beats[i]);
        }
        const std::size_t mid = lengths.size() / 2;
        std::nth_element(lengths.begin(), lengths.begin() + mid, lengths.end());
        return lengths[mid];
    };
    const double intro = middle(0, one);
    const double song = middle(one, std::min(last, one + 32));
    if (!(intro > 0.0) || !(song > 0.0) || std::fabs(intro / song - 1.0) <= kIntroTempoTolerance) {
        return grid;
    }
    const double oneSec = grid.beats[one];
    // Whole bars before beat 1, from about where the first beat was read
    // (half a beat of leeway), never before the start of the song.
    const double introSec = std::min(oneSec, oneSec - grid.beats.front() + 0.5 * song);
    const int count =
            kBeatsPerBar * static_cast<int>(std::floor(introSec / (kBeatsPerBar * song)));
    std::vector<double> beats;
    beats.reserve(grid.beats.size() - one + count);
    for (int k = count; k >= 1; --k) {
        beats.push_back(oneSec - k * song);
    }
    beats.insert(beats.end(), grid.beats.begin() + one, grid.beats.end());
    const Grid steady = Grid::fromBeats(std::move(beats));
    if (!steady.isValid()) {
        return grid;
    }
    if (pIntroBpm) {
        *pIntroBpm = 60.0 / intro;
    }
    if (pSongBpm) {
        *pSongBpm = 60.0 / song;
    }
    return steady;
}

int barsForSeconds(double wantedSec, double beatSec) {
    if (!(beatSec > 0.0) || !(wantedSec > 0.0)) {
        return kBarsPerPhrase;
    }
    const double wantedBars = wantedSec / (beatSec * kBeatsPerBar);
    const int phrases = std::max(1, static_cast<int>(std::lround(wantedBars / kBarsPerPhrase)));
    return phrases * kBarsPerPhrase;
}

double entryBeat(const Grid& grid, double bodyStartSec, bool marked) {
    const double bodyBeat = grid.beatAt(bodyStartSec);
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
    const double beat = grid.beatAt(bodyEndSec);
    const double bar = std::floor((beat + kEpsSec / grid.beatSecAt(bodyEndSec)) / kBeatsPerBar) *
            kBeatsPerBar;
    return grid.beatTime(std::max(0.0, bar));
}

std::optional<Plan> plan(const Grid& from,
        const Grid& to,
        double fromNowSec,
        double fromLimitSec,
        double toEarliestSec,
        int bars,
        double toBodyStartSec,
        bool toBodyMarked,
        double entryAt) {
    if (!from.isValid() || !to.isValid() || bars <= 0) {
        return std::nullopt;
    }
    const double fadeBeats = static_cast<double>(bars) * kBeatsPerBar;

    // Outgoing track: the LAST phrase start from which a whole fade still
    // ends before the limit, so as much of the track as possible plays.
    const double lastStartBeat = from.beatAt(fromLimitSec) - fadeBeats;
    if (lastStartBeat < 0.0) {
        return std::nullopt;
    }
    const double phraseStartBeat =
            std::floor((lastStartBeat + kEpsSec / from.beatSecAt(fromLimitSec)) /
                               kBeatsPerPhrase) *
            kBeatsPerPhrase;
    const double fadeBegin = from.beatTime(phraseStartBeat);
    // Auto DJ re-plans on every tempo step (e.g. while a track glides back
    // to its own tempo), and a re-plan can land just after the phrase start
    // has passed. Up to one bar late, keep the plan: the fade starts now and
    // the incoming track starts further in by the same time, so beats and
    // phrases still line up. More than that (the DJ jumped ahead): give up.
    const double lateSec = std::max(0.0, fromNowSec - fadeBegin);
    if (lateSec > kLateStartBeats * from.beatSecAt(fadeBegin) + kEpsSec) {
        return std::nullopt; // that phrase has already passed
    }

    // Incoming track: the first phrase start at or after its earliest start.
    // Up to one beat early is fine: the first sound is often a few
    // milliseconds after the grid's first beat.
    double toPhraseBeat = std::ceil(
            (to.beatAt(toEarliestSec) - 1.0) / kBeatsPerPhrase);
    toPhraseBeat = std::max(0.0, toPhraseBeat) * kBeatsPerPhrase;

    if (toBodyStartSec >= 0.0) {
        // Where the beat kicks in, snapped to the grid.
        const double entry = entryBeat(to, toBodyStartSec, toBodyMarked);
        // Start that part of the fade before it (half: the beat kicks in at
        // the bass swap; none: the song starts on it). Whole bars only.
        // Never earlier than the intro start found above.
        const double leadBeats =
                std::round(std::clamp(entryAt, 0.0, 1.0) * fadeBeats / kBeatsPerBar) *
                kBeatsPerBar;
        toPhraseBeat = std::max(toPhraseBeat, entry - leadBeats);
    }

    Plan p;
    p.fromFadeBeginSec = fadeBegin;
    p.fromFadeEndSec = from.beatTime(phraseStartBeat + fadeBeats);
    p.toStartSec = to.beatTime(toPhraseBeat) + lateSec;
    p.bars = bars;
    return p;
}

double bassSwapSec(const Grid& from, double fadeBeginSec, double fadeEndSec) {
    const double middle = 0.5 * (fadeBeginSec + fadeEndSec);
    if (!from.isValid() || !(fadeEndSec > fadeBeginSec)) {
        return middle;
    }
    const double beginBeat = from.beatAt(fadeBeginSec);
    const double endBeat = from.beatAt(fadeEndSec);
    const double startBeat = std::round(beginBeat);
    // Whole bars, never more than half the fade (a quick switch of 1 bar
    // swapped at its very end otherwise). Under 2 bars: the beat nearest
    // the middle.
    const double halfBeats =
            std::floor((endBeat - beginBeat) / 2.0 / kBeatsPerBar + 1e-6) * kBeatsPerBar;
    const double swapBeat = halfBeats > 0.0
            ? startBeat + halfBeats
            : std::round(0.5 * (beginBeat + endBeat));
    const double swapSec = from.beatTime(swapBeat);
    if (!(swapSec > fadeBeginSec && swapSec < fadeEndSec)) {
        return middle;
    }
    return swapSec;
}

std::optional<Plan> planHandover(const Grid& from,
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
    // The last phrase boundary of the outgoing track at or before the limit.
    const double endBeat = std::floor((from.beatAt(fromLimitSec) +
                                              kEpsSec / from.beatSecAt(fromLimitSec)) /
                                   kBeatsPerPhrase) *
            kBeatsPerPhrase;
    const double startBeat = endBeat - fadeBeats;
    if (startBeat < 0.0) {
        return std::nullopt;
    }
    const double fadeBegin = from.beatTime(startBeat);
    const double lateSec = std::max(0.0, fromNowSec - fadeBegin);
    if (lateSec > kLateStartBeats * from.beatSecAt(fadeBegin) + kEpsSec) {
        return std::nullopt;
    }
    // Incoming: its beat kicks in as the fade ends; the bars before it play
    // under the outgoing phrase (never before its intro start).
    double toStartBeat = std::max(0.0, std::ceil(to.beatAt(toEarliestSec) - 1.0));
    if (toBodyStartSec >= 0.0) {
        const double entry = entryBeat(to, toBodyStartSec, toBodyMarked);
        toStartBeat = std::max(toStartBeat, entry - fadeBeats);
    }
    Plan p;
    p.fromFadeBeginSec = fadeBegin;
    p.fromFadeEndSec = from.beatTime(endBeat);
    p.toStartSec = to.beatTime(toStartBeat) + lateSec;
    p.bars = bars;
    return p;
}

namespace {
// Seconds of `sections` inside [a, b].
double coveredSec(const Sections& sections, double a, double b) {
    double sum = 0.0;
    for (const auto& [start, end] : sections) {
        sum += std::max(0.0, std::min(end, b) - std::max(start, a));
    }
    return sum;
}

// Seconds when both sing during the fade: outgoing sections over [fa, fb],
// incoming ones over [ta, ta + (fb - fa)], compared on the fade's own clock.
double overlapSec(const Sections& fromSinging,
        double fa,
        double fb,
        const Sections& toSinging,
        double ta) {
    double sum = 0.0;
    for (const auto& [fs, fe] : fromSinging) {
        const double a = std::max(fs, fa) - fa;
        const double b = std::min(fe, fb) - fa;
        if (b <= a) {
            continue;
        }
        for (const auto& [ts, te] : toSinging) {
            const double c = std::max(ts - ta, a);
            const double d = std::min(te - ta, b);
            sum += std::max(0.0, d - c);
        }
    }
    return sum;
}

// Costs of a mix shape (tuned to prefer, in this order: no two singers at
// once, no singer faded out mid-line, the DJ's length, the latest phrase).
constexpr double kOverlapCostPerSec = 4.0;
constexpr double kCutSingingCostPerSec = 1.0; // outgoing sings while fading out
constexpr double kLateSingingCostPerSec = 0.5; // incoming sings while still quiet
constexpr double kLostMusicCostPerSec = 0.1;  // outgoing song cut short
constexpr double kShorterCostPerOctave = 1.5; // half the DJ's length
constexpr double kLongerCostPerOctave = 0.25; // twice the DJ's length...
constexpr double kCleanLongerBonusPerOctave = 1.0; // ...but when nobody sings in
                                                   // it, a long blend is smoothest
} // namespace

std::optional<ShapedPlan> planShaped(const Grid& from,
        const Grid& to,
        double fromNowSec,
        double fromLimitSec,
        double toEarliestSec,
        double toBodyStartSec,
        bool toBodyMarked,
        const ShapeInput& input) {
    const int preferred = std::clamp(input.preferredBars, 4, 64);
    const bool singingKnown = input.pFromSinging || input.pToSinging;
    std::optional<ShapedPlan> best;
    double usualOverlap = 0.0;
    const double limitBeat = from.isValid() ? from.beatAt(fromLimitSec) : 0.0;
    for (int earlier = 0; earlier <= (singingKnown ? 2 : 0); ++earlier) {
        const double limit = from.beatTime(limitBeat - earlier * kBeatsPerPhrase);
        for (const int bars : {preferred, 4, 8, 16, 32}) {
            if (bars > 2 * preferred || (!singingKnown && bars != preferred)) {
                continue;
            }
            const bool handover = bars < kBarsPerPhrase;
            const auto plan = handover
                    ? planHandover(from, to, fromNowSec, limit, toEarliestSec, bars,
                              toBodyStartSec, toBodyMarked)
                    : phrasealign::plan(from, to, fromNowSec, limit, toEarliestSec, bars,
                              toBodyStartSec, toBodyMarked, input.entryAt);
            if (!plan) {
                continue;
            }
            const double fa = plan->fromFadeBeginSec;
            const double fb = plan->fromFadeEndSec;
            const double length = fb - fa;
            double overlap = 0.0;
            double cost = 0.0;
            if (input.pFromSinging && input.pToSinging) {
                overlap = overlapSec(*input.pFromSinging, fa, fb, *input.pToSinging,
                        plan->toStartSec);
                cost += kOverlapCostPerSec * overlap;
            }
            if (input.pFromSinging) {
                // Singing in the second half of the fade is faded out mid-line.
                cost += kCutSingingCostPerSec * coveredSec(*input.pFromSinging, fa + length / 2, fb);
            }
            if (input.pToSinging) {
                // Incoming singing in the first half is heard only faintly.
                cost += kLateSingingCostPerSec *
                        coveredSec(*input.pToSinging, plan->toStartSec,
                                plan->toStartSec + length / 2);
            }
            cost += kLostMusicCostPerSec * std::max(0.0, fromLimitSec - fb);
            const double octaves = std::log2(static_cast<double>(bars) / preferred);
            if (octaves < 0.0) {
                cost += kShorterCostPerOctave * -octaves;
            } else if (octaves > 0.0) {
                cost += kLongerCostPerOctave * octaves;
                const bool clean = input.pFromSinging && input.pToSinging &&
                        coveredSec(*input.pFromSinging, fa, fb) <= 0.0 &&
                        coveredSec(*input.pToSinging, plan->toStartSec,
                                plan->toStartSec + length) <= 0.0;
                if (clean) {
                    cost -= kCleanLongerBonusPerOctave * octaves;
                }
            }
            if (earlier == 0 && bars == preferred) {
                usualOverlap = overlap;
            }
            if (!best || cost < best->cost - 1e-9) {
                ShapedPlan shaped;
                shaped.plan = *plan;
                shaped.phrasesEarlier = earlier;
                shaped.handover = handover;
                shaped.overlapSec = overlap;
                shaped.cost = cost;
                best = shaped;
            }
        }
    }
    if (best) {
        best->usualOverlapSec = usualOverlap;
    }
    return best;
}

double fadeNowLimitSec(const Grid& from, double nowSec, int bars, double minLeadSec) {
    if (!from.isValid()) {
        return -1.0;
    }
    const double beat = from.beatAt(nowSec + minLeadSec);
    const double phrase = std::max(0.0, std::ceil(beat / kBeatsPerPhrase)) * kBeatsPerPhrase;
    return from.beatTime(phrase + static_cast<double>(bars) * kBeatsPerBar);
}

std::optional<Plan> planUnmatched(const Grid& from,
        const Grid& to,
        double fromNowSec,
        double fromLimitSec,
        double toEarliestSec,
        int bars,
        double toBodyStartSec,
        bool toBodyMarked) {
    // The outgoing side is the same as a beatmatched mix: a whole number of
    // phrases, over before its energy drops. Only the incoming side differs.
    const auto outgoing = plan(from, from, fromNowSec, fromLimitSec, 0.0, bars);
    if (!outgoing) {
        return std::nullopt;
    }
    Plan p = *outgoing;
    double toStart = toEarliestSec;
    if (toBodyStartSec >= 0.0) {
        const double entrySec = to.isValid()
                ? to.beatTime(entryBeat(to, toBodyStartSec, toBodyMarked))
                : toBodyStartSec;
        // The tempos differ, so the two beats must not play together: the
        // new beat kicks in as the fade ENDS, and during the fade only the
        // incoming intro plays under the outgoing beat. If the intro is
        // shorter than the fade (or there is none), the fade is shortened
        // to fit it - down to a quick switch of kQuickSwitchBars - and still
        // ends on the outgoing phrase ending.
        // Counted in the outgoing track's own bars (they may bend).
        const double introSec = entrySec - toEarliestSec;
        const double fadeSec = p.fromFadeEndSec - p.fromFadeBeginSec;
        if (introSec < fadeSec) {
            const double endBeat = std::round(from.beatAt(p.fromFadeEndSec));
            int fitBars = 0;
            while (fitBars < p.bars &&
                    p.fromFadeEndSec - from.beatTime(endBeat - (fitBars + 1) * kBeatsPerBar) <=
                            introSec + 1e-6) {
                ++fitBars;
            }
            const int bars = std::clamp(fitBars, kQuickSwitchBars, p.bars);
            p.fromFadeBeginSec = from.beatTime(endBeat - bars * kBeatsPerBar);
            p.bars = bars;
        }
        toStart = std::max(toEarliestSec, entrySec - (p.fromFadeEndSec - p.fromFadeBeginSec));
    }
    // A fade that should have started a moment ago: the incoming track
    // starts that much further in, so its beat still arrives on time.
    const double lateSec = std::max(0.0, fromNowSec - p.fromFadeBeginSec);
    p.toStartSec = toStart + lateSec;
    return p;
}

} // namespace phrasealign
