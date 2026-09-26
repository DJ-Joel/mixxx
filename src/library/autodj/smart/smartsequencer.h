#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QStringList>
#include <QVector>
#include <optional>

#include "library/autodj/smart/mixscorer.h"

/// The outcome of SmartSequencer::solve().
struct SequenceResult {
    QList<TrackId> order;
    double totalCost = 0.0;
    /// Transitions the solver could not make smooth (key or tempo clash).
    int clashCount = 0;
    /// One human-readable line per clash, so the DJ knows where a bridge
    /// track is needed.
    QStringList warnings;
};

/// Sorts a set of tracks into the lowest-cost mixing order.
///
/// This is an open "travelling salesperson path" problem.
/// - Up to kExactLimit tracks: exact answer (Held-Karp dynamic programming).
/// - Larger sets: multi-start greedy, then 2-opt / Or-opt polishing and
///   random "kick and re-polish" rounds until the time budget runs out.
/// Pure logic: no Track objects, no database, safe on a worker thread.
class SmartSequencer {
  public:
    /// Largest set solved exactly. 2^15 * 15 * 15 = ~7M steps, ~6 MB memory.
    static constexpr int kExactLimit = 15;

    explicit SmartSequencer(const MixScorer& scorer);

    /// @param startId optional track that must stay first (e.g. playing now)
    /// @param timeBudgetMs stop improving after this long, return best so far
    SequenceResult solve(const QVector<TrackFeatures>& tracks,
            std::optional<TrackId> startId = std::nullopt,
            int timeBudgetMs = 2000) const;

  private:
    using CostMatrix = QVector<QVector<double>>;

    CostMatrix buildMatrix(const QVector<TrackFeatures>& tracks) const;
    static QVector<int> exactPath(const CostMatrix& c, int fixedStart);
    static QVector<int> heuristicPath(const CostMatrix& c, int fixedStart, int timeBudgetMs);
    static void localSearch(const CostMatrix& c,
            QVector<int>* pPath,
            bool lockFirst,
            const QElapsedTimer& timer,
            int timeBudgetMs);
    static QVector<int> greedyPath(const CostMatrix& c, int start);
    static bool improveTwoOpt(const CostMatrix& c, QVector<int>* pPath, bool lockFirst);
    static bool improveOrOpt(const CostMatrix& c, QVector<int>* pPath, bool lockFirst);
    static double pathCost(const CostMatrix& c, const QVector<int>& path);

    MixScorer m_scorer;
};
