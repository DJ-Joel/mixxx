#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "library/autodj/smart/phrasealign.h"

/// Auto DJ 2.0 plus Video Mixing: the beat check.
///
/// Mixxx's own beat detection listens to the whole mix and is often
/// fooled: a dotted rhythm read as the beat (Night Drive "After Dark": 113
/// BPM for an 85 BPM song), an intro without a kick read at a made-up
/// tempo (Cabaret Voltaire "Here To Go": 140 for 104), beat 1 of the bar
/// taken from an Intro End marker a beat or more off. The beat check runs
/// a second, independent beat finder, "Beat This!" (Foscarin, Schlüter and
/// Widmer, ISMIR 2024; code and model MIT licensed, JKU Linz), and compares
/// the two. What looks wrong is shown to the DJ as a list to Fix or
/// Ignore (DlgAnalysis); nothing is changed without that.
///
/// Tested against 24 songs checked by ear: the main tempo was right for
/// 22 with Mixxx and all 24 with Beat This (Night Drive at 170 = 2 x 85);
/// beat 1 of the bar was within a quarter beat for 2 of 11 Intro End
/// markers and 9 of 11 Beat This bar lines.
///
/// This file is the part without the AI engine (beatthisengine.h runs the
/// model): preparing its input, reading its output, and the rules.
namespace beatcheck {

/// The model's input: mono audio at this rate, 50 frames per second.
constexpr double kSampleRate = 22050.0;
constexpr int kFramesPerSecond = 50;
/// The model sees 30 s at a time (1500 frames); the first and last 6
/// frames of each piece are not trusted (the model was not trained on its
/// edges), so the pieces overlap by that much.
constexpr int kChunkFrames = 1500;
constexpr int kBorderFrames = 6;
constexpr int kMelBands = 128;

/// High-quality resampling (windowed sinc, Kaiser window), e.g. 44100 or
/// 48000 Hz to kSampleRate. The band above 0.97 of the new Nyquist
/// frequency is removed (no aliasing).
std::vector<float> resample(const std::vector<float>& input, double inRate, double outRate);

/// Where each model piece starts, in frames (the first one at
/// -kBorderFrames, padded with silence; the last one ends exactly at the
/// end), as Beat This does it (split_piece, avoid_short_end).
std::vector<int> chunkStarts(int frames);

/// One model piece of the spectrogram `spect` ([frames][kMelBands]),
/// starting at `start`, padded with zeros where it reaches past either
/// end (at most kBorderFrames at the end). Returns its length in frames.
int chunkInput(const std::vector<float>& spect, int frames, int start, std::vector<float>* pOut);

/// The model's per-frame answers (logits) for the whole song, put
/// together from the pieces: each piece without its untrusted edges, an
/// earlier piece wins where two overlap.
class Aggregator {
  public:
    explicit Aggregator(int frames);
    /// One piece's output, `length` frames as returned by chunkInput().
    void add(int start, const float* pBeat, const float* pDownbeat, int length);
    const std::vector<float>& beat() const {
        return m_beat;
    }
    const std::vector<float>& downbeat() const {
        return m_downbeat;
    }

