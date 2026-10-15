#pragma once

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <optional>
#include <vector>

#include "analyzer/energycalculator.h"
#include "track/trackid.h"

/// Auto DJ 2.0 plus Video Mixing: saves and loads energy scores.
///
/// Two tables, both created on demand with CREATE TABLE IF NOT EXISTS and
/// not part of Mixxx's versioned schema (so official schema updates cannot
/// collide with them, and official Mixxx ignores them):
///  - `autodj_energy`: measured by AnalyzerEnergy, rewritten on re-analysis
///  - `autodj_energy_manual`: the DJ's own 1..10 rating, never overwritten
///    by analysis. A rating always wins over the measured value.
///  - `autodj_auto_markers`: the Intro End / Outro Start positions the
///    analyzer set itself, so it can tell them apart from markers the DJ set
///    or moved (those are never touched).
///  - `autodj_grid_check`: how far the track's beat grid drifts off the
///    music (EnergyCalculator::gridDriftBeats), and which grid was checked,
///    so a grid the DJ changes later is checked again.
class EnergyStore {
  public:
    static constexpr int kMinRating = 1;
    static constexpr int kMaxRating = 10;

    struct Value {
        double energy = 0.0; ///< 1..10
        bool manual = false; ///< true = rated by the DJ
    };

    /// Creates the table if needed. Safe to call often.
    static bool ensureTable(const QSqlDatabase& db);

  private:
    /// The check itself; ensureTable runs it once per database connection.
    static bool ensureTableNow(const QSqlDatabase& db);

  public:

    /// The formula version stored for this track, if any.
    static std::optional<int> storedVersion(const QSqlDatabase& db, TrackId trackId);

    static bool save(const QSqlDatabase& db,
            TrackId trackId,
            const EnergyCalculator::Result& result,
            int version);

    /// Where the track's body starts and ends (seconds at its own speed),
    /// from analysis v2+. nullopt if not analysed yet.
    struct Body {
        double startSec = 0.0;
        double endSec = 0.0;
    };
    static std::optional<Body> loadBody(const QSqlDatabase& db, TrackId trackId);

    /// Marker positions (seconds) the analyzer set itself; -1 = none.
    struct AutoMarkers {
        double introEndSec = -1.0;
        double outroStartSec = -1.0;
    };
    static AutoMarkers loadAutoMarkers(const QSqlDatabase& db, TrackId trackId);
    static bool saveAutoMarkers(
            const QSqlDatabase& db, TrackId trackId, const AutoMarkers& markers);

    /// Beat grid check result. bpm 0 = the track had no grid.
    struct GridCheck {
        double bpm = 0.0;
        double firstBeatSec = 0.0;
        double driftBeats = -1.0; ///< -1 = could not tell
        int version = 0; ///< EnergyCalculator::kGridCheckVersion (or ...MapVersion)
        /// True if this check was made on that grid (the DJ has not
        /// changed it since), with the current or the previous version of
        /// the check (a previous result counts until the track is analysed
        /// again). `beatMap`: the grid is a beat map now.
        bool isFor(double gridBpm, double gridFirstBeatSec, bool beatMap) const;
        /// Made with the current version of the check.
        bool isCurrent(bool beatMap) const;
    };
    static std::optional<GridCheck> loadGridCheck(const QSqlDatabase& db, TrackId trackId);
    static bool saveGridCheck(const QSqlDatabase& db, TrackId trackId, const GridCheck& check);

    /// Where beat 1 of the bar is (downbeat.h), for the grid it was found on.
    struct Downbeat {
        double bpm = 0.0;
        double firstBeatSec = 0.0;
        int phase = 0; ///< grid beat index % 4 of beat 1
        double margin = 0.0;
        bool sure = false;
        int version = 0;
        static constexpr int kVersion = 3; // 2: must be steady, 3: stricter "sure"
        /// Found on this grid, with the current method.
        bool isFor(double gridBpm, double gridFirstBeatSec) const;
    };
    /// fromParts: beat 1 found on the song's stem parts (bass and chords
    /// without drums and singing, see AnalyzerStemDownbeat), kept apart from
    /// the one found on the whole song.
    static std::optional<Downbeat> loadDownbeat(
            const QSqlDatabase& db, TrackId trackId, bool fromParts = false);
    static bool saveDownbeat(const QSqlDatabase& db,
            TrackId trackId,
            const Downbeat& downbeat,
            bool fromParts = false);

    /// Sets the DJ's rating (1..10) for the tracks. rating 0 clears it, so
    /// the measured value is used again.
    static bool setManualRating(
            const QSqlDatabase& db, const QList<TrackId>& trackIds, int rating);

    /// Energy for the given tracks: the rating if set, else the measured
    /// value. Tracks with neither are left out.
    static QHash<TrackId, Value> loadEnergies(
            const QSqlDatabase& db, const QList<TrackId>& trackIds);

    /// The beat check (beatcheck.h): the beats and downbeats the second
    /// beat finder heard, kept so the review can be shown at any time (it
    /// compares them with the song's grid and markers as they are then).
    struct BeatCheck {
        static constexpr int kVersion = 1;
        int version = 0;
        QString model;             ///< which beat model heard them
        double durationSec = 0.0;  ///< the song's length then
        std::vector<double> beats; ///< seconds
        std::vector<double> downbeats;
        QString ignored;           ///< what the DJ chose to Ignore (see DlgAnalysis)
        /// For trying a proposed grid against the music (the beat grid
        /// check): the bass and treble levels, and the song's body. Empty
        /// for songs checked before they were kept (Analyze adds them).
        EnergyCalculator::GridLevels levels;
        double bodyStartSec = 0.0;
        double bodyEndSec = 0.0;
        bool hasLevels() const {
            return levels.blockSec > 0.0 && !levels.lowDb.empty();
        }
    };
    static std::optional<BeatCheck> loadBeatCheck(const QSqlDatabase& db, TrackId trackId);
    static bool saveBeatCheck(const QSqlDatabase& db, TrackId trackId, const BeatCheck& check);
    static bool setBeatCheckIgnored(const QSqlDatabase& db, TrackId trackId, const QString& ignored);
    /// The songs with a beat check result.
    static QList<TrackId> beatCheckedTracks(const QSqlDatabase& db);

    /// What a beat check Fix changed, to undo it (`autodj_beatcheck_undo`).
    /// Kept from before the first Fix: fixed again, it still goes back to
    /// how the song was before the beat check touched it.
    struct BeatCheckUndo {
        QString fixedAt;            ///< when (local time, for the list)
        QString what;               ///< what was done, for the list
        bool gridSaved = false;     ///< the grid was changed: the old one is here
        QByteArray oldBeats;        ///< Beats::toByteArray(), empty = no grid
        QString oldBeatsVersion;
        QString oldBeatsSubVersion;
        bool oldBpmLocked = false;
        QByteArray newBeatsHash;    ///< the grid the Fix left (to know it is still that one)
        bool introSaved = false;    ///< the Intro End marker was moved
        double oldIntroEndFrame = -1.0;
        double newIntroEndFrame = -1.0;
    };
    static std::optional<BeatCheckUndo> loadBeatCheckUndo(const QSqlDatabase& db, TrackId trackId);
    static bool saveBeatCheckUndo(
            const QSqlDatabase& db, TrackId trackId, const BeatCheckUndo& undo);
    static bool deleteBeatCheckUndo(const QSqlDatabase& db, TrackId trackId);
    static QList<TrackId> beatCheckUndoTracks(const QSqlDatabase& db);
};
