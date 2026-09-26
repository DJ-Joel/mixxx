#include "library/autodj/smart/energystore.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "library/queryutil.h"

// static
bool EnergyStore::ensureTable(const QSqlDatabase& db) {
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
            "(track_id, energy, loudness_db, bright_ratio, onsets_per_sec, bass_ratio, version) "
            "VALUES (:id, :energy, :loudness, :bright, :onsets, :bass, :version)"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    query.bindValue(QStringLiteral(":energy"), result.energy);
    query.bindValue(QStringLiteral(":loudness"), result.loudnessDb);
    query.bindValue(QStringLiteral(":bright"), result.brightRatio);
    query.bindValue(QStringLiteral(":onsets"), result.onsetsPerSec);
    query.bindValue(QStringLiteral(":bass"), result.bassRatio);
    query.bindValue(QStringLiteral(":version"), version);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// static
QHash<TrackId, double> EnergyStore::loadEnergies(
        const QSqlDatabase& db, const QList<TrackId>& trackIds) {
    QHash<TrackId, double> energies;
    if (trackIds.isEmpty() || !ensureTable(db)) {
        return energies;
    }
    const QSet<TrackId> wanted(trackIds.begin(), trackIds.end());
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT track_id, energy FROM autodj_energy"))) {
        LOG_FAILED_QUERY(query);
        return energies;
    }
    while (query.next()) {
        const TrackId id(query.value(0));
        if (wanted.contains(id)) {
            energies.insert(id, query.value(1).toDouble());
        }
    }
    return energies;
}
