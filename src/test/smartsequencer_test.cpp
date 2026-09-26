#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <limits>
#include <random>

#include "library/autodj/smart/mixscorer.h"
#include "library/autodj/smart/smartsequencer.h"
#include "library/autodj/smart/trackfeatures.h"

namespace {

TrackFeatures makeTrack(int id, int camelot, bool minor, double bpm, double energy = 0.0) {
    TrackFeatures f;
    f.energyIsManual = energy > 0.0; // tests rate energy by hand unless stated
    f.id = TrackId(QVariant(id));
    f.camelotNumber = camelot;
    f.camelotMinor = minor;
    f.bpm = bpm;
    f.energy = energy;
    return f;
}

double orderCost(const MixScorer& scorer,
        const QVector<TrackFeatures>& tracks,
        const QList<TrackId>& order) {
    auto find = [&tracks](const TrackId& id) -> const TrackFeatures& {
        for (const auto& t : tracks) {
            if (t.id == id) {
                return t;
            }
        }
        return tracks.front();
    };
    double sum = scorer.startCost(find(order.front())) + scorer.endCost(find(order.back()));
    for (int k = 1; k < order.size(); ++k) {
        sum += scorer.score(find(order[k - 1]), find(order[k])).total;
    }
    return sum;
}

// Exact answer by trying every order. Only for tiny inputs.
double bruteForceBest(const MixScorer& scorer, QVector<TrackFeatures> tracks) {
    std::sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) {
        return a.id.toString().toInt() < b.id.toString().toInt();
    });
    double best = std::numeric_limits<double>::max();
    do {
        double sum = scorer.startCost(tracks.front()) + scorer.endCost(tracks.back());
        for (int k = 1; k < tracks.size(); ++k) {
            sum += scorer.score(tracks[k - 1], tracks[k]).total;
        }
        best = std::min(best, sum);
    } while (std::next_permutation(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) {
        return a.id.toString().toInt() < b.id.toString().toInt();
    }));
    return best;
}

} // namespace

// ---------- TrackFeatures ----------

TEST(TrackFeaturesTest, OpenKeyToCamelot) {
    EXPECT_EQ(8, TrackFeatures::camelotFromOpenKey(1));  // C major / A minor
    EXPECT_EQ(9, TrackFeatures::camelotFromOpenKey(2));  // G major / E minor
    EXPECT_EQ(1, TrackFeatures::camelotFromOpenKey(6));  // B major / G# minor
    EXPECT_EQ(7, TrackFeatures::camelotFromOpenKey(12)); // F major / D minor
    EXPECT_EQ(0, TrackFeatures::camelotFromOpenKey(0));
    EXPECT_EQ(0, TrackFeatures::camelotFromOpenKey(13));
}

TEST(TrackFeaturesTest, CamelotText) {
    EXPECT_EQ(QStringLiteral("8A"), makeTrack(1, 8, true, 120).camelotText());
    EXPECT_EQ(QStringLiteral("12B"), makeTrack(1, 12, false, 120).camelotText());
    EXPECT_EQ(QStringLiteral("?"), makeTrack(1, 0, false, 120).camelotText());
}

// ---------- MixScorer: keys ----------

TEST(MixScorerTest, CamelotDistanceWrapsRoundTheWheel) {
    EXPECT_EQ(0, MixScorer::camelotDistance(8, 8));
    EXPECT_EQ(1, MixScorer::camelotDistance(12, 1));
    EXPECT_EQ(6, MixScorer::camelotDistance(1, 7));
    EXPECT_EQ(2, MixScorer::camelotStep(8, 10));
    EXPECT_EQ(-5, MixScorer::camelotStep(8, 3)); // +7
    EXPECT_EQ(-1, MixScorer::camelotStep(1, 12));
}

TEST(MixScorerTest, CamelotRulesAreOrderedAsDjsExpect) {
    const auto a8 = makeTrack(1, 8, true, 124);
    const double same = MixScorer::camelotCost(a8, makeTrack(2, 8, true, 124), 0, true);
    const double relative = MixScorer::camelotCost(a8, makeTrack(2, 8, false, 124), 0, true);
    const double adjacentUp = MixScorer::camelotCost(a8, makeTrack(2, 9, true, 124), 0, true);
    const double adjacentDown = MixScorer::camelotCost(a8, makeTrack(2, 7, true, 124), 0, true);
    const double diagonal = MixScorer::camelotCost(a8, makeTrack(2, 9, false, 124), 0, true);
    const double twoSteps = MixScorer::camelotCost(a8, makeTrack(2, 10, true, 124), 0, true);
    const double tritone = MixScorer::camelotCost(a8, makeTrack(2, 2, true, 124), 0, true);

    EXPECT_DOUBLE_EQ(0.0, same);
    EXPECT_LT(same, relative);
    EXPECT_LT(relative, adjacentUp);
    EXPECT_DOUBLE_EQ(adjacentUp, adjacentDown);
    EXPECT_LT(adjacentUp, diagonal);
    EXPECT_LT(diagonal, twoSteps);
    EXPECT_LT(twoSteps, tritone);
    EXPECT_GE(twoSteps, MixScorer::kClashKeyCost);
}