  private:
    std::vector<float> m_beat;
    std::vector<float> m_downbeat;
    std::vector<bool> m_set;
};

/// Beat and downbeat times (seconds) from the per-frame logits, as Beat
/// This's "minimal" post-processing: the highest frame within +-3 frames
/// (70 ms) above a probability of 0.5, neighbouring frames merged, each
/// downbeat moved onto the nearest beat.
struct Found {
    std::vector<double> beats;
    std::vector<double> downbeats;
};
Found peaks(const std::vector<float>& beatLogits, const std::vector<float>& downbeatLogits);

// --- The rules: what looks wrong, and the fix -------------------------

/// What Mixxx has for the song now.
struct Song {
    phrasealign::Grid grid;    ///< Mixxx's beat grid or beat map (track seconds)
    double introEndSec = -1.0; ///< the Intro End marker (beat 1), -1 = none
    bool introEndByDj = false; ///< set or moved by the DJ: never changed
    bool bpmLocked = false;    ///< locked: a new grid only when the DJ picks Fix for it
    double durationSec = 0.0;
    /// false: no new grid (the one worked out did not stay on the music in
    /// the beat grid check, EnergyCalculator::gridDriftBeats).
    bool allowNewGrid = true;
    /// false: not the intro-only repair (Mixxx's own main part did not
    /// pass the beat grid check): a grid from the beats heard instead.
    bool allowIntroOnly = true;
};

/// The beat check's answer for one song.
struct Review {
    bool checked = false;      ///< enough beats heard to judge
    double songBpm = 0.0;      ///< the tempo heard, at Mixxx's level where it agrees
    double mixxxBpm = 0.0;     ///< Mixxx's tempo of the main part
    // What looks wrong.
    bool tempoWrong = false;   ///< not the same tempo (nor double, nor half)
    bool introMisread = false; ///< Mixxx reads the intro at another tempo, the beat check does not
    double introBpm = 0.0;     ///< ... Mixxx's intro tempo
    double linesOnBeat = 1.0;  ///< share of the heard beats that have a Mixxx beat line on them
    double lineOffsetSec = 0.0; ///< the heard beats this much after Mixxx's lines (steady offset)
    bool linesOff = false;     ///< linesOnBeat < kMinLinesOnBeat
    /// ... and most of the beats off are half a beat off: one of the two
    /// is on the off-beat, and which one cannot be told without listening
    /// (Icicle Works: Mixxx's lines are where Auto DJ found the kicks, the
    /// beat check hears the beat half a beat later).
    bool halfBeat = false;
    double beatOneOffBeats = 0.0; ///< Intro End marker from the nearest heard bar line, in beats
    bool beatOneOff = false;   ///< more than kBeatOneTolerance (and at most kBeatOneMaxBeats)
    // The fix (only what the DJ has not locked or set).
    bool newGrid = false;      ///< replace the beat grid
    bool replacesLock = false; ///< ... of a song with a locked BPM (only if the DJ picks it)
    /// ... keeping Mixxx's own beats in the main part, the intro counted
    /// back from it at the main tempo (an intro misread, the rest right).
    bool introOnly = false;
    bool steady = true;        ///< ... with one tempo (else a beat map)
    double newBpm = 0.0;
    double newFirstBeatSec = 0.0;  ///< steady grid: its first line (a bar line)
    std::vector<double> newBeats;  ///< beat map: every beat
    double newIntroEndSec = -1.0;  ///< move the Intro End marker here (-1 = leave it)

    bool anyProblem() const {
        return tempoWrong || introMisread || linesOff || beatOneOff;
    }
    bool anyFix() const {
        return newGrid || newIntroEndSec >= 0.0;
    }
};

/// Same tempo within this (else: double, half, or wrong).
constexpr double kTempoTolerance = 0.03;
/// A beat line counts as on the beat within this, after the song's steady
/// offset is taken off (Mixxx puts its lines 10-55 ms before the kick on
/// many songs, the beat check on it; Auto DJ's kick line-up deals with
/// that, so only beats off by more than that count).
constexpr double kOnBeatSec = 0.06;
constexpr double kMinLinesOnBeat = 0.8;
/// The beat lines are only judged when the beat check counts the song
/// steadily itself: this share of its beat gaps within 10% of the usual
/// one. (In Icicle Works "Whisper to a Scream" it switches between beats
/// and half beats, and sits on the off-beat in between.)
constexpr double kMinSteadyGaps = 0.85;
/// Beat 1 off by more than a quarter beat, and by at most this many beats.
/// Two beats off (half a bar) is where the beat check itself was wrong in
/// the test songs (Human League "Fascination"), and a whole bar is a
/// question of which bar the main part starts on, which bar lines cannot
/// answer: both are left alone.
constexpr double kBeatOneTolerance = 0.25;
constexpr double kBeatOneMaxBeats = 1.75;
/// Below this share of the song with beats, the beat check does not judge.
constexpr double kMinCoverage = 0.5;
/// A tempo from the beat check is used between these (the half or the
/// double otherwise): the DJ's usual range for this music.
constexpr double kMinBpm = 70.0;
constexpr double kMaxBpm = 160.0;

/// Compares the beat check's beats with what Mixxx has.
Review review(const Found& found, const Song& song);

/// The bass and treble levels the beat grid check needs, kept with the
/// beat check's result: one byte a level (a quarter dB from kLevelFloorDb;
/// lower is the same to the check).
constexpr double kLevelFloorDb = -60.0;
std::vector<std::uint8_t> packLevels(const std::vector<float>& db);
std::vector<float> unpackLevels(const std::uint8_t* pBytes, std::size_t count);

} // namespace beatcheck
