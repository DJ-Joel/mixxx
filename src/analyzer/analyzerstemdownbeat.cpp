#include "analyzer/analyzerstemdownbeat.h"

#include <QtDebug>

#include "analyzer/analyzerenergy.h"
#include "library/autodj/smart/energystore.h"
#include "track/track.h"

namespace {
// Stem audio: 4 stereo parts (drums, bass, other, vocals).
constexpr int kStemChannels = 8;
} // namespace

AnalyzerStemDownbeat::AnalyzerStemDownbeat(const QSqlDatabase& dbConnection)
        : m_db(dbConnection) {
}

bool AnalyzerStemDownbeat::isNeeded(const TrackPointer& pTrack) const {
    double bpm = 0.0;
    double firstBeatSec = 0.0;
    if (!pTrack || !pTrack->getId().isValid() ||
            !AnalyzerEnergy::gridOf(pTrack, &bpm, &firstBeatSec)) {
        return false; // no grid: nothing to count bars on
    }
    const auto stored = EnergyStore::loadDownbeat(m_db, pTrack->getId(), true);
    return !stored || !stored->isFor(bpm, firstBeatSec);
}

bool AnalyzerStemDownbeat::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        mixxx::audio::ChannelCount channelCount,
        SINT frameLength) {
    Q_UNUSED(frameLength);
    const TrackPointer pTrack = track.getTrack();
    if (channelCount != mixxx::audio::ChannelCount(kStemChannels) || !sampleRate.isValid() ||
            !isNeeded(pTrack)) {
        return false;
    }
    m_trackId = pTrack->getId();
    m_pFeatures = std::make_unique<downbeat::Features>(static_cast<double>(sampleRate.value()));
    return true;
}

bool AnalyzerStemDownbeat::processSamples(const CSAMPLE* pIn, SINT count) {
    VERIFY_OR_DEBUG_ASSERT(m_pFeatures) {
        return false;
    }
    const SINT frames = count / kStemChannels;
    m_mono.resize(static_cast<std::size_t>(frames));
    downbeat::bassAndChordsFromParts(pIn, static_cast<int>(frames), m_mono.data());
    m_pFeatures->process(m_mono.data(), static_cast<int>(frames));
    return true;
}

void AnalyzerStemDownbeat::storeResults(TrackPointer pTrack) {
    VERIFY_OR_DEBUG_ASSERT(m_pFeatures) {
        return;
    }
    EnergyStore::Downbeat stored;
    stored.version = EnergyStore::Downbeat::kVersion;
    if (!AnalyzerEnergy::gridOf(pTrack, &stored.bpm, &stored.firstBeatSec)) {
        return;
    }
    const downbeat::Result found =
            downbeat::find(*m_pFeatures, AnalyzerEnergy::beatTimesOf(pTrack));
    stored.phase = found.phase;
    stored.margin = found.margin;
    stored.sure = found.sure;
    EnergyStore::saveDownbeat(m_db, m_trackId, stored, true);

    // Next to the other two answers, so it can be judged by ear.
    bool marked = false;
    const int markerPhase = AnalyzerEnergy::beatOnePhase(pTrack, &marked);
    const auto whole = EnergyStore::loadDownbeat(m_db, m_trackId, false);
    const bool wholeValid = whole && whole->isFor(stored.bpm, stored.firstBeatSec);
    QString compare;
    if (marked) {
        compare = markerPhase == found.phase
                ? QStringLiteral("- the Intro End marker agrees (beat %1)").arg(markerPhase + 1)
                : QStringLiteral("- the Intro End marker says beat %1 (DIFFERENT)")
                          .arg(markerPhase + 1);
    } else {
        compare = found.sure
                ? QStringLiteral("- no Intro End marker: Auto DJ counts bars from this")
                : QStringLiteral("- no Intro End marker, not sure: the grid's first beat stays "
                                 "beat 1");
    }
    if (wholeValid) {
        compare += QStringLiteral("; on the whole song: beat %1 (%2)")
                           .arg(whole->phase + 1)
                           .arg(whole->sure ? QStringLiteral("sure")
                                            : QStringLiteral("not sure"));
    }
    qInfo().noquote() << "AnalyzerStemDownbeat: beat 1 from the parts" << pTrack->getInfo()
                      << "- the grid's beat" << found.phase + 1 << "(clear by"
                      << found.margin << ")"
                      << (found.sure ? QStringLiteral("sure") : QStringLiteral("not sure"))
                      << compare;
}

void AnalyzerStemDownbeat::cleanup() {
    m_pFeatures.reset();
    m_mono.clear();
    m_trackId = TrackId();
}