TEST(MixScorerTest, EnergyBoostOnlyExcusedWhenEnergyRises) {
    const auto a8 = makeTrack(1, 8, true, 124);
    const auto a10 = makeTrack(2, 10, true, 124); // +2 steps
    const auto a3 = makeTrack(3, 3, true, 124);   // +7 steps
    EXPECT_DOUBLE_EQ(2.0, MixScorer::camelotCost(a8, a10, 1.5, true));
    EXPECT_DOUBLE_EQ(2.0, MixScorer::camelotCost(a8, a3, 1.0, true));
    EXPECT_GE(MixScorer::camelotCost(a8, a10, 0.0, true), MixScorer::kClashKeyCost);
    EXPECT_GE(MixScorer::camelotCost(a8, a10, 2.0, false), MixScorer::kClashKeyCost);
    // Going DOWN two steps is not a boost move.
    EXPECT_GE(MixScorer::camelotCost(a10, a8, 2.0, true), MixScorer::kClashKeyCost);
}

TEST(MixScorerTest, UnknownKeyIsMildPenalty) {
    const double c = MixScorer::camelotCost(makeTrack(1, 0, true, 124),
            makeTrack(2, 8, true, 124),
            0,
            true);
    EXPECT_GT(c, 1.0);
    EXPECT_LT(c, MixScorer::kClashKeyCost);
}

// ---------- MixScorer: tempo ----------

TEST(MixScorerTest, TempoCost) {
    EXPECT_DOUBLE_EQ(0.0, MixScorer::tempoCost(124, 124, 5, true));
    EXPECT_NEAR(1.0, MixScorer::tempoCost(100, 105, 5, true), 0.03);
    EXPECT_LT(MixScorer::tempoCost(124, 126, 5, true), 1.0);
    EXPECT_NEAR(0.0, MixScorer::tempoCost(87, 174, 5, true), 1e-9);  // double time
    EXPECT_GE(MixScorer::tempoCost(87, 174, 5, false), MixScorer::kClashTempoCost);
    EXPECT_GE(MixScorer::tempoCost(100, 120, 5, true), MixScorer::kClashTempoCost);
    EXPECT_DOUBLE_EQ(2.0, MixScorer::tempoCost(0, 124, 5, true)); // unknown
}

// ---------- MixScorer: energy ----------

TEST(MixScorerTest, TempoCostIsSymmetricAndSmooth) {
    // Speeding up costs the same as slowing down by the same ratio.
    EXPECT_NEAR(MixScorer::tempoCost(118, 128, 5, true),
            MixScorer::tempoCost(128, 118, 5, true),
            1e-9);
    EXPECT_NEAR(MixScorer::tempoCost(135, 149.4, 5, true),
            MixScorer::tempoCost(149.4, 135, 5, true),
            1e-9);
    // Just inside the tolerance mixes; just past it is a clash.
    EXPECT_LT(MixScorer::tempoCost(100, 100 * std::exp(0.049), 5, true), 1.0);
    EXPECT_GE(MixScorer::tempoCost(100, 100 * std::exp(0.051), 5, true),
            MixScorer::kClashTempoCost);
    // The Promise (118.1) -> Rock The Casbah (129.5): about 9%, a clash.
    EXPECT_GE(MixScorer::tempoCost(118.1, 129.5, 5, true), MixScorer::kClashTempoCost);
}

TEST(MixScorerTest, EnergyBuildPenalisesDrops) {
    MixScoreWeights w;
    w.direction = MixScoreWeights::EnergyDirection::Build;
    MixScorer scorer(w);
    EXPECT_DOUBLE_EQ(0.0, scorer.energyCost(5, 6));
    EXPECT_GT(scorer.energyCost(6, 5), 0.0);
    EXPECT_GT(scorer.energyCost(3, 8), 0.0); // too big a jump
}

TEST(MixScorerTest, EnergyWaveAllowsSmallDips) {
    MixScoreWeights w;
    w.direction = MixScoreWeights::EnergyDirection::Wave;
    MixScorer scorer(w);
    EXPECT_DOUBLE_EQ(0.0, scorer.energyCost(6, 5));
    EXPECT_GT(scorer.energyCost(8, 4), 0.0);
}

TEST(MixScorerTest, UnknownEnergyShowsQuestionMark) {
    MixScorer scorer;
    const MixScore s = scorer.score(makeTrack(1, 8, true, 124), makeTrack(2, 9, true, 125));
    EXPECT_TRUE(s.reason.contains(QStringLiteral("energy ?"))) << s.reason.toStdString();
}

