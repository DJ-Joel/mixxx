#pragma once

#include <optional>
#include <vector>

/// Auto DJ 2.0 plus Video Mixing, phase 2: the maths of a beatmatched transition.
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

/// Steady loudness through a mix. With Mixxx's additive crossfader both
/// songs play at full volume in the middle of the mix, about 2-3 dB louder
/// than either song alone: a give-away that a mix is happening. A deck's
/// loudness is estimated from its crossfader gain and how much of its bass
/// and of its mids and highs are open (half the power each, a rough model
/// of dance music); both decks are then turned down together so the sum
/// stays at one song's level, never by more than kMinLoudnessGain.
double deckPower(double faderGain, double bassLevel, double midHighLevel);
constexpr double kMinLoudnessGain = 0.79; // -2 dB
double loudnessGain(double fromPower, double toPower, double minGain = kMinLoudnessGain);

/// Tempo ride: two songs too far apart in tempo for one beatmatch (more
/// than the 5% rule) meet in the middle. Before the mix the outgoing song
/// eases its tempo part of the way towards the incoming one (key lock on,
/// over about a minute, too slowly to hear); the incoming song then
/// beatmatches as usual and eases back to its own tempo after the mix. So
/// neither song is ever more than `maxEachPct` off its own tempo.
/// The widest tempo gap the ride bridges, and the most each song moves.
constexpr double kTempoRideMaxGapPct = 9.0;
constexpr double kTempoRideMaxEachPct = 4.7;
struct RidePlan {
    double outgoingRatio = 1.0; ///< the outgoing song's speed at the mix (1 = own tempo)
    double incomingRatio = 1.0; ///< the incoming song's speed at the mix
    double gapPct = 0.0;        ///< how far apart the two tempos were
};
/// nullopt when no ride is needed (within `beatmatchPct`), or the gap is
/// wider than `maxGapPct`. Half and double time count as close, as in
/// matchRatio. The gap is shared equally when possible; the outgoing song
/// always moves at least enough to bring the incoming one within
/// `maxEachPct`, and never more than that itself.
std::optional<RidePlan> ridePlan(double fromBpm,
        double toTrackBpm,
        double beatmatchPct = 5.0,
        double maxGapPct = kTempoRideMaxGapPct,
        double maxEachPct = kTempoRideMaxEachPct);
/// How long the outgoing song takes to ease to the ride tempo.
constexpr double kTempoRideSeconds = 60.0;
/// The outgoing speed during the ride: from `startRatio` to `targetRatio`
/// as `progress` goes 0..1, easing in and out (no sudden change of pace).
double rideRatio(double startRatio, double targetRatio, double progress);

/// Key slide: when the keys clash in a beatmatched mix, the outgoing song
/// slides to a key that fits the incoming one (MixScorer::keyMorphSemitones,
/// key lock on) in its last seconds before the mix, while it plays alone.
/// The mix is then in tune, and the incoming song plays in its own key all
/// through (a key shift on the incoming song was kept to its end, and the
/// next song matched that shifted key: the shifts added up song after song).
/// The slide takes this long and ends where the fade begins...
constexpr double kKeySlideSeconds = 16.0;
/// ...and is not started with less time than this left (quicker is heard).
constexpr double kKeySlideMinSeconds = 8.0;
/// The outgoing song's key shift (semitones) during the slide: from `start`
/// to `target` as `progress` goes 0..1, easing in and out.
double keySlideSemitones(double start, double target, double progress);

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

/// Full EQ transition, on top of the bass swap: the mids and highs cross
/// over gradually instead of both tracks playing at full. First half: the
/// incoming mids/highs rise from kEqBlendFloor to full. Second half: the
/// outgoing mids/highs fall to kEqBlendFloor. The values multiply the
/// DJ's own EQ setting (1 = leave it as the DJ set it).
constexpr double kEqBlendFloor = 0.3; // about -10 dB
struct EqBlend {
    double fromMidHigh = 1.0;
    double toMidHigh = 1.0;
};
EqBlend eqBlend(double progress);

