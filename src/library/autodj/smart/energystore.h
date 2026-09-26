#pragma once

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <optional>

#include "analyzer/energycalculator.h"
#include "track/trackid.h"

/// Auto DJ 2.0: saves and loads energy scores.
///
/// Energy lives in its own table, `autodj_energy`, created on demand with
/// CREATE TABLE IF NOT EXISTS. It is not part of Mixxx's versioned schema,
/// so future official schema updates cannot collide with it, and official
/// Mixxx simply ignores the table.
class EnergyStore {
  public:
    /// Creates the table if needed. Safe to call often.
    static bool ensureTable(const QSqlDatabase& db);

    /// The formula version stored for this track, if any.
    static std::optional<int> storedVersion(const QSqlDatabase& db, TrackId trackId);

    static bool save(const QSqlDatabase& db,
            TrackId trackId,
            const EnergyCalculator::Result& result,
            int version);

    /// Energy (1..10) for the given tracks. Tracks without a score are left out.
    static QHash<TrackId, double> loadEnergies(
            const QSqlDatabase& db, const QList<TrackId>& trackIds);
};
