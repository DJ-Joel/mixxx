#pragma once

#include <QString>

#include "library/autodj/smart/mixscorer.h"

/// Auto DJ 2.0 plus Video Mixing: one judge for "can Auto DJ mix this song
/// into that one, and how well?", shared by Smart Fill, Smart Sort, the
/// bridge finder and the Live Assistant, so they all pick what Auto DJ can
/// really mix smoothly. Pure logic, unit tested (smartsequencer_test.cpp).
///
/// On top of the mix score (key, tempo, energy, see MixScorer) it knows
/// what Auto DJ does in a mix:
/// - beatmatching needs both beat grids steady (the grid check, 4.8);
/// - tempos close together beatmatch safely (a beat map's tempo where the
///   mix happens can be a little off its average);
/// - the key morph pitches a clashing incoming song to fit, but only in a
///   beatmatched mix;
/// - the tempo ride brings two songs up to tempoRidePct apart together, each
///   moving about half the way (see beatmatch::ridePlan), also only when
///   both grids are steady.
namespace mixstep {

struct Rules {
    int keyMorphLimit = 0;        ///< semitones the key morph may use (0 = off)
    double tempoRidePct = 0.0;    ///< widest gap the tempo ride bridges (0 = off)
    double tempoComfortPct = 3.0; ///< closer than this: no extra cost
    double tempoCostPerPct = 1.0; ///< extra cost per % beyond the comfort zone
    double unsteadyGridCost = 1.5;
    double keyMorphCost = 1.0;  ///< per semitone
    double tempoRideCost = 1.5; ///< for needing the ride at all
    double tempoRideCostPerPct = 0.75; ///< per % beyond the 5% beatmatch limit
    bool genre = true; ///< add the genre cost (BridgeFinder::genreCost)
    /// Extra cost for the same artist twice in a row (Smart Sort; Smart
    /// Fill leaves such songs out instead). 0 = no rule.
    double sameArtistCost = 0.0;
};

struct Step {
    bool smooth = false;      ///< no key or tempo clash left (with the help below)
    bool beatmatched = false; ///< Auto DJ will beatmatch it (grids steady, tempos in reach)
    double cost = 0.0;        ///< lower is better (a clash stays expensive)
    int keyShift = 0;         ///< key morph to apply to the incoming song
    bool tempoRide = false;   ///< the two tempos are brought together by the ride
    double tempoGapPct = 0.0; ///< tempo distance (half/double time counted as close)
    MixScore score;           ///< the plain mix score (keys as heard)
    /// What helps, for the DJ: "", "key morph -1", "tempo ride 6.4%", ...
    QString helpText() const;
};

/// Tempo distance in % (log ratio, half and double time count as close).
double tempoGapPct(double fromBpm, double toBpm, bool allowHalfDoubleTime = true);

/// `from` as it will be heard (with its own key morph applied: see
/// MixScorer::shiftKey), `to` as it is.
Step judge(const MixScorer& scorer,
        const TrackFeatures& from,
        const TrackFeatures& to,
        const Rules& rules);

/// The key `to` will be heard in after `step`.
TrackFeatures heard(const TrackFeatures& to, const Step& step);

} // namespace mixstep
