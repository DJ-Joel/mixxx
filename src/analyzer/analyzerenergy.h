#pragma once

#include <QSqlDatabase>
#include <memory>

#include "analyzer/analyzer.h"
#include "analyzer/energycalculator.h"
#include "library/autodj/smart/downbeat.h"
#include "library/autodj/smart/phrasealign.h"
#include "track/trackid.h"

/// Auto DJ 2.0 plus Video Mixing: computes the energy score (1..10) of each track and saves
/// it with EnergyStore. Skips tracks that already have a score from the
/// current formula version and a beat grid check for the grid it has now.
class AnalyzerEnergy : public Analyzer {
  public:
    /// The track's beat grid as tempo + first beat (seconds). False (and
    /// 0, 0) if it has none.
    static bool gridOf(const TrackPointer& pTrack, double* pBpm, double* pFirstBeatSec);
    /// The track's beats (seconds at its own speed): a steady grid, or the
    /// time of every beat when Mixxx made a beat map that bends with the
    /// music ("Assume constant tempo" off). Not valid if it has none.
    static phrasealign::Grid beatGrid(const TrackPointer& pTrack);
    /// Beat 1 of the bar: the grid beat (index % 4, in beatGrid's count)
    /// where the main beat kicks in = the Intro End marker (the DJ's, or the
    /// one the analysis set at the first kick). *pKnown false (and 0) when
    /// the song has no Intro End marker: then the grid's first line counts
    /// as beat 1, as before. (The chord/bass finder in downbeat.h was right
    /// on 2 of 3 songs checked by ear and disagreed with the marker on half
    /// the library, so it is only logged.)
    static int beatOnePhase(const TrackPointer& pTrack, bool* pKnown);

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
    void storeGridCheck(const TrackPointer& pTrack, const EnergyCalculator::Result* pResult);
    void storeDownbeat(const TrackPointer& pTrack);

    QSqlDatabase m_db;
    bool m_tableReady;
    TrackId m_trackId;
    std::unique_ptr<EnergyCalculator> m_pCalculator;
    std::unique_ptr<downbeat::Features> m_pDownbeat; ///< where beat 1 of the bar is
    int m_channels = 2;
    std::vector<float> m_mono;
};
