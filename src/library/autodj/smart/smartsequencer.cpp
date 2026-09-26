#include "library/autodj/smart/smartsequencer.h"

#include <QElapsedTimer>
#include <algorithm>
#include <limits>
#include <random>
#include <vector>

namespace {
constexpr double kEps = 1e-9;
// Stop kicking after this many rounds in a row without improvement.
constexpr int kMaxStaleKicks = 300;
} // namespace

SmartSequencer::SmartSequencer(const MixScorer& scorer)
        : m_scorer(scorer) {
}

// Cost of every ordered pair. Not symmetric: A->B can differ from B->A
// because the energy direction matters.
SmartSequencer::CostMatrix SmartSequencer::buildMatrix(
        const QVector<TrackFeatures>& tracks) const {
    const int n = static_cast<int>(tracks.size());
    CostMatrix c(n, QVector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i != j) {
                c[i][j] = m_scorer.score(tracks[i], tracks[j]).total;
            }
        }
    }
    return c;
}

// static
double SmartSequencer::pathCost(const CostMatrix& c, const QVector<int>& path) {
    double sum = 0.0;
    for (int k = 1; k < path.size(); ++k) {
        sum += c[path[k - 1]][path[k]];
    }
    return sum;
}

// static
// Always go to the cheapest unused track next.
QVector<int> SmartSequencer::greedyPath(const CostMatrix& c, int start) {
    const int n = static_cast<int>(c.size());
    QVector<bool> used(n, false);
    QVector<int> path;
    path.reserve(n);
    path.push_back(start);
    used[start] = true;
    while (path.size() < n) {
        const int last = path.back();
        int best = -1;
        double bestCost = std::numeric_limits<double>::max();
        for (int j = 0; j < n; ++j) {
            if (!used[j] && c[last][j] < bestCost) {
                bestCost = c[last][j];
                best = j;
            }
        }
        used[best] = true;
        path.push_back(best);
    }
    return path;
}

// static
// 2-opt: try reversing a stretch of the list. Costs are not symmetric, so
// the reversed stretch is priced with prefix sums; each test is O(1).
bool SmartSequencer::improveTwoOpt(const CostMatrix& c, QVector<int>* pPath, bool lockFirst) {
    QVector<int>& p = *pPath;
    const int n = static_cast<int>(p.size());
    if (n < 3) {
        return false;
    }
    QVector<double> fwd(n, 0.0); // edges walked forwards
    QVector<double> rev(n, 0.0); // the same edges walked backwards
    for (int k = 1; k < n; ++k) {
        fwd[k] = fwd[k - 1] + c[p[k - 1]][p[k]];
        rev[k] = rev[k - 1] + c[p[k]][p[k - 1]];
    }
    for (int i = lockFirst ? 1 : 0; i < n - 1; ++i) {
        for (int j = i + 1; j < n; ++j) {
            double before = fwd[j] - fwd[i];
            double after = rev[j] - rev[i];
            if (i > 0) {
                before += c[p[i - 1]][p[i]];
                after += c[p[i - 1]][p[j]];
            }
            if (j < n - 1) {
                before += c[p[j]][p[j + 1]];
                after += c[p[i]][p[j + 1]];
            }
            if (after + kEps < before) {
                std::reverse(p.begin() + i, p.begin() + j + 1);
                return true;
            }
        }
    }
    return false;
}

