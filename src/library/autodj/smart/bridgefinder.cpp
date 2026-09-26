#include "library/autodj/smart/bridgefinder.h"

#include <algorithm>

BridgeFinder::BridgeFinder(const MixScorer& scorer)
        : m_scorer(scorer) {
}

QList<TrackFeatures> BridgeFinder::extend(const TrackFeatures& last,
        const QVector<TrackFeatures>& candidates,
        QSet<TrackId> excludeIds,
        QSet<QString> excludeNames,
        int count) const {
    QList<TrackFeatures> chain;
    TrackFeatures current = last;
    excludeIds.insert(last.id);
    if (!last.displayName.isEmpty()) {
        excludeNames.insert(nameKey(last));
    }
    while (static_cast<int>(chain.size()) < count) {
        const TrackFeatures* pBest = nullptr;
        double bestCost = 0.0;
        for (const TrackFeatures& x : candidates) {
            if (!x.hasKey() || !x.hasBpm() || excludeIds.contains(x.id) ||
                    (!x.displayName.isEmpty() && excludeNames.contains(nameKey(x)))) {
                continue;
            }
            const MixScore s = m_scorer.score(current, x);
            if (!MixScorer::clashLabel(s).isEmpty()) {
                continue;
            }
            if (!pBest || s.total < bestCost) {
                pBest = &x;
                bestCost = s.total;
            }
        }
        if (!pBest) {
            break;
        }
        chain.append(*pBest);
        excludeIds.insert(pBest->id);
        if (!pBest->displayName.isEmpty()) {
            excludeNames.insert(nameKey(*pBest));
        }
        current = *pBest;
    }
    return chain;
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
