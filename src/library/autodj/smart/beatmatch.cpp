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

} // namespace beatmatch