// static
// Or-opt: lift out 1-3 tracks in a row and drop them somewhere cheaper.
bool SmartSequencer::improveOrOpt(const CostMatrix& c, QVector<int>* pPath, bool lockFirst) {
    QVector<int>& p = *pPath;
    const int n = static_cast<int>(p.size());
    // Edge cost; -1 means "no track here" (start or end of the list).
    auto edge = [&c](int a, int b) {
        return (a < 0 || b < 0) ? 0.0 : c[a][b];
    };
    const int firstMovable = lockFirst ? 1 : 0;
    for (int len = 1; len <= 3; ++len) {
        for (int i = firstMovable; i + len <= n; ++i) {
            const int prev = i > 0 ? p[i - 1] : -1;
            const int next = i + len < n ? p[i + len] : -1;
            const int s0 = p[i];
            const int s1 = p[i + len - 1];
            const double saved = edge(prev, s0) + edge(s1, next) - edge(prev, next);
            // k = the gap just before p[k]; k == n means the very end.
            for (int k = firstMovable; k <= n; ++k) {
                if (k >= i && k <= i + len) {
                    continue; // same place
                }
                const int x = k > 0 ? p[k - 1] : -1;
                const int y = k < n ? p[k] : -1;
                const double added = edge(x, s0) + edge(s1, y) - edge(x, y);
                if (added + kEps < saved) {
                    QVector<int> out;
                    out.reserve(n);
                    for (int t = 0; t <= n; ++t) {
                        if (t == k) {
                            out += p.mid(i, len);
                        }
                        if (t < n && (t < i || t >= i + len)) {
                            out.push_back(p[t]);
                        }
                    }
                    p = out;
                    return true;
                }
            }
        }
    }
    return false;
}

