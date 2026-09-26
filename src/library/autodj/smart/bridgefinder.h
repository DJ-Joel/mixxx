#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

#include "library/autodj/smart/mixscorer.h"

/// A library track that could be played between two tracks that clash.
struct BridgeSuggestion {
    TrackFeatures track;
    MixScore in;  ///< from -> bridge
    MixScore out; ///< bridge -> to
    double cost = 0.0;
};

/// Auto DJ 2.0: finds "bridge" tracks. For a clashing pair A -> B it looks
/// for library tracks X where both A -> X and X -> B mix smoothly (no key or
/// tempo clash), best first. Pure logic, safe on a worker thread.
class BridgeFinder {
  public:
    explicit BridgeFinder(const MixScorer& scorer);

    /// @param excludeIds tracks that must not be suggested (already queued)
    /// @param excludeNames lower-case "artist - title" to skip, so another
    ///        copy of a queued song is not suggested either
    QList<BridgeSuggestion> find(const TrackFeatures& from,
            const TrackFeatures& to,
            const QVector<TrackFeatures>& candidates,
            const QSet<TrackId>& excludeIds,
            const QSet<QString>& excludeNames,
            int maxResults = 3) const;

    static QString nameKey(const TrackFeatures& track) {
        return track.displayName.trimmed().toLower();
    }

  private:
    MixScorer m_scorer;
};
