# Auto DJ 2.0 plus Video Mixing: a smarter Auto DJ and music video mixing for Mixxx

This fork of [Mixxx](https://mixxx.org), **Auto DJ 2.0 plus Video Mixing**, adds an Auto DJ
that plans a set like a DJ does and mixes it so that **the dancers never lose
the beat**, and **music video mixing** for video DJ sets.

All changes are on the branch **`autodj-2`**.

> **Not part of the official Mixxx project.** This is an independent fork. It
> has not been submitted to Mixxx and the author does not plan to do so. The
> Mixxx developers, or anyone else, are welcome to look at these changes and
> take whatever is useful, under the same license as Mixxx (GPL, version 2 or
> later). There is no support and no warranty.

---

## Contents

1. [What it does, in short](#1-what-it-does-in-short)
2. [Using it](#2-using-it)
3. [Building it](#3-building-it)
4. [How it works](#4-how-it-works)
   - 4.1 [Track features](#41-track-features)
   - 4.2 [The mix score](#42-the-mix-score-how-well-two-tracks-follow-each-other)
   - 4.3 [Smart Sort](#43-smart-sort-the-running-order)
   - 4.4 [Bridge tracks](#44-bridge-tracks)
   - 4.5 [Smart Fill](#45-smart-fill)
   - 4.6 [Live Assistant](#46-live-assistant)
   - 4.7 [Energy analysis](#47-energy-analysis)
   - 4.8 [Beat grid check](#48-beat-grid-check)
   - 4.9 [Transitions: beatmatch, bass swap, EQ](#49-transitions-beatmatch-bass-swap-eq)
   - 4.10 [Phrase alignment](#410-phrase-alignment)
   - 4.11 [Mixes that cannot be beatmatched](#411-mixes-that-cannot-be-beatmatched-quick-switch)
   - 4.12 [Beat maps and beat lock](#412-beat-maps-and-beat-lock-songs-whose-tempo-drifts)
   - 4.13 [Key morph](#413-key-morph)
   - 4.14 [Genre Scan](#414-genre-scan)
   - 4.15 [Video mixing](#415-video-mixing)
   - 4.16 [The Windows installer](#416-the-windows-installer)
   - 4.17 [Stems: splitting songs into parts](#417-stems-splitting-songs-into-parts)
5. [Where the code is](#5-where-the-code-is)
6. [Stored data](#6-stored-data)
7. [Tests](#7-tests)
8. [Known limits](#8-known-limits)
9. [Change history](#9-change-history)

---

## 1. What it does, in short

Plain Mixxx Auto DJ plays the queue in order and crossfades for a fixed time.
It does not look at the key, the tempo or the energy of the songs, and it
does not know where the beat is. Auto DJ 2.0 plus Video Mixing adds:

| Feature | What the DJ gets |
|---|---|
| **Smart Sort** | The queue is put in the order that mixes best (key, tempo, energy), with a report of any mix that will still clash. |
| **Bridge tracks** | For a clash that cannot be avoided, songs from the library that fit in between (one song, or two for a big tempo jump). |
| **Smart Fill** | Adds songs from the library (or a crate or playlist) that continue the set smoothly; can keep the queue filled automatically. |
| **Live Assistant** | A small window that always shows the best next songs for whatever is playing, for DJs mixing by hand. |
| **Energy** | Each song gets an energy score (1 to 10), measured from the audio or rated by the DJ. Sets can build up, stay level, or go up and down. |
| **Beatmatched mixes** | The incoming song plays at the tempo of the outgoing one (within 5%), with its beats lined up. |
| **Bass swap and EQ blend** | Two bass lines never play together; mids and highs cross over gradually. |
| **Phrase alignment** | Mixes start on an 8-bar phrase, and the new song's beat comes in exactly when the bass swaps. |
| **Automatic markers** | Analysis sets the Intro End (where the beat kicks in) and Outro Start (where the energy drops) markers, which the DJ can move. |
| **Beat grid check** | Songs whose beat grid drifts off the music are found, so Auto DJ does not try to beatmatch them. |
| **Quick switch** | When two songs cannot be beatmatched, the change is short and lands on a phrase ending, so two different tempos never play together. |
| **Beat maps and beat lock** | Songs with a live drummer (tempo drifts) can use Mixxx's variable-tempo beat map; during the mix the incoming speed keeps following the outgoing beats. |
| **Key morph** | If two keys clash, the incoming song is pitched by a semitone (tempo unchanged) so the keys fit. |
| **Genre Scan** | Suggests genres for untagged songs from MusicBrainz; the DJ reviews them before anything is saved. |
| **Video mixing** | Music videos play on a second screen or projector, follow each deck (tempo, loops, jumps) and are mixed like the sound, or cut on the beat. Song titles, the DJ's name or logo, and moving pictures for songs without video. The whole video set can be recorded as an MP4. |
| **End of the queue** | Auto DJ stays on while the last song plays, so more songs can still be added; it switches off when that song ends. |
| **Stems (in progress)** | Songs are split into drums, bass, other and vocals by an AI model on the graphics card, in the background, and kept as stem files that Mixxx's stem controls can mute and fade. (Stem mixing controls and stem transitions for Auto DJ are the next steps.) |
| **Own installer** | A Windows installer that installs "Mixxx Auto DJ 2.0 plus Video Mixing" next to a normal Mixxx without touching it. |

---

## 2. Using it

Everything is on the **Auto DJ** page, in the row of buttons above the queue:

| Button | What it does |
|---|---|
| **Shuffle**, **Repeat** | As in Mixxx (Repeat was moved next to Shuffle). |
| **Beatmatch** | Turns the smart transitions on or off (beatmatch, bass swap, EQ blend, phrase alignment). Right-click: key morph limit (off, 1 or 2 semitones). |
| **Skip to Mix** | Jumps to just before the next planned mix, for testing transitions quickly. |
| **Energy** | Rate the energy of the selected songs by hand (1 to 10), or clear the rating. |
| **Smart Sort** | Sorts the queue. Shows the running order, the clashes, and bridge tracks to choose from. |
| **+?** | Adds a random song (as in Mixxx). |
| **Smart Fill** | Adds songs that continue the set. The arrow opens the options: how many, energy direction (build up / keep level / up and down), never the same artist twice, where to take songs from (library, crate or playlist), and "keep the queue filled automatically". |
| **Live Assistant** | Opens the next-song window. Double-click a suggestion to load it on the free deck. |
| **Video** | Show the videos full screen on a screen or projector, in a window, or in a small preview. Also: transitions (crossfade or cut on the beat), song titles, moving pictures, your name or logo, picture timing, graphics-card decoding on/off, **record video** (saves the picture and the mix as one MP4), stop video. |

On the **Analyze** page there is a **Genre Scan** button.

Tips:

- **Analyze** the library first. The analysis measures energy, finds where
  the beat kicks in and checks the beat grids.
- A song whose tempo drifts (live drums, many 1980s records) mixes much
  better with a beat map: right-click it, **Analyze → Reanalyze (variable
  BPM)**.
- Markers set by the DJ always win over the automatic ones.

---

## 3. Building it

Build it like Mixxx: see the Mixxx build guides on the
[Mixxx wiki](https://github.com/mixxxdj/mixxx/wiki). On Windows, in the
"x64 Native Tools Command Prompt for VS 2022":

```
cd /d C:\mixxx
tools\windows_buildenv.bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

Video mixing needs FFmpeg (included in the Mixxx build environment). On
Windows, H.264/H.265 video is decoded by Windows itself (Media Foundation),
because the FFmpeg in the Mixxx build environment has no H.264 decoder.

To make the Windows installer (needs the .NET SDK and **WiX 6.0.2**; WiX 7
asks you to accept its "Open Source Maintenance Fee" licence first):

```
dotnet tool install --global wix --version 6.0.2
wix extension add --global WixToolset.UI.wixext/6.0.2
cd build
cpack -G WIX
```

The result is `build\mixxx-autodj2-video-mixing-<version>-amd64.msi`.

**The stems engine** (song splitting, about 2 GB, not in git) is a folder
`build\stems-engine` next to `mixxx.exe`. When it is there, the build
setup puts it into the installer, without NVIDIA's files (see below). It is
made from Python packages:

```
python -m venv onnx-env
onnx-env\Scripts\pip install onnxruntime-gpu[cuda,cudnn]==1.30.0
```

- `onnxruntime.dll`, `onnxruntime_providers_shared.dll`,
  `onnxruntime_providers_cuda.dll`: from
  `onnx-env\Lib\site-packages\onnxruntime\capi`
- `cuda\*.dll`: from `onnx-env\Lib\site-packages\nvidia\cu13\bin\x86_64`
  (cudart, cublas, cublasLt, cufft, curand, nvrtc, nvrtc-builtins,
  nvJitLink) and `...\nvidia\cudnn\bin` (all cuDNN 9 DLLs)
- `model\htdemucs.onnx`: Demucs v4 "htdemucs" exported to ONNX with the
  Mixxx project's conversion (github.com/mixxxdj/demucs)
- `licenses\`: the licence texts of all of the above and a NOTICE.txt

Without the `cuda` folder the engine runs on the processor (about 4x
faster than playing instead of about 15x).

**setup.exe** (what DJs download, about 400 MB) is a WiX "bundle"
(`packaging/wix/stems/Bundle.wxs`, needs the WiX extensions
`WixToolset.BootstrapperApplications.wixext/6.0.2` and
`WixToolset.Util.wixext/6.0.2`). It installs the Mixxx MSI; on a computer
with an NVIDIA graphics driver (`nvcuda.dll` in System32) it also downloads
and installs the **NVIDIA speed-up** (`packaging/wix/stems/StemsNvidia.wxs`:
the `cuda` folder and `onnxruntime_providers_cuda.dll`, about 1 GB) from the
GitHub release `stems-nvidia-1`. Other computers never download it.

```
build nvidia-pack    (once: makes build\stems-nvidia\MixxxStemsNvidia.msi)
build installer      (the Mixxx MSI, then build\setup\MixxxAutoDJ2-Setup.exe)
```

Upload `MixxxStemsNvidia.msi` once as the file of the GitHub release
`stems-nvidia-1`. Keep that exact file: setup.exe checks that the download
is byte for byte the file it was built with.

---

## 4. How it works

The rule behind every design choice: **the dancers must never lose the
beat.** When in doubt, Auto DJ 2.0 plus Video Mixing does the safe thing (a short, clean
change) rather than a risky one (a long mix that may drift).

Most of the logic is written as small classes without Mixxx or database
types (`src/library/autodj/smart/`, `src/analyzer/energycalculator.*`,
`src/video/videomix.*`), so it can be unit-tested. `AutoDJProcessor`,
`DlgAutoDJ` and the analyzers connect that logic to Mixxx.

### 4.1 Track features

`TrackFeatures` is a small copy of what the mixing decisions need: BPM, key
(on the Camelot wheel, e.g. "8A"), energy (and whether the DJ rated it),
duration, artist, title, genre, whether it is a video, and whether its beat
grid drifts. It is copied out of Mixxx's `Track` objects on the GUI thread,
so sorting can run on a worker thread without touching the database.

### 4.2 The mix score: how well two tracks follow each other

`MixScorer::score(from, to)` gives a cost; lower is better. It is the sum of
three parts, each with a weight:

- **Key** (Camelot wheel):
  same key 0; same number other letter 0.5; one step 1; one step and other
  letter 2; further steps 4, 6, 8, 10, 12. The common DJ "energy boost"
  moves (+2 steps, or +7 steps = +1 semitone, same letter) cost only 2 when
  the energy really rises. Unknown key: 2.5.
  *Why:* these are the standard harmonic-mixing rules; anything beyond one
  step is audible as a clash.
- **Tempo:** the difference is measured as a log ratio, so speeding up and
  slowing down by the same amount cost the same. Within the tolerance (5%)
  the cost is 0 to 1; beyond it the mix is a **clash** (cost 5 and up).
  Half and double time count (e.g. 72 under 144 BPM).
  *Why 5%:* beyond that, the change of tempo is audible to the crowd. A plain
  percentage (the first version) made going up dearer than going down, and
  every sort ran from fast to slow.
- **Energy**, depending on the chosen direction:
  *build up* - a drop costs 1.5 per point, a rise of more than 2 points
  costs the excess; *keep level* - 0.75 per point either way; *up and down*
  - only jumps of more than 1.5 points cost.
  A **measured** energy counts for only a quarter of a **rated** one
  (`measuredEnergyTrust = 0.25`), because in testing the measured energy
  often disagreed with the DJ's ears.

A key cost of 3 or more, or a tempo cost of 5 or more, is reported as a
**clash**.

### 4.3 Smart Sort: the running order

`SmartSequencer` finds the order of the queue with the lowest total cost.
This is the "travelling salesperson path" problem:

- Up to 16 songs: the exact best order (Held-Karp dynamic programming).
- More songs: several greedy starts, then improved by 2-opt and Or-opt moves
  and random "kick and polish again" rounds until the time budget (2 s) is
  used.
- The first and last song also carry a **set shape** cost: when building up,
  a calm opener and a peak at the end are preferred.
- If a song is playing, it stays first.

What remains is reported to the DJ: the whole running order, each clash with
its reason, and for each clash the bridge tracks from section 4.4. Songs
whose beat grid drifts are marked: "no beatmatch: quick switch".

### 4.4 Bridge tracks

For a clash A → B, `BridgeFinder` looks in the library for a song X such that
both A → X and X → B mix without a clash. If no single song works (for
example a tempo jump of more than about 10%, since each step may change at
most 5%), it looks for **two** songs X → Y. Up to 80 candidates per side are
checked.

- In a video set, a bridge that is not a video costs extra (the screen would
  show cover art in the middle of the videos).
- Songs whose beat grid drifts are never bridges (a bridge exists to be mixed
  smoothly).
- The DJ chooses the bridge per gap in the Smart Sort window; nothing is added
  without the DJ's OK.

### 4.5 Smart Fill

Smart Fill adds a chain of songs after the last one in the queue, each the
smoothest next mix from the one before. To avoid the same fill every time,
it picks at random among the few candidates within 1.0 of the best cost
(at most 6). Rules:

- no key or tempo clash;
- a third song in a row in the same key costs extra, so the set moves around
  the wheel;
- genres: same genre or family costs nothing, related families a little,
  unrelated ones more;
- optionally never the same artist twice in a row;
- never the same **song** twice: versions and covers count as the same song
  (the title is compared after removing "The", brackets, "Remix/Edit/Version"
  endings, "feat." and so on);
- with an empty queue it starts with a calm song, so the set has room to
  build.

"Keep the queue filled automatically" runs Smart Fill when the queue gets
short; Mixxx's random fill is only used if Smart Fill finds nothing.

### 4.6 Live Assistant

A small window for DJs who mix by hand. It follows the deck that is playing
now (Mixxx's "current playing deck") and lists the best next songs: no clash,
ranked by the mix score plus the genre cost. It leaves out songs on the decks,
songs already played this session (kept in memory, because Mixxx writes "played"
to the database only when a track is saved), other versions of those, and
the same artist. There is no randomness: the same situation gives the same
list.

### 4.7 Energy analysis

`EnergyCalculator` (run by `AnalyzerEnergy` during analysis) measures three
things from the audio, each scaled 0 to 1:

- **loudness** - the 90th percentile of 1-second levels;
- **brightness** - the share of energy above about 2.5 kHz (hats, synths);
- **busyness** - onsets per second.

energy = 1 + 9 × (0.45 × loudness + 0.30 × brightness + 0.25 × busyness).

It also finds the **body** of the song: where the main beat kicks in (the
drums, not just a loud intro) and where the energy starts to drop at the end.
These become the automatic **Intro End** and **Outro Start** markers, snapped
to the beat grid with the same rules Auto DJ uses. A marker the DJ has set or
moved is never touched.

*Why the DJ rating matters more:* the measured energy is a starting point.
In testing it matched the DJ's own ranking poorly, so a rated energy always
wins and a measured one counts for a quarter.

### 4.8 Beat grid check

A beatmatched mix only works if the beat grid really stays on the beat. Many
songs with live drums have a steady grid that drifts off the music. The check
(`EnergyCalculator::gridDriftBeats`) looks at where in the beat the bass hits
and the treble hits (hats, snares) fall, in 16-beat windows at the start,
middle and end of the song's body:

- with a good grid that place stays put (it does not matter where it is: an
  off-beat bass line is fine, as long as it stays off-beat);
- with a wrong tempo, or a drummer who drifts, it wanders.

The result is how far it wanders, in beats. **More than 0.15 beats** (75 ms at
120 BPM, an audible flam) and Auto DJ does not beatmatch that song.

In each region of four windows, the **one window that does not fit is left
out** and the other three must agree (version 5 of the check). A single odd
window is usually the music changing (a breakdown, an intro before the bass
comes in), not the grid; a grid that drifts moves all the windows. Tested on
reference songs: correct grids 0.03 to 0.14 beats; a grid only 0.05% off in
tempo already 0.15 or more.

The result is stored per song together with the grid it was made on, so it is
done again if the DJ changes the grid. Earlier results stay valid until a song
is analysed again, so no song that was flagged is beatmatched unchecked.

### 4.9 Transitions: beatmatch, bass swap, EQ

When a mix starts (`AutoDJProcessor::beginSmartTransition`):

1. **Tempo:** if the two tempos are within 5% (or half/double time), the
   incoming deck plays at the outgoing tempo, with key lock on (no pitch
   change) and quantize on. Its beats are lined up with Mixxx's beat sync
   (phase) when it starts.
2. **Bass swap:** during the first half of the fade the incoming bass is cut;
   at the middle the bass swaps hard; in the second half the outgoing bass is
   cut. *Why:* two bass lines together sound muddy and hide the beat.
3. **EQ blend:** mids and highs cross over gradually (down to about −10 dB),
   relative to how the DJ had set them. Everything is put back afterwards.
4. **Glide back:** after the mix the new song eases back to its own tempo over
   30 seconds, too slowly to hear.

If the tempos are too far apart, or a beat grid drifts, there is no
beatmatch: see 4.11. Whether a mix is beatmatched is decided once, by the
phrase plan (4.10), which also places the incoming song for it; the mix
itself follows that decision. (Deciding twice, a moment apart, could
disagree for tempos right at the 5% limit.) A song that is still easing back
to its own tempo after the previous mix is judged at its own tempo, since it
will be there when the mix starts.

**End of the queue:** when the queue runs empty, Mixxx normally switches
Auto DJ off at once, and songs added afterwards are never played. Here Auto
DJ stays on while the last song plays: songs added in the meantime are
loaded and mixed as usual. When the last song ends with nothing to follow,
Auto DJ switches itself off.

### 4.10 Phrase alignment

Dance music is built in phrases of 8 bars (32 beats). A mix that starts in
the middle of a phrase sounds wrong even when the beats match.
`phrasealign::plan()`:

- counts phrases from the first beat of each song's grid;
- picks, on the outgoing song, the **last** phrase start from which a whole
  fade still ends before its energy drops (Outro Start marker, or the end of
  the body);
- starts the incoming song so that its beat kicks in (Intro End marker) at
  the **middle** of the fade, exactly when the bass swaps, so the outgoing
  beat hands over straight to the incoming beat;
- makes the fade a whole number of phrases (about the fade time the DJ set);
- is planned again when something changes (a tempo step, the DJ moving a
  marker, an analysis finishing).

**Fade Now** waits for the next phrase start instead of fading at once.
**Skip to Mix** jumps to just before the planned mix.

### 4.11 Mixes that cannot be beatmatched: quick switch

When two songs are more than 5% apart in tempo, or a grid drifts, their beats
must not play together. `phrasealign::planUnmatched()` still ends the fade on
an outgoing phrase ending, but the new beat comes in as the fade **ends**, and
only the new song's intro plays under the outgoing beat. If the intro is
shorter than the fade, the fade is shortened to fit it, down to a **quick
switch of one bar**. An incoming song **without a beat grid** is handled the
same way (it cannot be beatmatched).

**Safety net:** if the outgoing song has **no beat grid** at all (never
analysed, or a broken file) but the next one has, phrases cannot be found.
Instead of Mixxx's long default crossfade (two beats that do not match, for
many seconds), Auto DJ makes a short switch of 4 seconds just before the
song's outro (or its end). When **neither** song has a beat grid, nothing is
known about either beat, and Mixxx's own timing is kept. *Why:* a short, clean change on the phrase is what a DJ
does with two songs that do not match; a long crossfade of two tempos is the
worst case for dancers.

### 4.12 Beat maps and beat lock: songs whose tempo drifts

Mixxx can make a **beat map** that follows a drifting tempo (preference
"Assume constant tempo" off, or "Reanalyze (variable BPM)" on a song).
Auto DJ 2.0 plus Video Mixing uses the real beat positions everywhere:

- `phrasealign::Grid` holds either a steady grid or the time of every beat.
  All phrase maths counts **beats**, so phrases are found on the song's own
  beats even when the tempo moves.
- The grid check follows the beat map beat by beat (a drifting song with a
  good beat map passes).
- The beatmatch decision uses the tempo each song has **where the mix
  happens**, not its average.
- **Beat lock:** when either song has a beat map, the incoming speed is set
  again on every update of the mix (`beatmatch::followRatio`): the same beat
  length as the outgoing song now, plus a small nudge (at most 2%) that pulls
  a beat that slipped back in line within about four beats. Slips smaller
  than 1% of a beat are left alone, so the speed does not wobble. In a real
  mix of two drifting songs the beats stayed within 0.015 of a beat
  (about 8 ms).

### 4.13 Key morph

If two beatmatched songs' keys clash, the incoming song is pitched by the
smallest shift that makes the keys fit (key lock keeps the tempo). One
semitone is 7 steps round the Camelot wheel. The limit is 1 semitone by
default (0, 1 or 2 by right-clicking the Beatmatch button). The song keeps
that key until it has played out: gliding the pitch back would be heard as
the song going out of tune. The next mix uses the shifted key.

### 4.14 Genre Scan

Many songs have no genre tag, and Smart Fill uses genres. **Genre Scan** (on
the Analyze page) looks up the selected songs on
[MusicBrainz](https://musicbrainz.org): the recording's genre, or else the
artist's genre. It keeps to MusicBrainz's limit of one request per second and
identifies itself with Mixxx's own User-Agent (no personal data is sent). The
DJ sees every suggestion in a review window, can change or untick it, and only
the ticked ones are saved.

### 4.15 Video mixing

For video DJ sets (`src/video/`):

- Each deck has a video decoder on its own thread. It shows the frame for the
  deck's **current position**, so tempo changes, beatmatching, loops, jumps
  and scratching carry the picture along. The audio is never touched: if the
  video falls behind, only an older picture is shown.
- **Decoders:** on Windows the Windows decoder (Media Foundation) first, which
  can decode H.264, H.265, VP9 and more, on the graphics card when possible
  (Direct3D 11, with a fallback to the processor); then FFmpeg for other
  formats. The Mixxx build environment's FFmpeg has no H.264 decoder, which
  is why the Windows decoder comes first.
- **Mixing:** each deck's picture is weighted by its volume fader and the
  crossfader (the same crossfader curve as the sound), so every mix, by hand
  or by Auto DJ, is also a video crossfade. Paused decks are hidden. A song
  without video shows its cover art and title.
- **Picture timing:** projectors and TVs often show the picture late. A slider
  moves the picture up to 500 ms earlier or later than the sound.
- **Loops and jumps back:** the last few seconds of pictures (up to 192 MB per
  deck) are kept, so a loop or a jump back shows the right picture at once
  instead of decoding again from the last key frame. After a real jump, the
  pictures on the way from the key frame to the target are not shown (no
  flash of the wrong picture).
- Output: 1920×1080, full screen on any screen, in a window, or a small
  preview.
- **Transitions:** "crossfade with the mix" (above), or **"cut on the
  beat"**: one video at a time. The new song's video takes over when it is
  clearly louder than the old one, on the new song's next beat (Mixxx's
  `beat_active`), or after at most one second. During an Auto DJ mix the
  crossfader passes the middle exactly at the bass swap, so the picture cuts
  where the new song takes over. A small margin stops the picture flickering
  between two decks that are about equally loud.
- **Song titles:** "Artist - Title" appears at the bottom for 7 seconds
  (fading in and out) whenever a new song takes over the screen.
- **Your name or logo:** a text and/or a picture file, shown in the top right
  corner all the time.
- **Moving pictures:** a song without video shows its cover art and title;
  while it plays, the picture grows about 4% on every beat and settles back
  (like a speaker cone), so the screen moves with the music.
- **Record video** (Windows): saves exactly what the video screen shows
  (titles, logo, cuts) together with the mix as one MP4 file (H.264 video,
  1920x1080, 30 pictures a second; AAC sound, 192 kbit/s), using the encoder
  built into Windows (Media Foundation), on the graphics card when it has
  one. It works with or without a video window open. The Video button shows
  "Video (REC)" while recording.
  - **Mixxx's REC button** does it too: if the video is showing when REC is
    pressed, an MP4 with the same name is saved next to the sound file, and
    pressing REC again stops both. With the video off, REC records the sound
    only, as before. (It watches `[Recording],status`; Mixxx's recording
    code is not changed.)
  - The sound is the same mix Mixxx's own Record button saves. It comes from
    Mixxx's recording side channel (`src/video/videoaudiotap.*`), so the
    audio engine is not changed except for two frame counters in
    `EngineSideChain`.
  - Picture and sound share one clock: the number of sound frames the engine
    has made. The side channel delivers the sound in batches (up to about
    half a second late), each tagged with its frame number; each picture is
    stamped with the engine's frame count when it was drawn, minus the
    picture timing. So they stay in step however late either arrives.
  - The file is written on its own thread (`src/video/videorecorder.*`). If
    it falls behind, pictures are dropped (the one before stays a little
    longer), never the sound. The log says how many.
  - The sound must run at 44100 or 48000 Hz (an AAC limit).

### 4.16 The Windows installer

The installer is a separate product, **Mixxx Auto DJ 2.0 plus Video Mixing**, so it can be
installed next to a normal Mixxx:

- its own install folder, shortcuts and installer ID (WiX upgrade code). The
  Mixxx installer contains an action that removes older Mixxx installations;
  here it only looks for older installations of this fork;
- its own settings and library folder, `%LOCALAPPDATA%\Mixxx Auto DJ 2.0 plus Video Mixing`
  (normal Mixxx uses `%LOCALAPPDATA%\Mixxx`), so it never changes a normal
  Mixxx's settings or library. Installs made before the fork was renamed used
  `%LOCALAPPDATA%\Mixxx Auto DJ 2.0`; that folder stays in use until the new
  one exists, so nothing is lost.
- the window title and the About box say "Mixxx - Auto DJ 2.0 plus Video
  Mixing".
- the stems engine (ONNX Runtime + the Demucs model, about 0.35 GB) is in
  the installer when it was there at build time (its own cabinet,
  `CPACK_WIX_CAB_PER_COMPONENT`: one Windows cabinet holds at most about
  2 GB). NVIDIA's CUDA / cuDNN files are the separate NVIDIA speed-up that
  setup.exe downloads only for NVIDIA computers (see Building it). They are
  passed on as runtime files of this application under NVIDIA's licences
  (only used by this application, not offered on their own; the download
  is part of this application's setup); the licence texts are in
  `stems-engine\licenses`.

---

### 4.17 Stems: splitting songs into parts

Done in 6 steps: listening test, splitting engine, decks play the parts,
DJ stem controls, Auto DJ stem mixes, waveforms and installer. Songs are split into
four parts - drums, bass, other (synths, guitars, melody) and vocals - by
Demucs v4 ("htdemucs", Meta, MIT license), the model the Mixxx project
converted to ONNX (github.com/mixxxdj/demucs).

- **When:** a song loaded into a deck is split at once, in the background;
  the next 3 songs of the Auto DJ queue are split ahead of time. One song at
  a time, on its own thread; the music is never held up.
- **Where the parts go:** `Artist - Title [code].stem.mp4` in a folder
  named "Mixxx Stems", the NI stem format Mixxx already plays (5 AAC
  tracks: the mix, then the four parts, plus the stem manifest). Auto DJ >
  Stems chooses where: in one folder (default: the settings folder; "Choose
  the folder..." for a big or USB drive) or next to each song (the parts
  travel with the songs; if that folder cannot be written, the one folder is
  used). The code comes from the song's file name, size and date - not its
  path - so a USB drive with a new drive letter still finds its parts. Both
  places are checked. The library scanner skips "Mixxx Stems" folders.
  Each song is split only once.
- **Decks play the parts** (`src/stems/stemcache.*`, `CachingReaderWorker`):
  when a song is loaded and its stem file is ready, the deck reads the stem
  file instead (only if sample rate and length match), so the LateNight
  stem panel appears with a volume and mute per part. Cue points, beat grid
  and loops stay where they are (the stem file keeps the song's own sample
  rate; checked: 0 samples offset). If the parts finish while the song sits
  in a stopped deck, it is reloaded at the same position; with Auto DJ on,
  only when the next mix is more than 10 s away
  (`AutoDJProcessor::secondsUntilMix`).
- **STEM SPLIT** (Analyze page, next to Genre Scan, and right-click >
  Analyze > Stem Split): splits the selected songs (or the whole list) in
  the background, after the songs in the decks and the Auto DJ queue. It
  first shows the time (from this computer's own measured speed) and the
  disk space (about 120 KB per second of music) and refuses if the drive is
  too full. The button shows "Stem Split n/N - time left"; click again to
  stop (the song being split is dropped too). At the end, songs that could
  not be split are listed with the reason.
- **Safety:** each file is made in the Windows temp folder first and moved
  (or copied) to its place when finished, so slow network shares, cloud
  folders and USB drives do not slow the split. A song that takes more than
  6 times the expected time (at least 3 minutes) is skipped, so one bad song
  never blocks the others. Mono songs are split (the one channel on both
  sides); songs that are not 44.1 or 48 kHz are skipped (the Windows AAC
  encoder only takes those rates, and the parts must line up with the song).
- **DJ stem controls** (`src/stems/stemcontrols.*`, LateNight 2-deck
  mixer row `mixer/stem_controls.xml`, `mixer/stem_knob.xml`): per deck
  three knobs - VOC (vocals), INST (bass + other) and DRUM - whose names
  are kill buttons (short click toggles, hold = kill while held). They set
  the four part volumes (`[ChannelN_StemM],volume`) only when turned, so the
  stem panel's own four knobs keep working too; the engine ramps every gain
  change, so there are no clicks. **ECHO OUT** puts Mixxx's Echo on the
  vocals' own quick-effect slot (send fully open), cuts the vocals on the
  next beat and lets the echo ring out for 4 bars, then puts the DJ's own
  vocal effect back (the part's gain is applied before its effect, so the
  tail keeps sounding). **VOCAL SWAP** (middle of the mixer) fades the
  vocals of the louder playing deck out and of the other playing deck in
  over 4 bars, starting on the next beat, on an equal-power curve. A new
  song resets the deck's controls; songs without parts ignore them
  (`stem_ready` = 0). Controls for MIDI mapping: `[ChannelN]` `stem_vocals`,
  `stem_instrumental`, `stem_drums` (0-1), `stem_vocals_kill`,
  `stem_instrumental_kill`, `stem_drums_kill`, `stem_echo_out`,
  `stem_echo_out_active`, `stem_ready`; `[Stems]` `vocal_swap`,
  `vocal_swap_active`, and `stem_bass` (not on screen: the bass inside the
  instrumental, used by Auto DJ). The 4-deck mixer does not show them yet.
- **Auto DJ stem mixes** (`AutoDJProcessor::beginSmartTransition` /
  `updateSmartTransition`, `beatmatch::stemBlend`, unit tested): when a mix
  is beatmatched and both songs have their parts, the parts cross over
  instead of the EQ: first half the incoming instrumental (synths, no bass)
  rises while the outgoing vocals fall; in the middle drums + bass swap hard
  on the same beat as the bass swap; second half the outgoing instrumental
  falls while the incoming vocals rise. Two vocals never play together and
  two basslines or kicks never clash. The EQ is left as the DJ set it; the
  stem knobs move on screen and are put back afterwards (the DJ's own part
  levels and kills are kept as the starting point). When a mix is not
  beatmatched (quick switch or plain fade) and the outgoing song has its
  parts, its vocals leave with ECHO OUT, which covers the change. Songs
  without parts: the EQ mix as before. Switch: Auto DJ > Stems > "Use the
  parts in Auto DJ mixes" (`[Stems],AutoDJStems`, on by default).
  Limit: Auto DJ does not yet know where a song has singing.
- **Waveforms with the parts:** when a deck loads a song whose parts are
  ready, the analyzer makes its waveform once more from the stem file
  (`AnalyzerThread::analyzeStemWaveform`), so the deck shows the parts in
  their colours like a real stem file. Only the waveform: BPM, key,
  loudness and energy stay those of the song itself. Such a waveform is
  marked "[parts from the stem file]" in its description; a waveform that
  claims parts without that mark (and is not from a real stem file) is
  made again.
- **How:** the song is read exactly as the deck plays it, converted to
  44100 Hz if needed (windowed-sinc resampler), normalised, and cut into
  7.8 s pieces that overlap by a quarter; the model's answers are blended
  with a triangle weight (as Demucs does). Only one piece is kept in memory;
  finished audio is written as it goes (`src/stems/stemmath.*`, unit
  tested).
- **The engine is not built into Mixxx.** `src/stems/stemengine.cpp` loads
  Microsoft's ONNX Runtime (`onnxruntime.dll`) at run time from the
  `stems-engine` folder next to `mixxx.exe`, with the model in
  `stems-engine/model/htdemucs.onnx`. With NVIDIA's CUDA files in
  `stems-engine/cuda/` it runs on the graphics card; otherwise on the
  processor, using half the cores so the audio keeps running smoothly.
  Without the folder, stems are simply off. Only ONNX Runtime's C header is
  in the source (`lib/onnxruntime/include`, MIT license).
- **Speed (RTX 5080 laptop):** a 4-5 minute song in 16-19 s including
  reading and writing (about 15x faster than it plays); the engine starts in
  about 5 s. On the processor about 4x faster than playing.
- **Why CUDA and not DirectML:** DirectML (Windows' own AI layer) could not
  run this model on the NVIDIA chip with any setting ("not enough memory"),
  so CUDA is used.
- The stem files are written with the AAC encoder built into Windows
  (Media Foundation, `src/stems/stemfilewriter.*`).

## 5. Where the code is

| Path | What |
|---|---|
| `src/library/autodj/smart/trackfeatures.*` | The track data used for mixing decisions. |
| `src/library/autodj/smart/mixscorer.*` | The mix score (key, tempo, energy, set shape, key morph). |
| `src/library/autodj/smart/smartsequencer.*` | Smart Sort (the running order) and its report. |
| `src/library/autodj/smart/bridgefinder.*` | Bridge tracks, Smart Fill, Live Assistant suggestions, genre families, same-song detection. |
| `src/library/autodj/smart/beatmatch.*` | Tempo ratio, glide back, bass swap, EQ blend, beat lock. |
| `src/library/autodj/smart/phrasealign.*` | Beat grids and beat maps, phrase-aligned plans, quick switch, Fade Now. |
| `src/library/autodj/smart/energystore.*` | Database tables for energy, song body, automatic markers, grid checks, DJ ratings. |
| `src/library/autodj/smart/genrescan.*` | Genre Scan logic (choosing a genre from MusicBrainz answers). |
| `src/library/autodj/genrescanner.*` | Genre Scan network requests (MusicBrainz, 1 per second). |
| `src/library/autodj/autodjprocessor.*` | Connects it all to Mixxx's Auto DJ: transitions, phrase plans, beat lock, key morph, Smart Fill, Live Assistant data. |
| `src/library/autodj/dlgautodj.*` | The Auto DJ page: buttons, Smart Sort window, Smart Fill options, Live Assistant window, Video menu. |
| `src/library/analysis/dlganalysis.*` | The Genre Scan button and review window on the Analyze page. |
| `src/analyzer/energycalculator.*` | Energy score, song body, beat grid check (pure maths). |
| `src/analyzer/analyzerenergy.*` | Runs the energy calculator during analysis, sets the automatic markers, reads beat grids and beat maps. |
| `src/stems/` | Stems: splitting songs into drums, bass, other and vocals (maths, AI engine loader, stem file writer, background splitter, where the stem files live). Also touched: `CachingReaderWorker` and `SoundSourceProxy` (decks read the stem file), `DlgAnalysis` and `WTrackMenu` (STEM SPLIT), `DlgAutoDJ` (Stems menu). |
| `src/video/` | Video decoding (Windows decoder, FFmpeg), video mixing, video windows. |
| `src/library/playlisttablemodel.*` | Small additions for reordering the Auto DJ queue. |
| `src/util/cmdlineargs.cpp` | The separate settings folder on Windows. |
| `CMakeLists.txt`, `packaging/` | New source files, FFmpeg components, installer name and ID. |

The code comments explain the reasons for the details (the "why"), not only
what the code does.

---

## 6. Stored data

Auto DJ 2.0 plus Video Mixing adds its own tables to Mixxx's library database
(`mixxxdb.sqlite`). It does not change Mixxx's own tables.

| Table | What |
|---|---|
| `autodj_energy` | Measured energy, loudness, brightness, busyness, song body, analysis version. |
| `autodj_auto_markers` | Where the analysis put the Intro End / Outro Start markers (so markers the DJ moved are recognised and never overwritten). |
| `autodj_grid_check` | Beat grid check result, the grid it was made on, and the version of the check. |
| `autodj_energy_manual` | The DJ's own energy ratings. |

Settings are stored in Mixxx's settings file under `[Auto DJ]` (Smart Fill
options, key morph limit and so on) and `[Video]` (picture timing, graphics
card, the folder of the last video recording).

---

## 7. Tests

Unit tests are in `src/test/` and run with the other Mixxx tests:

```
mixxx-test --gtest_filter=TrackFeaturesTest.*:MixScorerTest.*:SmartSequencerTest.*:EnergyCalculatorTest.*:BridgeFinderTest.*:BeatmatchTest.*:PhraseAlignTest.*:AutoDJProcessorTest.*:GenreScanTest.*:VideoMixTest.*:StemMathTest.*
```

150 tests. The energy and grid-check tests use synthetic drum loops (steady,
drifting, off-beat bass, a drummer who speeds up, one odd stretch) so the
expected answer is known. Mixes, video and analysis were also tested by ear
and eye on a real library of mostly 1980s new wave, synth-pop, EBM and goth
music, many of them music videos.

---

## 8. Known limits

- Energy weights, energy ranges and the intro detection were tuned on a small
  number of songs. The DJ's own energy rating is the better guide.
- Phrases are counted from the first beat of the grid. If the grid's first
  beat is not a downbeat (common), phrases are off by a few beats; the DJ can
  move the grid.
- Assumes 4/4 time.
- A song with no beat grid gets a short 4-second switch (or, if neither song
  has one, Mixxx's own timing), not a phrase-aligned mix.
- Beat lock needs a beat map on at least one of the two songs; steady grids
  rely on the one-time beatmatch.
- Video: the picture follows the deck, but the video is not time-stretched
  (at +5% the picture simply runs 5% faster, which is what you want).
- Video recording is Windows only, and needs the sound at 44100 or 48000 Hz.
- Stems: Windows only for now; the engine folder (about 1.8 GB with the NVIDIA files) is not part of the source and is not yet in the installer. Stem files are always 44100 Hz.
- Tested mainly on Windows 11.

---

## 9. Change history

Oldest first. Each entry is one commit on the `autodj-2` branch.

1. **MixScorer and SmartSequencer** - the mix score and the exact / heuristic
   sorter.
2. **Smart Sort button** on the Auto DJ page.
3. **Clearer Smart Sort report** - the running order with clash reasons.
4. **Energy analyzer** - energy score from loudness, brightness, busyness.
5. **DJ energy rating** - hand ratings win; measured energy counts less.
6. **Set shape, full running order, symmetric tempo cost** (log ratio).
7. **Tempo changes over 5% count as a clash.**
8. **Bridge-track finder.**
9. **Beatmatched transitions with bass swap.**
10. **Phrase-aligned transitions**, deck tempo reset.
11. **Beat-aware transitions** - song body detection, Intro End marker.
12. **Analysis sets Intro End / Outro Start markers.**
13. **Drum-aware intro detection, Skip to Mix, unmatched mixes, glide fix.**
14. **Phrase-aligned Fade Now, bridge choice per gap, Smart Fill.**
15. **Smart Fill options, song duplicates, genre families, crate source.**
16. **Genre Scan** (MusicBrainz suggestions, reviewed by the DJ).
17. **Automatic Smart Fill, full EQ transition, beat grid check.**
18. **Grid check v3** - a "double kick" rule was tried and removed (it flagged
    more steady songs); back to counting every hit.
19. **Key morph.**
20. **Live Assistant.**
21. **Video mixing, phase 1** - Windows decoder, FFmpeg fallback, mixing by
    faders and crossfader, video windows.
22. **Quick switch, two-track bridges, beat maps and beat lock.**
23. **Fairer grid check (v5)** - one odd window is left out; Auto DJ plans
    the mix again when an analysis finishes.
24. **Video mixing, phase 2** - graphics-card decoding, picture timing,
    remembered pictures for loops and jumps back.
25. **Own Windows installer** next to a normal Mixxx.
26. **Repeat button next to Shuffle.**
27. **This documentation.**
28. **Renamed to "Auto DJ 2.0 plus Video Mixing"** (installer, install
    folder, settings folder, window title, About box, code comments, docs).
29. **Safety net for songs without a beat grid** (short switch) and **video
    phase 3**: cut on the beat, song titles, name or logo, moving pictures.
30. **End of the queue**: Auto DJ stays on until the last song ends; the mix
    follows the phrase plan's beatmatch decision; updated tooltips.
31. **Video recording**: "Record video..." saves the mixed picture and sound
    as one MP4 (Windows encoder), in step through the engine's frame count.
    Mixxx's REC button also records the video while it is showing.
32. **Stems, step 2: the splitting engine.** Songs in the decks and the
    next songs of the Auto DJ queue are split into drums, bass, other and
    vocals on the graphics card (Demucs v4 via ONNX Runtime + CUDA, loaded
    at run time) and kept as stem files.
33. **Stems, step 3: decks play the parts.** Loaded songs switch to their
    stem file (stem panel with mute/volume per part, markers kept); a
    stopped deck reloads when its parts finish (dynamic: not within 10 s of
    an Auto DJ mix). Choice of where the parts go (one folder or next to
    each song, USB/network friendly). STEM SPLIT button and track menu item
    for splitting many songs in the background with time and space check,
    progress and stop. Made on this computer first, time limit per song,
    mono songs supported, unusual sample rates skipped with a reason.
34. **Stems, step 4: DJ stem controls.** VOC / INST / DRUM knobs with kill
    buttons per deck in the LateNight mixer, ECHO OUT (vocals leave with an
    echo that rings out) and VOCAL SWAP (vocals move to the other deck over
    4 bars on the beat). The first-sound check is skipped for songs playing
    from their parts (false "first sound has been moved" warnings).
35. **Stems, step 5: Auto DJ stem mixes.** Beatmatched mixes of two songs
    with parts: instrumental first, drums + bass swap in the middle, vocals
    never together (stem knobs move, restored afterwards). Unmatched mixes:
    the outgoing vocals echo out. Switch in Auto DJ > Stems.
36. **Stems, step 6: waveforms and installer.** Songs playing from their
    parts get a waveform with the parts in colour. The installer includes
    the stems engine with its licence texts; setup.exe downloads the NVIDIA
    speed-up only on computers with an NVIDIA graphics driver. Fixes found
    on the way: Windows' MP4 reader was asked for 8 channels and made up
    "surround" channels from stereo (MP4 videos then played and were
    analysed as 8 channels, and their waveforms claimed 4 fake parts); it
    now never gets more channels than the file has, and fake-parts
    waveforms are made again. The newest saved waveform is now the one
    kept. An MP4 keeps the parts list its deck set when the file is opened
    again elsewhere (it was wiped, so the deck drew the plain waveform).
