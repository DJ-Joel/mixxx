#include "library/autodj/smart/energystore.h"

#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "library/autodj/smart/beatcheck.h"
#include "library/queryutil.h"

namespace {
// Database connections whose tables are known to be ready. The check below
// is about 8 statements, and every load and save used to run it again.
QMutex s_readyMutex;
QSet<QString> s_readyConnections;
} // namespace

// static
bool EnergyStore::ensureTable(const QSqlDatabase& db) {
    const QString connection = db.connectionName() + QChar('|') + db.databaseName();
    {
        const QMutexLocker lock(&s_readyMutex);
        if (s_readyConnections.contains(connection)) {
            return true;
        }
    }
    if (!ensureTableNow(db)) {
        return false;
    }
    const QMutexLocker lock(&s_readyMutex);
    s_readyConnections.insert(connection);
    return true;
}

// static
bool EnergyStore::ensureTableNow(const QSqlDatabase& db) {
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_energy ("
                "track_id INTEGER PRIMARY KEY, "
                "energy REAL NOT NULL, "
                "loudness_db REAL, "
                "bright_ratio REAL, "
                "onsets_per_sec REAL, "
                "bass_ratio REAL, "
                "version INTEGER NOT NULL)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    // Columns added in analysis v2. Older tables get them added once.
    QSet<QString> columns;
    if (query.exec(QStringLiteral("PRAGMA table_info(autodj_energy)"))) {
        while (query.next()) {
            columns.insert(query.value(1).toString());
        }
    }
    for (const QString& column : {QStringLiteral("body_start_sec"), QStringLiteral("body_end_sec")}) {
        if (!columns.contains(column) &&
                !query.exec(QStringLiteral("ALTER TABLE autodj_energy ADD COLUMN %1 REAL")
                                    .arg(column))) {
            LOG_FAILED_QUERY(query);
            return false;
        }
    }
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_auto_markers ("
                "track_id INTEGER PRIMARY KEY, "
                "intro_end_sec REAL, "
                "outro_start_sec REAL)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_grid_check ("
                "track_id INTEGER PRIMARY KEY, "
                "bpm REAL, "
                "first_beat_sec REAL, "
                "drift_beats REAL)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    // Added with grid check v2. Older rows get NULL = version 0.
    bool hasVersion = false;
    if (query.exec(QStringLiteral("PRAGMA table_info(autodj_grid_check)"))) {
        while (query.next()) {
            hasVersion = hasVersion || query.value(1).toString() == QStringLiteral("version");
        }
    }
    if (!hasVersion &&
            !query.exec(QStringLiteral(
                    "ALTER TABLE autodj_grid_check ADD COLUMN version INTEGER"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_downbeat ("
                "track_id INTEGER PRIMARY KEY, "
                "bpm REAL, "
                "first_beat_sec REAL, "
                "phase INTEGER, "
                "margin REAL, "
                "sure INTEGER, "
                "version INTEGER)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    // Beat 1 found on the stem parts (the same columns).
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_downbeat_parts ("
                "track_id INTEGER PRIMARY KEY, "
                "bpm REAL, "
                "first_beat_sec REAL, "
                "phase INTEGER, "
                "margin REAL, "
                "sure INTEGER, "
                "version INTEGER)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_energy_manual ("
                "track_id INTEGER PRIMARY KEY, "
                "rating INTEGER NOT NULL)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_beatcheck ("
                "track_id INTEGER PRIMARY KEY, "
                "version INTEGER NOT NULL, "
                "model TEXT, "
                "duration_sec REAL, "
                "beats BLOB, "
                "downbeats BLOB, "
                "ignored TEXT)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    // The levels for the beat grid check, added later: older tables get the
    // columns once.
    QSet<QString> beatCheckColumns;
    if (query.exec(QStringLiteral("PRAGMA table_info(autodj_beatcheck)"))) {
        while (query.next()) {
            beatCheckColumns.insert(query.value(1).toString());
        }
    }
    for (const auto& column : {std::pair<QString, QString>(QStringLiteral("block_sec"), QStringLiteral("REAL")),
                 std::pair<QString, QString>(QStringLiteral("low_levels"), QStringLiteral("BLOB")),
                 std::pair<QString, QString>(QStringLiteral("high_levels"), QStringLiteral("BLOB")),
                 std::pair<QString, QString>(QStringLiteral("body_start_sec"), QStringLiteral("REAL")),
                 std::pair<QString, QString>(QStringLiteral("body_end_sec"), QStringLiteral("REAL"))}) {
        if (!beatCheckColumns.contains(column.first) &&
                !query.exec(QStringLiteral("ALTER TABLE autodj_beatcheck ADD COLUMN %1 %2")
                                    .arg(column.first, column.second))) {
            LOG_FAILED_QUERY(query);
            return false;
        }
    }
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_beatcheck_undo ("
                "track_id INTEGER PRIMARY KEY, "
                "fixed_at TEXT, "
                "what TEXT, "
                "grid_saved INTEGER, "
                "old_beats BLOB, "
                "old_beats_version TEXT, "
                "old_beats_sub_version TEXT, "
                "old_bpm_locked INTEGER, "
                "new_beats_hash BLOB, "
                "intro_saved INTEGER, "
                "old_intro_end REAL, "
                "new_intro_end REAL)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

namespace {
// Times as 32-bit floats, little-endian (20 microseconds at 5 minutes).
QByteArray packTimes(const std::vector<double>& times) {
    QByteArray bytes;
    bytes.reserve(static_cast<int>(times.size() * 4));
    for (double t : times) {
        const float f = static_cast<float>(t);
        quint32 bits;
        std::memcpy(&bits, &f, 4);
        for (int k = 0; k < 4; ++k) {
            bytes.append(static_cast<char>((bits >> (8 * k)) & 0xff));
        }
    }
    return bytes;
}
QByteArray packLevels(const std::vector<float>& db) {
    const std::vector<std::uint8_t> bytes = beatcheck::packLevels(db);
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
}
std::vector<float> unpackLevels(const QByteArray& bytes) {
    return beatcheck::unpackLevels(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size()));
}
std::vector<double> unpackTimes(const QByteArray& bytes) {
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(bytes.size() / 4));
    for (int i = 0; i + 3 < bytes.size(); i += 4) {
        quint32 bits = 0;
        for (int k = 0; k < 4; ++k) {
            bits |= static_cast<quint32>(static_cast<unsigned char>(bytes[i + k])) << (8 * k);
        }
        float f;
        std::memcpy(&f, &bits, 4);
        times.push_back(f);
    }
    return times;
}
} // namespace

// static
std::optional<EnergyStore::BeatCheck> EnergyStore::loadBeatCheck(
        const QSqlDatabase& db, TrackId trackId) {
    if (!trackId.isValid() || !ensureTable(db)) {
        return std::nullopt;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT version, model, duration_sec, beats, downbeats, ignored, "
            "block_sec, low_levels, high_levels, body_start_sec, body_end_sec "
            "FROM autodj_beatcheck WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    BeatCheck check;
    check.version = query.value(0).toInt();
    check.model = query.value(1).toString();
    check.durationSec = query.value(2).toDouble();
    check.beats = unpackTimes(query.value(3).toByteArray());
    check.downbeats = unpackTimes(query.value(4).toByteArray());
    check.ignored = query.value(5).toString();
    check.levels.blockSec = query.value(6).toDouble();
    check.levels.lowDb = unpackLevels(query.value(7).toByteArray());
    check.levels.highDb = unpackLevels(query.value(8).toByteArray());
    check.bodyStartSec = query.value(9).toDouble();
    check.bodyEndSec = query.value(10).toDouble();
    return check;
}

// static
bool EnergyStore::saveBeatCheck(const QSqlDatabase& db, TrackId trackId, const BeatCheck& check) {
    if (!trackId.isValid() || !ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO autodj_beatcheck "
            "(track_id, version, model, duration_sec, beats, downbeats, ignored, "
            "block_sec, low_levels, high_levels, body_start_sec, body_end_sec) "
            "VALUES (:id, :version, :model, :duration, :beats, :downbeats, :ignored, "
            ":block, :low, :high, :bodyStart, :bodyEnd)"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":version"), check.version);
    query.bindValue(QStringLiteral(":model"), check.model);
    query.bindValue(QStringLiteral(":duration"), check.durationSec);
    query.bindValue(QStringLiteral(":beats"), packTimes(check.beats));
    query.bindValue(QStringLiteral(":downbeats"), packTimes(check.downbeats));
    query.bindValue(QStringLiteral(":ignored"), check.ignored);
    query.bindValue(QStringLiteral(":block"), check.levels.blockSec);
    query.bindValue(QStringLiteral(":low"), packLevels(check.levels.lowDb));
    query.bindValue(QStringLiteral(":high"), packLevels(check.levels.highDb));
    query.bindValue(QStringLiteral(":bodyStart"), check.bodyStartSec);
    query.bindValue(QStringLiteral(":bodyEnd"), check.bodyEndSec);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
bool EnergyStore::setBeatCheckIgnored(
        const QSqlDatabase& db, TrackId trackId, const QString& ignored) {
    if (!trackId.isValid() || !ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE autodj_beatcheck SET ignored=:ignored WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":ignored"), ignored);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
QList<TrackId> EnergyStore::beatCheckedTracks(const QSqlDatabase& db) {
    QList<TrackId> ids;
    if (!ensureTable(db)) {
        return ids;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT track_id FROM autodj_beatcheck"))) {
        LOG_FAILED_QUERY(query);
        return ids;
    }
    while (query.next()) {
        ids.append(TrackId(query.value(0)));
    }
    return ids;
}

// static
std::optional<EnergyStore::BeatCheckUndo> EnergyStore::loadBeatCheckUndo(
        const QSqlDatabase& db, TrackId trackId) {
    if (!trackId.isValid() || !ensureTable(db)) {
        return std::nullopt;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT fixed_at, what, grid_saved, old_beats, old_beats_version, "
            "old_beats_sub_version, old_bpm_locked, new_beats_hash, intro_saved, "
            "old_intro_end, new_intro_end FROM autodj_beatcheck_undo WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    BeatCheckUndo undo;
    undo.fixedAt = query.value(0).toString();
    undo.what = query.value(1).toString();
    undo.gridSaved = query.value(2).toInt() != 0;
    undo.oldBeats = query.value(3).toByteArray();
    undo.oldBeatsVersion = query.value(4).toString();
    undo.oldBeatsSubVersion = query.value(5).toString();
    undo.oldBpmLocked = query.value(6).toInt() != 0;
    undo.newBeatsHash = query.value(7).toByteArray();
    undo.introSaved = query.value(8).toInt() != 0;
    undo.oldIntroEndFrame = query.value(9).toDouble();
    undo.newIntroEndFrame = query.value(10).toDouble();
    return undo;
}

// static
bool EnergyStore::saveBeatCheckUndo(
        const QSqlDatabase& db, TrackId trackId, const BeatCheckUndo& undo) {
    if (!trackId.isValid() || !ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO autodj_beatcheck_undo "
            "(track_id, fixed_at, what, grid_saved, old_beats, old_beats_version, "
            "old_beats_sub_version, old_bpm_locked, new_beats_hash, intro_saved, "
            "old_intro_end, new_intro_end) VALUES (:id, :at, :what, :gridSaved, :oldBeats, "
            ":oldVersion, :oldSubVersion, :oldLocked, :newHash, :introSaved, :oldIntro, "
            ":newIntro)"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":at"), undo.fixedAt);
    query.bindValue(QStringLiteral(":what"), undo.what);
    query.bindValue(QStringLiteral(":gridSaved"), undo.gridSaved ? 1 : 0);
    query.bindValue(QStringLiteral(":oldBeats"), undo.oldBeats);
    query.bindValue(QStringLiteral(":oldVersion"), undo.oldBeatsVersion);
    query.bindValue(QStringLiteral(":oldSubVersion"), undo.oldBeatsSubVersion);
    query.bindValue(QStringLiteral(":oldLocked"), undo.oldBpmLocked ? 1 : 0);
    query.bindValue(QStringLiteral(":newHash"), undo.newBeatsHash);
    query.bindValue(QStringLiteral(":introSaved"), undo.introSaved ? 1 : 0);
    query.bindValue(QStringLiteral(":oldIntro"), undo.oldIntroEndFrame);
    query.bindValue(QStringLiteral(":newIntro"), undo.newIntroEndFrame);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
bool EnergyStore::deleteBeatCheckUndo(const QSqlDatabase& db, TrackId trackId) {
    if (!trackId.isValid() || !ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM autodj_beatcheck_undo WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
QList<TrackId> EnergyStore::beatCheckUndoTracks(const QSqlDatabase& db) {
    QList<TrackId> ids;
    if (!ensureTable(db)) {
        return ids;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT track_id FROM autodj_beatcheck_undo"))) {
        LOG_FAILED_QUERY(query);
        return ids;
    }
    while (query.next()) {
        ids.append(TrackId(query.value(0)));
    }
    return ids;
}

// static
bool EnergyStore::setManualRating(
        const QSqlDatabase& db, const QList<TrackId>& trackIds, int rating) {
    if (trackIds.isEmpty() || !ensureTable(db)) {
        return false;
    }
    const bool clear = rating == 0;
    if (!clear && (rating < kMinRating || rating > kMaxRating)) {
        return false;
    }
    ScopedTransaction transaction(db);
    QSqlQuery query(db);
    query.prepare(clear
                    ? QStringLiteral("DELETE FROM autodj_energy_manual WHERE track_id=:id")
                    : QStringLiteral(
                              "INSERT OR REPLACE INTO autodj_energy_manual "
                              "(track_id, rating) VALUES (:id, :rating)"));
    for (const TrackId& id : trackIds) {
        query.bindValue(QStringLiteral(":id"), id.toVariant());
        if (!clear) {
            query.bindValue(QStringLiteral(":rating"), rating);
        }
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            return false;
        }
    }
    transaction.commit();
    return true;
}

// static
std::optional<int> EnergyStore::storedVersion(const QSqlDatabase& db, TrackId trackId) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT version FROM autodj_energy WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (query.next()) {
        return query.value(0).toInt();
    }
    return std::nullopt;
}

// static
bool EnergyStore::save(const QSqlDatabase& db,
        TrackId trackId,
        const EnergyCalculator::Result& result,
        int version) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO autodj_energy "
            "(track_id, energy, loudness_db, bright_ratio, onsets_per_sec, bass_ratio, version, "
            "body_start_sec, body_end_sec) "
            "VALUES (:id, :energy, :loudness, :bright, :onsets, :bass, :version, "
            ":body_start, :body_end)"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":energy"), result.energy);
    query.bindValue(QStringLiteral(":loudness"), result.loudnessDb);
    query.bindValue(QStringLiteral(":bright"), result.brightRatio);
    query.bindValue(QStringLiteral(":onsets"), result.onsetsPerSec);
    query.bindValue(QStringLiteral(":bass"), result.bassRatio);
    query.bindValue(QStringLiteral(":version"), version);
    query.bindValue(QStringLiteral(":body_start"), result.bodyStartSec);
    query.bindValue(QStringLiteral(":body_end"), result.bodyEndSec);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
std::optional<EnergyStore::Body> EnergyStore::loadBody(const QSqlDatabase& db, TrackId trackId) {
    if (!ensureTable(db)) {
        return std::nullopt;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT body_start_sec, body_end_sec FROM autodj_energy "
            "WHERE track_id=:id AND body_start_sec IS NOT NULL "
            "AND body_end_sec IS NOT NULL"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    Body body;
    body.startSec = query.value(0).toDouble();
    body.endSec = query.value(1).toDouble();
    if (body.endSec <= body.startSec) {
        return std::nullopt;
    }
    return body;
}

// static
EnergyStore::AutoMarkers EnergyStore::loadAutoMarkers(const QSqlDatabase& db, TrackId trackId) {
    AutoMarkers markers;
    if (!ensureTable(db)) {
        return markers;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT intro_end_sec, outro_start_sec FROM autodj_auto_markers "
            "WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return markers;
    }
    if (query.next()) {
        if (!query.value(0).isNull()) {
            markers.introEndSec = query.value(0).toDouble();
        }
        if (!query.value(1).isNull()) {
            markers.outroStartSec = query.value(1).toDouble();
        }
    }
    return markers;
}

// static
bool EnergyStore::saveAutoMarkers(
        const QSqlDatabase& db, TrackId trackId, const AutoMarkers& markers) {
    if (!ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO autodj_auto_markers "
            "(track_id, intro_end_sec, outro_start_sec) VALUES (:id, :intro, :outro)"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":intro"),
            markers.introEndSec >= 0.0 ? QVariant(markers.introEndSec) : QVariant());
    query.bindValue(QStringLiteral(":outro"),
            markers.outroStartSec >= 0.0 ? QVariant(markers.outroStartSec) : QVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

bool EnergyStore::GridCheck::isCurrent(bool beatMap) const {
    return version ==
            (beatMap ? EnergyCalculator::kGridCheckMapVersion
                     : EnergyCalculator::kGridCheckVersion);
}

bool EnergyStore::GridCheck::isFor(
        double gridBpm, double gridFirstBeatSec, bool beatMap) const {
    const int oldVersion = beatMap ? EnergyCalculator::kGridCheckOldMapVersion
                                   : EnergyCalculator::kGridCheckOldVersion;
    // Tiny differences are rounding in the database, not a new grid.
    return (isCurrent(beatMap) || version == oldVersion) &&
            std::fabs(bpm - gridBpm) < 0.001 &&
            std::fabs(firstBeatSec - gridFirstBeatSec) < 0.002;
}

// static
std::optional<EnergyStore::GridCheck> EnergyStore::loadGridCheck(
        const QSqlDatabase& db, TrackId trackId) {
    if (!ensureTable(db)) {
        return std::nullopt;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT bpm, first_beat_sec, drift_beats, version FROM autodj_grid_check "
            "WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    GridCheck check;
    check.bpm = query.value(0).toDouble();
    check.firstBeatSec = query.value(1).toDouble();
    check.driftBeats = query.value(2).isNull() ? -1.0 : query.value(2).toDouble();
    check.version = query.value(3).toInt(); // NULL -> 0
    return check;
}

// static
bool EnergyStore::saveGridCheck(
        const QSqlDatabase& db, TrackId trackId, const GridCheck& check) {
    if (!ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO autodj_grid_check "
            "(track_id, bpm, first_beat_sec, drift_beats, version) "
            "VALUES (:id, :bpm, :first, :drift, :version)"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":bpm"), check.bpm);
    query.bindValue(QStringLiteral(":first"), check.firstBeatSec);
    query.bindValue(QStringLiteral(":drift"), check.driftBeats);
    query.bindValue(QStringLiteral(":version"), check.version);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

bool EnergyStore::Downbeat::isFor(double gridBpm, double gridFirstBeatSec) const {
    return version == kVersion && std::fabs(bpm - gridBpm) < 0.001 &&
            std::fabs(firstBeatSec - gridFirstBeatSec) < 0.002;
}

// static
std::optional<EnergyStore::Downbeat> EnergyStore::loadDownbeat(
        const QSqlDatabase& db, TrackId trackId, bool fromParts) {
    if (!ensureTable(db)) {
        return std::nullopt;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT bpm, first_beat_sec, phase, margin, sure, version FROM %1 "
            "WHERE track_id=:id")
                    .arg(fromParts ? QStringLiteral("autodj_downbeat_parts")
                                   : QStringLiteral("autodj_downbeat")));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    Downbeat d;
    d.bpm = query.value(0).toDouble();
    d.firstBeatSec = query.value(1).toDouble();
    d.phase = query.value(2).toInt();
    d.margin = query.value(3).toDouble();
    d.sure = query.value(4).toInt() != 0;
    d.version = query.value(5).toInt();
    return d;
}

// static
bool EnergyStore::saveDownbeat(
        const QSqlDatabase& db, TrackId trackId, const Downbeat& d, bool fromParts) {
    if (!ensureTable(db)) {
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO %1 "
            "(track_id, bpm, first_beat_sec, phase, margin, sure, version) "
            "VALUES (:id, :bpm, :first, :phase, :margin, :sure, :version)")
                    .arg(fromParts ? QStringLiteral("autodj_downbeat_parts")
                                   : QStringLiteral("autodj_downbeat")));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":bpm"), d.bpm);
    query.bindValue(QStringLiteral(":first"), d.firstBeatSec);
    query.bindValue(QStringLiteral(":phase"), d.phase);
    query.bindValue(QStringLiteral(":margin"), d.margin);
    query.bindValue(QStringLiteral(":sure"), d.sure ? 1 : 0);
    query.bindValue(QStringLiteral(":version"), d.version);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
QHash<TrackId, EnergyStore::Value> EnergyStore::loadEnergies(
        const QSqlDatabase& db, const QList<TrackId>& trackIds) {
    QHash<TrackId, Value> energies;
    if (trackIds.isEmpty() || !ensureTable(db)) {
        return energies;
    }
    const QSet<TrackId> wanted(trackIds.begin(), trackIds.end());
    // Small requests (e.g. one selected track) ask for just those ids;
    // large ones read the whole table, which is quicker than a huge IN list.
    constexpr int kMaxIdsInQuery = 500;
    const bool filter = wanted.size() <= kMaxIdsInQuery;
    QString idList;
    if (filter) {
        QStringList parts;
        parts.reserve(wanted.size());
        for (const TrackId& id : wanted) {
            parts << id.toString(); // integers from our own ids, safe to inline
        }
        idList = QStringLiteral(" WHERE track_id IN (") + parts.join(QChar(',')) + QChar(')');
    }
    const auto readInto = [&](const QString& sql, bool manual) {
        QSqlQuery query(db);
        if (!query.exec(sql + idList)) {
            LOG_FAILED_QUERY(query);
            return;
        }
        while (query.next()) {
            const TrackId id(query.value(0));
            if (wanted.contains(id)) {
                energies.insert(id, Value{query.value(1).toDouble(), manual});
            }
        }
    };
    readInto(QStringLiteral("SELECT track_id, energy FROM autodj_energy"), false);
    // Ratings last, so they replace measured values.
    readInto(QStringLiteral("SELECT track_id, rating FROM autodj_energy_manual"), true);
    return energies;
}
