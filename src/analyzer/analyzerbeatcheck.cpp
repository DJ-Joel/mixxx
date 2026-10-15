#include "analyzer/analyzerbeatcheck.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QtDebug>
#include <cmath>

#include "analyzer/beatthisengine.h"
#include "library/autodj/smart/beatcheck.h"
#include "library/autodj/smart/energystore.h"
#include "stems/stemengine.h"
#include "track/track.h"

namespace {
const QString kAutoDjGroup = QStringLiteral("[Auto DJ]");
const QString kStemsGroup = QStringLiteral("[Stems]");
constexpr double kMaxSongSec = 20.0 * 60.0; // DJ mixes are left out

QString stemsEngineFolder(const UserSettingsPointer& pConfig) {
    const QString folder = pConfig->getValue(ConfigKey(kStemsGroup, "EngineFolder"), QString());
    return folder.isEmpty() ? stems::Engine::defaultFolder() : folder;
}
} // namespace

AnalyzerBeatCheck::AnalyzerBeatCheck(UserSettingsPointer pConfig, const QSqlDatabase& dbConnection)
        : m_pConfig(std::move(pConfig)),
          m_db(dbConnection) {
}

// static
bool AnalyzerBeatCheck::isEnabled(const UserSettingsPointer& pConfig) {
    return pConfig && pConfig->getValue(ConfigKey(kAutoDjGroup, "BeatCheck"), true);
}

// static
QString AnalyzerBeatCheck::runtimeFile(const UserSettingsPointer& pConfig) {
#if defined(_WIN32)
    const QString name = QStringLiteral("onnxruntime.dll");
#elif defined(__APPLE__)
    const QString name = QStringLiteral("libonnxruntime.dylib");
#else
    const QString name = QStringLiteral("libonnxruntime.so");
#endif
    return QDir(stemsEngineFolder(pConfig)).filePath(name);
}

// static
QString AnalyzerBeatCheck::melModel(const UserSettingsPointer& pConfig) {
    return QDir(pConfig->getResourcePath()).filePath(QStringLiteral("beatcheck/mel_spectrogram.onnx"));
}

// static
QString AnalyzerBeatCheck::beatModel(const UserSettingsPointer& pConfig, QString* pName) {
    const QString full = QDir(stemsEngineFolder(pConfig)).filePath(QStringLiteral("model/beat_this.onnx"));
    if (QFileInfo::exists(full)) {
        if (pName) {
            *pName = QStringLiteral("full");
        }
        return full;
    }
    if (pName) {
        *pName = QStringLiteral("small");
    }
    return QDir(pConfig->getResourcePath()).filePath(QStringLiteral("beatcheck/beat_this_small.onnx"));
}

void AnalyzerBeatCheck::needs(
        const TrackPointer& pTrack, const QString& model, bool* pBeats, bool* pLevels) const {
    const auto stored = EnergyStore::loadBeatCheck(m_db, pTrack->getId());
    *pBeats = !stored || stored->version != EnergyStore::BeatCheck::kVersion ||
            stored->model != model ||
            std::fabs(stored->durationSec - pTrack->getDuration()) > 1.0;
    *pLevels = *pBeats || !stored->hasLevels();
}

bool AnalyzerBeatCheck::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        mixxx::audio::ChannelCount channelCount,
        SINT frameLength) {
    const TrackPointer pTrack = track.getTrack();
    // The normal pass only (stereo), not the second pass over the stem file.
    if (!isEnabled(m_pConfig) || !pTrack || !pTrack->getId().isValid() ||
            channelCount != mixxx::audio::ChannelCount::stereo() || !sampleRate.isValid() ||
            frameLength <= 0) {
        return false;
    }
    const double seconds = static_cast<double>(frameLength) / sampleRate.value();
    if (seconds > kMaxSongSec || seconds < 30.0) {
        return false;
    }
    QString model;
    const QString beat = beatModel(m_pConfig, &model);
    bool needBeats = false;
    bool needLevels = false;
    needs(pTrack, model, &needBeats, &needLevels);
    if (!needLevels) {
        return false;
    }
    QString why;
    if (needBeats &&
            !beatcheck::Engine::shared(runtimeFile(m_pConfig), melModel(m_pConfig), beat, &why)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            qInfo().noquote() << "Beat check: off -" << why;
        }
        return false;
    }
    m_trackId = pTrack->getId();
    m_sampleRate = sampleRate.value();
    m_durationSec = pTrack->getDuration();
    m_model = model;
    m_needBeats = needBeats;
    m_mono.clear();
    if (m_needBeats) {
        m_mono.reserve(static_cast<std::size_t>(frameLength));
    }
    m_pLevels = std::make_unique<EnergyCalculator>(m_sampleRate, 2);
    return true;
}