/// Stem mix: when both songs have their parts (drums, bass, other =
/// instrumental, vocals), the parts cross over instead of the EQ.
/// First half: the incoming instrumental rises, the outgoing vocals fall.
/// Middle: drums and bass swap hard, like the bass swap. Second half: the
/// outgoing instrumental falls, the incoming vocals rise. So two vocals
/// never play together and two basslines / kicks never clash. The values
/// multiply the DJ's own part levels (equal-power curves).
struct StemBlend {
    double fromVocals = 1.0;
    double fromInstrumental = 1.0;
    double fromDrums = 1.0;
    double fromBass = 1.0;
    double toVocals = 0.0;
    double toInstrumental = 0.0;
    double toDrums = 0.0;
    double toBass = 0.0;
};
/// `fadeDrums`: the drums cross over gradually instead (incoming drums rise
/// over the first half, outgoing drums fall over the second half); the bass
/// still swaps at the middle (two basslines clash).
StemBlend stemBlend(double progress, bool fadeDrums = false);

/// Singing detection: who sings during the mix, and when the outgoing
/// singer ends a line (as mix progress 0..1).
struct VocalPlan {
    bool fromSings = true;
    bool toSings = true;
    double swapAt = 0.5;
};
/// The stem mix with the vocals timed by the singing: a song that does not
/// sing during the mix keeps its vocals with its instrumental (untouched);
/// only when both sing are the vocals handed over, at `swapAt`, never
/// together.
StemBlend stemBlend(double progress, const VocalPlan& plan, bool fadeDrums = false);

/// Beat lock, for a track whose tempo bends (a beat map): during the mix
/// the incoming speed is set again and again so that its beats stay on the
/// outgoing beats, like a DJ riding the pitch fader.
///
/// @param fromBeat outgoing track: beat number now (fractional)
/// @param fromBeatRealSec outgoing track: length of its beat now, in real
///        seconds (at the deck's speed)
/// @param toBeat incoming track: beat number now (fractional)
/// @param toBeatTrackSec incoming track: length of its beat now, at its
///        own speed
/// @return the incoming tempo ratio: the one that gives it the same beat
///         length, nudged by up to kMaxLockNudge to pull a beat that has
///         slipped back in line within about kLockBeats beats. A slip of
///         less than kLockDeadBeats is left alone (no needless wobble).
constexpr double kLockBeats = 2.0;
constexpr double kMaxLockNudge = 0.06; // 6 % (key lock keeps the pitch)
constexpr double kLockDeadBeats = 0.01; // 5 ms at 120 BPM
/// Early in the mix (the incoming song still quiet), a slip bigger than
/// this is not pulled back slowly: the incoming song jumps in line.
constexpr double kResyncBeats = 0.05;
double followRatio(double fromBeat,
        double fromBeatRealSec,
        double toBeat,
        double toBeatTrackSec,
        double* pSlipBeats = nullptr);

/// Where the kick really starts after a song's beat lines. Beat analysis
/// does not put every song's lines at the same point of the kick: in real
/// songs the kick started 8 to 41 ms after the line. Two songs whose lines
/// are locked can then still have their kicks 30 ms apart (heard as a
/// clash), so beat lock lines up the kicks instead.
///
/// @param level the drum level (0..1), `rate` values per second (the
///        waveform: about 441 per second)
/// @param beatTimes the song's beat lines, in seconds
/// @return the middle offset in seconds (kick after the line = positive),
///         or nothing when fewer than 16 beats have a clear hit
std::optional<double> kickOffset(const std::vector<float>& level,
        double rate,
        const std::vector<double>& beatTimes);

/// Tempo check: is the song's real beat 4/3 slower than its beat grid? A
/// dotted rhythm (three hits for every four beats) is read by the beat
/// analysis as the beat: Night Drive "After Dark" is 85 BPM but read 113.
/// Such a song cannot be beatmatched by its grid (it clashes and its bars
/// feel wrong). Measured on the drum part (or the high band): how well the
/// hits repeat after one grid beat and after 4/3 of one, in 12 s windows.
/// In a 318-song library this margin was 0.20 for Night Drive and at most
/// -0.01 for every other song. (The other way round, a beat read too slow,
/// is not checked: swing fooled it, George Michael "Freedom! '90".)
///
/// @param level drum level (0..1), `rate` values per second
/// @param beatSec the grid's beat length
/// @return how much better the hits repeat at 4/3 of the beat than at the
///         beat (> kFourThirdsMargin: the real beat is 4/3 slower), or
///         nothing when there is too little music between fromSec and toSec
constexpr double kFourThirdsMargin = 0.1;
std::optional<double> fourThirdsMargin(const std::vector<float>& level,
        double rate,
        double beatSec,
        double fromSec,
        double toSec);

} // namespace beatmatch
