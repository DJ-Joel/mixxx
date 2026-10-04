#include "library/autodj/smart/mixstep.h"

#include <algorithm>
#include <cmath>

#include "library/autodj/smart/bridgefinder.h"

namespace mixstep {

namespace {
// Auto DJ beatmatches up to this tempo distance without the ride.
constexpr double kBeatmatchPct = 5.0;
} // namespace

QString Step::helpText() const {
    QStringList parts;
    if (keyShift != 0) {
        parts << QStringLiteral("key morph %1%2")
                         .arg(keyShift > 0 ? QStringLiteral("+") : QString())
                         .arg(keyShift);
    }
    if (tempoRide) {
        parts << QStringLiteral("tempo ride %1%").arg(tempoGapPct, 0, 'f', 1);
    }
    return parts.join(QStringLiteral(", "));
}

double tempoGapPct(double fromBpm, double toBpm, bool allowHalfDoubleTime) {
    if (!(fromBpm > 0.0) || !(toBpm > 0.0)) {
        return 100.0;
    }
    double diff = std::fabs(std::log(toBpm / fromBpm));
    if (allowHalfDoubleTime) {
        diff = std::min({diff,
                std::fabs(std::log(2.0 * toBpm / fromBpm)),
                std::fabs(std::log(toBpm / (2.0 * fromBpm)))});
    }
    return diff * 100.0;
}

Step judge(const MixScorer& scorer,
        const TrackFeatures& from,
        const TrackFeatures& to,
        const Rules& rules) {
    Step step;
    const MixScoreWeights& w = scorer.weights();
    const bool steady = !from.gridUnsteady && !to.gridUnsteady;
    const bool bpmsKnown = from.hasBpm() && to.hasBpm();
    step.tempoGapPct = bpmsKnown ? tempoGapPct(from.bpm, to.bpm, w.allowHalfDoubleTime) : 100.0;
    // Tempo: plain beatmatch, the tempo ride, or out of reach.
    const bool inBeatmatch = bpmsKnown && step.tempoGapPct <= w.bpmTolerancePct;
    step.tempoRide = !inBeatmatch && steady && bpmsKnown && rules.tempoRidePct > 0.0 &&
            step.tempoGapPct <= rules.tempoRidePct;
    step.beatmatched = steady && (inBeatmatch || step.tempoRide);

    // Key: as it is, or with the key morph (beatmatched mixes only).
    TrackFeatures incoming = to;
    step.score = scorer.score(from, incoming);
    const bool keyClash = step.score.keyCost >= MixScorer::kClashKeyCost;
    if (keyClash && step.beatmatched && rules.keyMorphLimit > 0) {
        const int shift = MixScorer::keyMorphSemitones(from, to, rules.keyMorphLimit);
        if (shift != 0) {
            step.keyShift = shift;
            incoming = MixScorer::shiftKey(to, shift);
            step.score = scorer.score(from, incoming);
        }
    }
    double tempoCost = step.score.tempoCost;
    if (step.tempoRide) {
        // The plain score calls this a clash; the ride makes it mixable at
        // a price: more ride, more cost.
        tempoCost = 1.0 + rules.tempoRideCost +
                rules.tempoRideCostPerPct * (step.tempoGapPct - kBeatmatchPct);
        step.score.tempoCost = tempoCost;
    }
    const bool keyOk = step.score.keyCost < MixScorer::kClashKeyCost;
    const bool tempoOk = step.tempoRide || step.score.tempoCost < MixScorer::kClashTempoCost;
    step.smooth = keyOk && tempoOk;

    // (score.energyCost already counts measured energy less: see score().)
    double cost = w.key * step.score.keyCost + w.tempo * tempoCost +
            w.energy * step.score.energyCost;
    step.score.total = cost;
    if (!steady) {
        cost += rules.unsteadyGridCost;
    }
    if (bpmsKnown) {
        cost += std::max(0.0, std::min(step.tempoGapPct, w.bpmTolerancePct) -
                        rules.tempoComfortPct) *
                rules.tempoCostPerPct;
    }
    cost += rules.keyMorphCost * std::abs(step.keyShift);
    if (rules.genre) {
        cost += BridgeFinder::genreCost(from.genre, to.genre);
    }
    if (rules.sameArtistCost > 0.0) {
        const QString artist = BridgeFinder::artistKey(from);
        if (!artist.isEmpty() && artist == BridgeFinder::artistKey(to)) {
            cost += rules.sameArtistCost;
        }
    }
    step.cost = cost;
    return step;
}

TrackFeatures heard(const TrackFeatures& to, const Step& step) {
    return step.keyShift != 0 ? MixScorer::shiftKey(to, step.keyShift) : to;
}

} // namespace mixstep
