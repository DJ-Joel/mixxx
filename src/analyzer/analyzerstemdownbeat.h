#pragma once

#include <QSqlDatabase>
#include <memory>
#include <vector>

#include "analyzer/analyzer.h"
#include "library/autodj/smart/downbeat.h"
#include "track/trackid.h"

/// Auto DJ 2.0 plus Video Mixing: finds beat 1 of the bar again, on the
/// song's stem parts. The finder (downbeat.h) listens for the bass note and
/// the chords changing; on the whole song the drums and the singing get in
/// the way, on the parts it hears only the bass and the chords ("other").
///
/// Runs in the analyzer's second pass over the song's stem file (see
/// AnalyzerThread::analyzeStemWaveform), once per song and beat grid. The
/// result is saved apart (EnergyStore, fromParts) and logged next to the
/// Intro End marker, so it can be checked by ear. Auto DJ uses it only for a
/// song that has no Intro End marker (see AutoDJProcessor::gridFor).
class AnalyzerStemDownbeat : public Analyzer {
  public:
    explicit AnalyzerStemDownbeat(const QSqlDatabase& dbConnection);
    ~AnalyzerStemDownbeat() override = default;

    /// True when this song has a beat grid but no beat 1 from its parts
    /// for that grid yet.
    bool isNeeded(const TrackPointer& pTrack) const;

    /// Only for stem audio (4 stereo parts: drums, bass, other, vocals),
    /// and only when isNeeded().
    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* pIn, SINT count) override;
    void storeResults(TrackPointer pTrack) override;
    void cleanup() override;

  private:
    QSqlDatabase m_db;
    TrackId m_trackId;
    std::unique_ptr<downbeat::Features> m_pFeatures;
    std::vector<float> m_mono;
};
