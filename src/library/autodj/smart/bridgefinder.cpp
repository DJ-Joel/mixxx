#include "library/autodj/smart/bridgefinder.h"

#include <algorithm>

BridgeFinder::BridgeFinder(const MixScorer& scorer)
        : m_scorer(scorer) {
}

QList<BridgeSuggestion> BridgeFinder::find(const TrackFeatures& from,
        const TrackFeatures& to,
        const QVector<TrackFeatures>& candidates,
        const QSet<TrackId>& excludeIds,
        const QSet<QString>& excludeNames,
        int maxResults) const {
    QList<BridgeSuggestion> found;
    for (const TrackFeatures& x : candidates) {
        // Only tracks we can judge: key and BPM must be known.
        if (!x.hasKey() || !x.hasBpm() || excludeIds.contains(x.id) ||
                x.id == from.id || x.id == to.id ||
                (!x.displayName.isEmpty() && excludeNames.contains(nameKey(x)))) {
            continue;
        }
        BridgeSuggestion s;
        s.in = m_scorer.score(from, x);
        s.out = m_scorer.score(x, to);
        // A bridge must make both mixes smooth, or it is no bridge.
        if (!MixScorer::clashLabel(s.in).isEmpty() ||
                !MixScorer::clashLabel(s.out).isEmpty()) {
            continue;
        }
        s.track = x;
        s.cost = s.in.total + s.out.total;
        found.append(s);
    }
    std::sort(found.begin(), found.end(), [](const BridgeSuggestion& a, const BridgeSuggestion& b) {
        return a.cost < b.cost;
    });
    if (found.size() > maxResults) {
        found = found.mid(0, maxResults);
    }
    return found;
}
