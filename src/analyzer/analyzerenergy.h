#pragma once

#include <QSqlDatabase>
#include <memory>

#include "analyzer/analyzer.h"
#include "analyzer/energycalculator.h"
#include "track/trackid.h"

/// Auto DJ 2.0: computes the energy score (1..10) of each track and saves
/// it with EnergyStore. Skips tracks that already have a score from the
/// current formula version.
class AnalyzerEnergy : public Analyzer {
  public:
    explicit AnalyzerEnergy(const QSqlDatabase& dbConnection);
    ~AnalyzerEnergy() override = default;

    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* pIn, SINT count) override;
    void storeResults(TrackPointer pTrack) override;
    void cleanup() override;

  private:
    void setAutoMarkers(const TrackPointer& pTrack, const EnergyCalculator::Result& result);

    QSqlDatabase m_db;
    bool m_tableReady;
    TrackId m_trackId;
    std::unique_ptr<EnergyCalculator> m_pCalculator;
};
