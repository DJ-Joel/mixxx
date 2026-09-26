#pragma once

#include <QString>

#include "library/autodj/smart/trackfeatures.h"

/// Tunable weights for MixScorer. All values are starting points to be
/// tuned by ear; they are not measured optimums.
struct MixScoreWeights {
    enum class EnergyDirection {
        Build, ///< energy should rise gently (short sets, warm-ups)
        Hold,  ///< energy should stay level
        Wave,  ///< rises and dips are fine (long sets)
    };

    double key = 1.0;
    double tempo = 1.0;
    double energy = 0.6;
    double bpmTolerancePct = 5.0;
    bool allowHalfDoubleTime = true;
    bool energyBoostExcusesKeyJump = true;
    EnergyDirection direction = EnergyDirection::Build;
};

/// The cost of playing one track after another. Lower is better.
/// The parts are kept so the GUI can explain *why* a track was chosen.
struct MixScore {
    double total = 0.0;
    double keyCost = 0.0;
    double tempoCost = 0.0;
    double energyCost = 0.0;
    QString reason;
};

class MixScorer {
  public:
    // Costs at or above these count as a "clash" in reports.
    static constexpr double kClashKeyCost = 3.0;
    static constexpr double kClashTempoCost = 5.0;

    explicit MixScorer(MixScoreWeights weights = MixScoreWeights());

    MixScore score(const TrackFeatures& from, const TrackFeatures& to) const;

    const MixScoreWeights& weights() const {
        return m_weights;
    }

    /// Harmonic cost on the Camelot wheel. 0 = same key.
    static double camelotCost(const TrackFeatures& from,
            const TrackFeatures& to,
            double energyDelta,
            bool boostExcusesKeyJump);

    /// Tempo cost. 0..1 inside tolerance, 1..5 up to twice the tolerance,
    /// 20+ beyond that.
    static double tempoCost(double bpmFrom,
            double bpmTo,
            double tolerancePct,
            bool allowHalfDoubleTime);

    /// Energy-flow cost, depends on the chosen EnergyDirection.
    double energyCost(double energyFrom, double energyTo) const;

    /// "key clash", "tempo clash", "key + tempo clash", or empty if smooth.
    static QString clashLabel(const MixScore& score);

    /// Shortest distance round the Camelot wheel: 0..6.
    static int camelotDistance(int from, int to);

    /// Signed clockwise step from `from` to `to`: -5..+6 (+7 shows as -5).
    static int camelotStep(int from, int to);

  private:
    MixScoreWeights m_weights;
};
