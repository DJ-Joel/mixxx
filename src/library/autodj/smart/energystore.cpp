#include "library/autodj/smart/energystore.h"

#include <QSet>
#include <QStringList>
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
    if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS autodj_energy_manual ("
                "track_id INTEGER PRIMARY KEY, "
                "rating INTEGER NOT NULL)"))) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
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