// static
// Exact answer by dynamic programming (Held-Karp), for small sets.
// dp[mask][j] = cheapest way to play exactly the tracks in `mask`, ending on j.
QVector<int> SmartSequencer::exactPath(const CostMatrix& c, int fixedStart) {
    const int n = static_cast<int>(c.size());
    const int full = (1 << n) - 1;
    const double inf = std::numeric_limits<double>::max();
    const auto at = [n](int mask, int j) {
        return static_cast<std::size_t>(mask) * n + j;
    };
    std::vector<double> dp(static_cast<std::size_t>(full + 1) * n, inf);
    std::vector<int> parent(static_cast<std::size_t>(full + 1) * n, -1);
    for (int s = 0; s < n; ++s) {
        if (fixedStart < 0 || s == fixedStart) {
            dp[at(1 << s, s)] = 0.0;
        }
    }
    for (int mask = 1; mask <= full; ++mask) {
        for (int j = 0; j < n; ++j) {
            const double cur = dp[at(mask, j)];
            if (!(mask & (1 << j)) || cur == inf) {
                continue;
            }
            for (int k = 0; k < n; ++k) {
                if (mask & (1 << k)) {
                    continue;
                }
                const int next = mask | (1 << k);
                const double v = cur + c[j][k];
                if (v < dp[at(next, k)]) {
                    dp[at(next, k)] = v;
                    parent[at(next, k)] = j;
                }
            }
        }
    }
    int bestEnd = 0;
    for (int j = 1; j < n; ++j) {
        if (dp[at(full, j)] < dp[at(full, bestEnd)]) {
            bestEnd = j;
        }
    }
    QVector<int> path;
    path.reserve(n);
    int mask = full;
    for (int j = bestEnd; j != -1;) {
        path.push_back(j);
        const int prev = parent[at(mask, j)];
        mask &= ~(1 << j);
        j = prev;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// static
// Polish with 2-opt and Or-opt until nothing improves or time runs out.
void SmartSequencer::localSearch(const CostMatrix& c,
        QVector<int>* pPath,
        bool lockFirst,
        const QElapsedTimer& timer,
        int timeBudgetMs) {
    bool improved = true;
    while (improved && timer.elapsed() < timeBudgetMs) {
        improved = improveTwoOpt(c, pPath, lockFirst);
        improved = improveOrOpt(c, pPath, lockFirst) || improved;
    }
}

// static
// For large sets: greedy start, polish, then repeatedly "kick" the order
// (move a random block) and re-polish, keeping any improvement.
QVector<int> SmartSequencer::heuristicPath(const CostMatrix& c, int fixedStart, int timeBudgetMs) {
    QElapsedTimer timer;
    timer.start();
    const int n = static_cast<int>(c.size());
    const bool lockFirst = fixedStart >= 0;

    // Step 1: greedy from every possible first track; keep the best.
    QVector<int> best;
    double bestCost = std::numeric_limits<double>::max();
    for (int s = 0; s < n; ++s) {
        if (lockFirst && s != fixedStart) {
            continue;
        }
        const QVector<int> path = greedyPath(c, s);
        const double cost = pathCost(c, path);
        if (cost < bestCost) {
            bestCost = cost;
            best = path;
        }
        if (timer.elapsed() > timeBudgetMs / 4) {
            break; // leave time for polishing
        }
    }

    // Step 2: polish.
    localSearch(c, &best, lockFirst, timer, timeBudgetMs);
    bestCost = pathCost(c, best);

    // Step 3: kick and re-polish. Fixed seed: the same crate always gives
    // the same order, which makes results repeatable and testable.
    std::mt19937 rng(20260925u);
    const int firstMovable = lockFirst ? 1 : 0;
    const int movable = n - firstMovable;
    int staleKicks = 0;
    while (movable >= 4 && staleKicks < kMaxStaleKicks && timer.elapsed() < timeBudgetMs) {
        QVector<int> cand = best;
        const int maxLen = std::min(8, movable - 1);
        const int len = std::uniform_int_distribution<int>(1, maxLen)(rng);
        const int from = std::uniform_int_distribution<int>(firstMovable, n - len)(rng);
        QVector<int> block = cand.mid(from, len);
        cand.remove(from, len);
        if (rng() & 1u) {
            std::reverse(block.begin(), block.end());
        }
        const int to = std::uniform_int_distribution<int>(
                firstMovable, static_cast<int>(cand.size()))(rng);
        for (int b = 0; b < len; ++b) {
            cand.insert(to + b, block[b]);
        }
        localSearch(c, &cand, lockFirst, timer, timeBudgetMs);
        const double cost = pathCost(c, cand);
        if (cost + kEps < bestCost) {
            best = cand;
            bestCost = cost;
            staleKicks = 0;
        } else {
            ++staleKicks;
        }
    }
    return best;
}

SequenceResult SmartSequencer::solve(const QVector<TrackFeatures>& tracks,
        std::optional<TrackId> startId,
        int timeBudgetMs) const {
    SequenceResult result;
    const int n = static_cast<int>(tracks.size());
    if (n == 0) {
        return result;
    }
    const CostMatrix c = buildMatrix(tracks);

    int fixedStart = -1;
    if (startId) {
        for (int i = 0; i < n; ++i) {
            if (tracks[i].id == *startId) {
                fixedStart = i;
                break;
            }
        }
    }

    const QVector<int> best = n <= kExactLimit
            ? exactPath(c, fixedStart)
            : heuristicPath(c, fixedStart, timeBudgetMs);

    // Report, including the clashes that could not be avoided.
    result.totalCost = pathCost(c, best);
    for (int k = 0; k < n; ++k) {
        result.order.push_back(tracks[best[k]].id);
        if (k == 0) {
            continue;
        }
        const TrackFeatures& from = tracks[best[k - 1]];
        const TrackFeatures& to = tracks[best[k]];
        const MixScore s = m_scorer.score(from, to);
        const QString label = MixScorer::clashLabel(s);
        if (!label.isEmpty()) {
            ++result.clashCount;
            // k is 0-based, so the pair is tracks k and k + 1 counted from 1.
            QString warning = QStringLiteral("Tracks %1 and %2 (%3): %4")
                                      .arg(QString::number(k),
                                              QString::number(k + 1),
                                              label,
                                              s.reason);
            if (!from.displayName.isEmpty() || !to.displayName.isEmpty()) {
                warning += QStringLiteral("\n    %1\n    -> %2")
                                   .arg(from.displayName, to.displayName);
            }
            result.warnings << warning;
        }
    }
    return result;
}
