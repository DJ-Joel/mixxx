#include "library/autodj/smart/beatmatch.h"

#include <algorithm>
#include <cmath>

namespace beatmatch {

std::optional<double> matchRatio(double fromBpm, double toTrackBpm, double tolerancePct) {
    if (!(fromBpm > 0.0) || !(toTrackBpm > 0.0)) {
        return std::nullopt;
    }
    // Candidates: same tempo, or the incoming track at half/double time.
    const double candidates[] = {
            fromBpm / toTrackBpm,
            2.0 * fromBpm / toTrackBpm,
            fromBpm / (2.0 * toTrackBpm),
    };
    double best = candidates[0];
    for (double r : candidates) {
        if (std::abs(std::log(r)) < std::abs(std::log(best))) {
            best = r;
        }
    }
    if (std::abs(std::log(best)) * 100.0 > tolerancePct) {
        return std::nullopt;
    }
    return best;
}

double glideRatio(double startRatio, double elapsedSeconds, double glideSeconds) {
    if (glideSeconds <= 0.0 || elapsedSeconds >= glideSeconds) {
        return 1.0;
    }
    const double t = std::clamp(elapsedSeconds / glideSeconds, 0.0, 1.0);
    return startRatio + (1.0 - startRatio) * t;
}

BassState bassSwap(double progress) {
    BassState s;
    if (progress < 0.5) {
        s.toLowKilled = true;
    } else {
        s.fromLowKilled = true;
    }
    return s;
}

EqBlend eqBlend(double progress) {
    const double p = std::isnan(progress) ? 0.0 : std::clamp(progress, 0.0, 1.0);
    EqBlend e;
    if (p < 0.5) {
        e.toMidHigh = kEqBlendFloor + (1.0 - kEqBlendFloor) * (p / 0.5);
    } else {
        e.fromMidHigh = 1.0 - (1.0 - kEqBlendFloor) * ((p - 0.5) / 0.5);
    }
    return e;
}

StemBlend stemBlend(double progress) {
    const double p = std::isnan(progress) ? 0.0 : std::clamp(progress, 0.0, 1.0);
    constexpr double kHalfPi = 1.5707963267948966;
    StemBlend s;
    if (p < 0.5) {
        const double x = p / 0.5; // 0..1 over the first half
        s.fromVocals = std::cos(x * kHalfPi);
        s.toInstrumental = std::sin(x * kHalfPi);
    } else {
        const double x = (p - 0.5) / 0.5; // 0..1 over the second half
        s.fromVocals = 0.0;
        s.toInstrumental = 1.0;
        s.fromInstrumental = std::cos(x * kHalfPi);
        s.toVocals = std::sin(x * kHalfPi);
    }
    // The same moment as the bass swap.
    const BassState bass = bassSwap(p);
    s.fromDrums = s.fromBass = bass.fromLowKilled ? 0.0 : 1.0;
    s.toDrums = s.toBass = bass.toLowKilled ? 0.0 : 1.0;
    return s;
}

StemBlend stemBlend(double progress, const VocalPlan& plan) {
    const double p = std::isnan(progress) ? 0.0 : std::clamp(progress, 0.0, 1.0);
    constexpr double kHalfPi = 1.5707963267948966;
    StemBlend s = stemBlend(p);
    const double swapAt = std::clamp(std::isnan(plan.swapAt) ? 0.5 : plan.swapAt, 0.15, 0.85);
    if (!plan.fromSings) {
        s.fromVocals = s.fromInstrumental; // nothing sung: leave with the song
    } else if (plan.toSings) {
        // Both sing: the outgoing line ends at swapAt.
        const double w = std::min(0.25, swapAt);
        const double x = std::clamp((p - (swapAt - w)) / w, 0.0, 1.0);
        s.fromVocals = p >= swapAt ? 0.0 : std::cos(x * kHalfPi);
    }
    if (!plan.toSings) {
        s.toVocals = s.toInstrumental; // nothing sung yet: comes in with the song
    } else if (plan.fromSings) {
        const double w = std::min(0.25, 1.0 - swapAt);
        const double x = std::clamp((p - swapAt) / w, 0.0, 1.0);
        s.toVocals = p <= swapAt ? 0.0 : std::sin(x * kHalfPi);
    } else {
        // Only the new song sings: over the old instrumental, no clash.
        s.toVocals = s.toInstrumental;
    }
    return s;
}

double followRatio(double fromBeat,
        double fromBeatRealSec,
        double toBeat,
        double toBeatTrackSec,
        double* pSlipBeats) {
    if (!(fromBeatRealSec > 0.0) || !(toBeatTrackSec > 0.0)) {
        return 1.0;
    }
    // Where in the beat each track is: positive = the incoming beat comes
    // late (it must catch up).
    double slip = fromBeat - toBeat;
    slip -= std::round(slip); // -0.5 .. 0.5
    if (pSlipBeats) {
        *pSlipBeats = slip;
    }
    double nudge = 0.0;
    if (std::fabs(slip) > kLockDeadBeats) {
        nudge = std::clamp(slip / kLockBeats, -kMaxLockNudge, kMaxLockNudge);
    }
    return toBeatTrackSec / fromBeatRealSec * (1.0 + nudge);
}

} // namespace beatmatch
