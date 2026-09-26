#pragma once

#include <optional>

/// Auto DJ 2.0 Phase 2: the maths of a beatmatched transition.
/// Pure functions, no Mixxx controls, so they can be unit-tested.
namespace beatmatch {

/// Seconds the incoming track takes to glide back to its own tempo
/// after the mix. Slow enough that the change is not heard.
constexpr double kGlideSeconds = 30.0;

/// The tempo ratio to play the incoming track at so its beats line up with
/// the outgoing track (1.0 = its own speed). Uses half/double time when that
/// is closer (e.g. 72 BPM under 144 BPM). Returns nullopt when the change
/// would be more than `tolerancePct` percent (the DJ's 5% rule) or a BPM is
/// unknown; the transition is then a plain crossfade.
std::optional<double> matchRatio(double fromBpm, double toTrackBpm, double tolerancePct);

/// Ratio during the glide back: a straight line from `startRatio` to 1.0
/// over `glideSeconds`.
double glideRatio(double startRatio, double elapsedSeconds, double glideSeconds = kGlideSeconds);

/// Which bass (low EQ) is cut at a point in the crossfade (0..1).
/// Hard swap at the middle: first half the incoming bass is cut, second
/// half the outgoing bass is cut, so two basslines never play together.
struct BassState {
    bool fromLowKilled = false;
    bool toLowKilled = false;
};
BassState bassSwap(double progress);

} // namespace beatmatch