bool AnalyzerBeatCheck::processSamples(const CSAMPLE* pIn, SINT count) {
    if (m_pLevels) {
        m_pLevels->process(pIn, count);
    }
    if (m_needBeats) {
        for (SINT i = 0; i + 1 < count; i += 2) {
            m_mono.push_back(0.5f * (pIn[i] + pIn[i + 1]));
        }
    }
    return true;
}

void AnalyzerBeatCheck::storeResults(TrackPointer pTrack) {
    // The levels for the beat grid check.
    EnergyCalculator::GridLevels levels;
    double bodyStartSec = 0.0;
    double bodyEndSec = 0.0;
    if (m_pLevels) {
        EnergyCalculator::Result result;
        if (m_pLevels->finish(&result)) {
            levels = m_pLevels->gridLevels();
            bodyStartSec = result.bodyStartSec;
            bodyEndSec = result.bodyEndSec;
        }
        m_pLevels.reset();
    }
    if (!m_needBeats) {
        auto stored = EnergyStore::loadBeatCheck(m_db, m_trackId);
        if (stored && levels.blockSec > 0.0) {
            stored->levels = std::move(levels);
            stored->bodyStartSec = bodyStartSec;
            stored->bodyEndSec = bodyEndSec;
            EnergyStore::saveBeatCheck(m_db, m_trackId, *stored);
            qDebug().noquote() << "Beat check: levels added for" << pTrack->getInfo();
        }
        return;
    }
    QString why;
    const auto pEngine = beatcheck::Engine::shared(
            runtimeFile(m_pConfig), melModel(m_pConfig), beatModel(m_pConfig), &why);
    if (!pEngine || m_mono.empty()) {
        return;
    }
    QElapsedTimer timer;
    timer.start();
    const std::vector<float> audio =
            beatcheck::resample(m_mono, m_sampleRate, beatcheck::kSampleRate);
    m_mono.clear();
    m_mono.shrink_to_fit();
    beatcheck::Found found;
    if (!pEngine->analyse(audio, &found, &why)) {
        qWarning().noquote() << "Beat check: failed for" << pTrack->getInfo() << "-" << why;
        return;
    }
    EnergyStore::BeatCheck check;
    check.version = EnergyStore::BeatCheck::kVersion;
    check.model = m_model;
    check.durationSec = m_durationSec;
    check.beats = found.beats;
    check.downbeats = found.downbeats;
    check.levels = std::move(levels);
    check.bodyStartSec = bodyStartSec;
    check.bodyEndSec = bodyEndSec;
    // Keep what the DJ ignored before (it still applies while nothing changes).
    if (const auto before = EnergyStore::loadBeatCheck(m_db, m_trackId)) {
        check.ignored = before->ignored;
    }
    EnergyStore::saveBeatCheck(m_db, m_trackId, check);
    qInfo().noquote() << "Beat check:" << pTrack->getInfo() << "-" << found.beats.size()
                      << "beats," << found.downbeats.size() << "downbeats (" << m_model
                      << "model," << timer.elapsed() << "ms)";
}

void AnalyzerBeatCheck::cleanup() {
    m_mono.clear();
    m_mono.shrink_to_fit();
    m_pLevels.reset();
    m_trackId = TrackId();
}