TEST(MixScorerTest, ClashLabel) {
    MixScorer scorer;
    EXPECT_TRUE(MixScorer::clashLabel(
            scorer.score(makeTrack(1, 8, true, 124), makeTrack(2, 9, true, 124)))
                        .isEmpty());
    EXPECT_EQ(QStringLiteral("key clash"),
            MixScorer::clashLabel(
                    scorer.score(makeTrack(1, 8, true, 124), makeTrack(2, 2, true, 124))));
    EXPECT_EQ(QStringLiteral("tempo clash"),
            MixScorer::clashLabel(
                    scorer.score(makeTrack(1, 8, true, 100), makeTrack(2, 8, true, 125))));
    EXPECT_EQ(QStringLiteral("key + tempo clash"),
            MixScorer::clashLabel(
                    scorer.score(makeTrack(1, 8, true, 100), makeTrack(2, 2, true, 125))));
}

TEST(MixScorerTest, MeasuredEnergyCountsLessThanRated) {
    MixScorer scorer;
    TrackFeatures a = makeTrack(1, 8, true, 124, 8);
    TrackFeatures b = makeTrack(2, 8, true, 124, 4); // big energy drop
    const double rated = scorer.score(a, b).energyCost;
    a.energyIsManual = false;
    const double measured = scorer.score(a, b).energyCost;
    EXPECT_GT(rated, 0.0);
    EXPECT_NEAR(measured, rated * scorer.weights().measuredEnergyTrust, 1e-9);
    EXPECT_TRUE(scorer.score(a, b).reason.contains(QStringLiteral("(measured)")));
}

TEST(MixScorerTest, OnlyRatedEnergyExcusesKeyJump) {
    MixScorer scorer;
    TrackFeatures a = makeTrack(1, 8, true, 124, 5);
    TrackFeatures b = makeTrack(2, 10, true, 124, 7); // +2 steps, energy +2
    EXPECT_LT(scorer.score(a, b).keyCost, MixScorer::kClashKeyCost);
    b.energyIsManual = false;
    EXPECT_GE(scorer.score(a, b).keyCost, MixScorer::kClashKeyCost);
}

TEST(MixScorerTest, ScoreHasReadableReason) {
    MixScorer scorer;
    const MixScore s = scorer.score(makeTrack(1, 8, true, 124, 5), makeTrack(2, 9, true, 125, 6));
    EXPECT_TRUE(s.reason.contains(QStringLiteral("8A -> 9A")));
    EXPECT_TRUE(s.reason.contains(QStringLiteral("energy +1.0 (rated)"))) << s.reason.toStdString();
    EXPECT_GT(s.total, 0.0);
}

// ---------- SmartSequencer ----------

TEST(SmartSequencerTest, EmptyAndSingle) {
    SmartSequencer seq{MixScorer()};
    EXPECT_TRUE(seq.solve({}).order.isEmpty());
    const auto r = seq.solve({makeTrack(1, 8, true, 124)});
    ASSERT_EQ(1, r.order.size());
    EXPECT_EQ(0, r.clashCount);
}

TEST(SmartSequencerTest, SortsAHarmonicChainWithoutClashes) {
    // 8A..12A shuffled. The only clash-free orders walk the wheel in steps.
    QVector<TrackFeatures> tracks = {
            makeTrack(1, 11, true, 124),
            makeTrack(2, 8, true, 124),
            makeTrack(3, 12, true, 124),
            makeTrack(4, 10, true, 124),
            makeTrack(5, 9, true, 124),
    };
    SmartSequencer seq{MixScorer()};
    const auto r = seq.solve(tracks);
    ASSERT_EQ(5, r.order.size());
    EXPECT_EQ(0, r.clashCount) << r.warnings.join('\n').toStdString();
}

TEST(SmartSequencerTest, KeepsFixedFirstTrack) {
    QVector<TrackFeatures> tracks = {
            makeTrack(1, 8, true, 124),
            makeTrack(2, 3, true, 124),
            makeTrack(3, 9, true, 124),
            makeTrack(4, 4, true, 124),
    };
    SmartSequencer seq{MixScorer()};
    const auto r = seq.solve(tracks, TrackId(QVariant(2)));
    ASSERT_EQ(4, r.order.size());
    EXPECT_EQ(TrackId(QVariant(2)), r.order.front());
}

TEST(SmartSequencerTest, ReportsClashesItCannotAvoid) {
    // 8A and 2A are opposite on the wheel: no smooth path exists.
    QVector<TrackFeatures> tracks = {
            makeTrack(1, 8, true, 124),
            makeTrack(2, 2, true, 124),
    };
    SmartSequencer seq{MixScorer()};
    const auto r = seq.solve(tracks);
    EXPECT_EQ(1, r.clashCount);
    ASSERT_EQ(1, r.warnings.size());
    EXPECT_TRUE(r.warnings[0].contains(QStringLiteral("key clash")));
}

