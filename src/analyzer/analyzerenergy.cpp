#include "analyzer/analyzerenergy.h"

#include <QtDebug>

#include "analyzer/analyzertrack.h"
#include "library/autodj/smart/energystore.h"
#include "track/track.h"

AnalyzerEnergy::AnalyzerEnergy(const QSqlDatabase& dbConnection)
        : m_db(dbConnection),
          m_tableReady(EnergyStore::ensureTable(dbConnection)) {
}

bool AnalyzerEnergy::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        mixxx::audio::ChannelCount channelCount,
        SINT frameLength) {
    if (!m_tableReady || frameLength <= 0) {
        return false;
    }
    const TrackPointer& pTrack = track.getTrack();
    m_trackId = pTrack->getId();
    if (!m_trackId.isValid()) {
        return false;
    }
    // Already analysed with the current formula? Then skip.
    const auto version = EnergyStore::storedVersion(m_db, m_trackId);
    if (version && *version == EnergyCalculator::kVersion) {
        return false;
    }
    m_pCalculator = std::make_unique<EnergyCalculator>(
            static_cast<double>(sampleRate.value()),
            static_cast<int>(channelCount.value()));
    return true;
}

bool AnalyzerEnergy::processSamples(const CSAMPLE* pIn, SINT count) {
    VERIFY_OR_DEBUG_ASSERT(m_pCalculator) {
        return false;
    }
    m_pCalculator->process(pIn, count);
    return true;
}

void AnalyzerEnergy::storeResults(TrackPointer pTrack) {
    VERIFY_OR_DEBUG_ASSERT(m_pCalculator) {
        return;
    }
    EnergyCalculator::Result result;
    if (!m_pCalculator->finish(&result)) {
        qDebug() << "AnalyzerEnergy: not enough audio for an energy score"
                 << pTrack->getInfo();
        return;
    }
    if (EnergyStore::save(m_db, m_trackId, result, EnergyCalculator::kVersion)) {
        qDebug() << "AnalyzerEnergy:" << pTrack->getInfo()
                 << "energy" << result.energy
                 << "loudness dB" << result.loudnessDb
                 << "bright" << result.brightRatio
                 << "onsets/s" << result.onsetsPerSec;
    }
}

void AnalyzerEnergy::cleanup() {
    m_pCalculator.reset();
}
