#pragma once

#include <QSqlDatabase>
#include <memory>
#include <vector>

#include "analyzer/analyzer.h"
#include "analyzer/energycalculator.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"

/// Auto DJ 2.0 plus Video Mixing: the beat check (beatcheck.h) during the
/// normal analysis of a song. It listens to the whole song once with the
/// "Beat This!" beat finder and keeps the beats and downbeats it hears
/// (EnergyStore::BeatCheck). Nothing about the song is changed: the review
/// on the Analyze page (Beat Check button) compares them with the song's
/// beat grid and markers, and the DJ picks what to Fix or Ignore.
///
/// It also keeps the bass and treble levels the beat grid check needs
/// (EnergyCalculator), so a grid the review proposes can be tried against
/// the music before it is offered. Songs checked before the levels were
/// kept only get the levels added (quick, the model is not run again).
///
/// Off when ONNX Runtime is not there (it comes with the stems engine), or
/// with [Auto DJ] BeatCheck = 0. Songs over 20 minutes (DJ mixes) are left
/// out, like for Stem Split.
class AnalyzerBeatCheck : public Analyzer {
  public:
    AnalyzerBeatCheck(UserSettingsPointer pConfig, const QSqlDatabase& dbConnection);
    ~AnalyzerBeatCheck() override = default;

    static bool isEnabled(const UserSettingsPointer& pConfig);
    /// onnxruntime.dll in the stems engine folder.
    static QString runtimeFile(const UserSettingsPointer& pConfig);
    static QString melModel(const UserSettingsPointer& pConfig);
    /// The best beat model there is, and its short name ("full", "small").
    static QString beatModel(const UserSettingsPointer& pConfig, QString* pName = nullptr);

    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* pIn, SINT count) override;
    void storeResults(TrackPointer pTrack) override;
    void cleanup() override;

  private:
    /// What this song still needs: the beats (the model), the levels.
    void needs(const TrackPointer& pTrack, const QString& model, bool* pBeats, bool* pLevels) const;

    UserSettingsPointer m_pConfig;
    QSqlDatabase m_db;
    TrackId m_trackId;
    double m_sampleRate = 0.0;
    double m_durationSec = 0.0;
    QString m_model;
    bool m_needBeats = false;
    std::vector<float> m_mono;
    std::unique_ptr<EnergyCalculator> m_pLevels;
};