TEST(SmartSequencerTest, BuildStartsCalmAndEndsAtPeak) {
    // Same key and tempo, so only energy decides the order.
    QVector<TrackFeatures> tracks = {
            makeTrack(1, 8, true, 124, 9),
            makeTrack(2, 8, true, 124, 3),
            makeTrack(3, 8, true, 124, 6),
            makeTrack(4, 8, true, 124, 7),
    };
    SmartSequencer seq{MixScorer()};
    const auto r = seq.solve(tracks);
    ASSERT_EQ(4, r.order.size());
    EXPECT_EQ(TrackId(QVariant(2)), r.order.front()); // energy 3 first
    EXPECT_EQ(TrackId(QVariant(1)), r.order.back());  // energy 9 last
}

TEST(SmartSequencerTest, HeuristicAlsoBuildsEnergy) {
    // Above kExactLimit: 20 tracks, same key/tempo, energies 1..10 twice.
    QVector<TrackFeatures> tracks;
    std::mt19937 rng(11);
    for (int i = 1; i <= 20; ++i) {
        tracks.push_back(makeTrack(i, 8, true, 124, 1 + (i % 10)));
    }
    std::shuffle(tracks.begin(), tracks.end(), rng);
    SmartSequencer seq{MixScorer()};
    const auto r = seq.solve(tracks);
    ASSERT_EQ(20, r.order.size());
    auto energyOf = [&](const TrackId& id) {
        for (const auto& t : tracks) {
            if (t.id == id) {
                return t.energy;
            }
        }
        return 0.0;
    };
    for (int k = 1; k < r.order.size(); ++k) {
        EXPECT_LE(energyOf(r.order[k - 1]), energyOf(r.order[k])) << "at " << k;
    }
}

TEST(SmartSequencerTest, OrderLinesListEveryTrackAndClash) {
    QVector<TrackFeatures> tracks = {
            makeTrack(1, 8, true, 124),
            makeTrack(2, 2, true, 124), // clash with 8A either way
    };
    tracks[0].displayName = QStringLiteral("Artist - One");
    SmartSequencer seq{MixScorer()};
    const auto r = seq.solve(tracks);
    ASSERT_EQ(3, r.orderLines.size()); // track, clash note, track
    EXPECT_TRUE(r.orderLines[1].contains(QStringLiteral("key clash")));
    EXPECT_TRUE(r.orderLines.join('\n').contains(QStringLiteral("Artist - One")));
}

TEST(SmartSequencerTest, ExactSolverMatchesBruteForce) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> key(1, 12);
    std::uniform_int_distribution<int> letter(0, 1);
    std::uniform_real_distribution<double> bpm(118.0, 132.0);
    std::uniform_real_distribution<double> energy(1.0, 10.0);
    MixScorer scorer;
    SmartSequencer seq(scorer);
    for (int round = 0; round < 20; ++round) {
        QVector<TrackFeatures> tracks;
        for (int i = 1; i <= 7; ++i) {
            tracks.push_back(makeTrack(i, key(rng), letter(rng) == 1, bpm(rng), energy(rng)));
        }
        const double best = bruteForceBest(scorer, tracks);
        const auto r = seq.solve(tracks);
        ASSERT_EQ(7, r.order.size());
        EXPECT_NEAR(best, orderCost(scorer, tracks, r.order), 1e-6);
        EXPECT_NEAR(best, r.totalCost, 1e-6);
    }
}

TEST(SmartSequencerTest, LargeSetFindsClashFreeWheelWalk) {
    // All 24 keys, shuffled. A clash-free order exists
    // (1A..12A, 12B..1B), and is above kExactLimit, so this tests the
    // heuristic path.
    QVector<TrackFeatures> tracks;
    int id = 1;
    for (int k = 1; k <= 12; ++k) {
        tracks.push_back(makeTrack(id++, k, true, 124));
        tracks.push_back(makeTrack(id++, k, false, 124));
    }
    std::mt19937 rng(7);
    std::shuffle(tracks.begin(), tracks.end(), rng);
    ASSERT_GT(tracks.size(), SmartSequencer::kExactLimit);

    SmartSequencer seq{MixScorer()};
    QElapsedTimer timer;
    timer.start();
    const auto r = seq.solve(tracks, std::nullopt, 2000);
    EXPECT_LT(timer.elapsed(), 3000);
    ASSERT_EQ(24, r.order.size());
    QSet<int> seen;
    for (const auto& t : r.order) {
        seen.insert(t.toString().toInt());
    }
    EXPECT_EQ(24, seen.size()); // every track exactly once
    EXPECT_EQ(0, r.clashCount) << r.warnings.join('\n').toStdString();
}
