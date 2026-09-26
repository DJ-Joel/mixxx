#include "library/autodj/autodjprocessor.h"

#include <QFutureWatcher>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "engine/channels/enginedeck.h"
#include "control/controlobject.h"
#include "library/autodj/smart/beatmatch.h"
#include "library/autodj/smart/bridgefinder.h"
#include "library/autodj/smart/phrasealign.h"
#include "library/autodj/smart/energystore.h"
#include "library/autodj/smart/smartsequencer.h"
#include "library/autodj/smart/trackfeatures.h"
#include "library/columncache.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "moc_autodjprocessor.cpp"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/assert.h"
#include "util/logger.h"
#include "util/math.h"

namespace {
const mixxx::Logger kLogger("AutoDJ");
const QString kPreferenceGroup = QStringLiteral("[Auto DJ]");
const QString kControlGroup = QStringLiteral("[AutoDJ]");
const char* kTransitionPreferenceName = "Transition";
const char* kTransitionModePreferenceName = "TransitionMode";
constexpr double kTransitionPreferenceDefault = 10.0;
constexpr double kKeepPosition = -1.0;

// A track needs to be longer than two callbacks to not stop AutoDJ
constexpr double kMinimumTrackDurationSec = 0.2;

// Auto DJ 2.0 Phase 2 helpers. Controls may be missing (e.g. in tests or
// when the EQ rack is not loaded), so every access checks first.
const QString kBeatmatchPreference = QStringLiteral("SmartBeatmatch");
constexpr double kBeatmatchTolerancePct = 5.0; // the DJ's 5% rule
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();

ConfigKey eqKillKey(const QString& deckGroup) {
    // parameter1 = low band; button_parameter1 = its kill switch.
    return ConfigKey(QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup),
            QStringLiteral("button_parameter1"));
}

double readControl(const ConfigKey& key) {
    return ControlObject::exists(key) ? ControlObject::get(key) : kMissing;
}

void writeControl(const ConfigKey& key, double value) {
    if (!std::isnan(value) && ControlObject::exists(key)) {
        ControlObject::set(key, value);
    }
}

// Auto DJ 2.0: every library track with a known key and BPM whose file
// still exists, as bridge candidates. Read straight from the database so
// no Track objects are loaded for the whole library.
QVector<TrackFeatures> loadBridgeCandidates(const QSqlDatabase& db) {
    QVector<TrackFeatures> candidates;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
                "SELECT library.id, library.bpm, library.key_id, "
                "library.artist, library.title "
                "FROM library JOIN track_locations "
                "ON track_locations.id = library.location "
                "WHERE library.mixxx_deleted = 0 "
                "AND track_locations.fs_deleted = 0 "
                "AND library.bpm > 0 AND library.key_id > 0"))) {
        kLogger.warning() << "Could not read bridge candidates:" << query.lastError();
        return candidates;
    }
    QList<TrackId> ids;
    while (query.next()) {
        TrackFeatures f;
        f.id = TrackId(query.value(0));
        f.bpm = query.value(1).toDouble();
        const int keyValue = query.value(2).toInt();
        if (!mixxx::track::io::key::ChromaticKey_IsValid(keyValue)) {
            continue;
        }
        const auto key = static_cast<mixxx::track::io::key::ChromaticKey>(keyValue);
        if (key == mixxx::track::io::key::INVALID) {
            continue;
        }
        f.camelotNumber = TrackFeatures::camelotFromOpenKey(KeyUtils::keyToOpenKeyNumber(key));
        f.camelotMinor = !KeyUtils::keyIsMajor(key);
        const QString artist = query.value(3).toString().trimmed();
        const QString title = query.value(4).toString().trimmed();
        f.displayName = artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title;
        candidates.append(f);
        ids.append(f.id);
    }
    const QHash<TrackId, EnergyStore::Value> energies = EnergyStore::loadEnergies(db, ids);
    for (TrackFeatures& f : candidates) {
        const EnergyStore::Value value = energies.value(f.id);
        f.energy = value.energy;
        f.energyIsManual = value.manual;
    }
    return candidates;
}

const char* autoDJStateName(AutoDJProcessor::AutoDJState state) {
    switch (state) {
    case AutoDJProcessor::ADJ_IDLE:
        return "IDLE";
    case AutoDJProcessor::ADJ_LEFT_FADING:
        return "LEFT_FADING";
    case AutoDJProcessor::ADJ_RIGHT_FADING:
        return "RIGHT_FADING";
    case AutoDJProcessor::ADJ_ENABLE_P1LOADED:
        return "ENABLE_P1LOADED";
    case AutoDJProcessor::ADJ_ENABLE_P1PLAYING:
        return "ENABLE_P1PLAYING";
    case AutoDJProcessor::ADJ_DISABLED:
        return "DISABLED";
    }
    return "UNKNOWN";
}

// True when the from-deck has reached its fade point, or the engine has
// stopped it at EOF before playposition caught up to 1.0 / fadeBeginPos.
bool fromDeckReachedFadeOrEnd(
        const DeckAttributes* pDeck, double playPosition, double durationSec) {
    if (!pDeck->isFromDeck) {
        return false;
    }
    if (playPosition >= pDeck->fadeBeginPos || playPosition >= 1.0) {
        return true;
    }
    return durationSec > 0.0 &&
            (1.0 - playPosition) * durationSec <= kMinimumTrackDurationSec;
}
} // anonymous namespace

DeckAttributes::DeckAttributes(int index,
        BaseTrackPlayer* pPlayer)
        : index(index),
          group(pPlayer->getGroup()),
          startPos(kKeepPosition),
          fadeBeginPos(1.0),
          fadeEndPos(1.0),
          isFromDeck(false),
          loading(false),
          m_orientation(group, "orientation"),
          m_playPos(group, "playposition"),
          m_play(group, "play"),
          m_repeat(group, "repeat"),
          m_introStartPos(group, "intro_start_position"),
          m_introEndPos(group, "intro_end_position"),
          m_outroStartPos(group, "outro_start_position"),
          m_outroEndPos(group, "outro_end_position"),
          m_trackSamples(group, "track_samples"),
          m_sampleRate(group, "track_samplerate"),
          m_rateRatio(group, "rate_ratio"),
          m_pPlayer(pPlayer) {
    connect(m_pPlayer, &BaseTrackPlayer::newTrackLoaded,
            this, &DeckAttributes::slotTrackLoaded);
    connect(m_pPlayer, &BaseTrackPlayer::loadingTrack,
            this, &DeckAttributes::slotLoadingTrack);
    connect(m_pPlayer, &BaseTrackPlayer::playerEmpty,
            this, &DeckAttributes::slotPlayerEmpty);
    m_playPos.connectValueChanged(this, &DeckAttributes::slotPlayPosChanged);
    m_play.connectValueChanged(this, &DeckAttributes::slotPlayChanged);
    m_introStartPos.connectValueChanged(this, &DeckAttributes::slotIntroStartPositionChanged);
    m_introEndPos.connectValueChanged(this, &DeckAttributes::slotIntroEndPositionChanged);
    m_outroStartPos.connectValueChanged(this, &DeckAttributes::slotOutroStartPositionChanged);
    m_outroEndPos.connectValueChanged(this, &DeckAttributes::slotOutroEndPositionChanged);
    m_rateRatio.connectValueChanged(this, &DeckAttributes::slotRateChanged);
    m_orientation.connectValueChanged(this, &DeckAttributes::slotOrientationChanged);
}

DeckAttributes::~DeckAttributes() {
}

void DeckAttributes::slotPlayChanged(double v) {
    emit playChanged(this, v > 0.0);
}

void DeckAttributes::slotPlayPosChanged(double v) {
    emit playPositionChanged(this, v);
}

void DeckAttributes::slotIntroStartPositionChanged(double v) {
    emit introStartPositionChanged(this, v);
}

void DeckAttributes::slotIntroEndPositionChanged(double v) {
    emit introEndPositionChanged(this, v);
}

void DeckAttributes::slotOutroStartPositionChanged(double v) {
    emit outroStartPositionChanged(this, v);
}

void DeckAttributes::slotOutroEndPositionChanged(double v) {
    emit outroEndPositionChanged(this, v);
}

void DeckAttributes::slotTrackLoaded(TrackPointer pTrack) {
    emit trackLoaded(this, pTrack);
}

void DeckAttributes::slotLoadingTrack(TrackPointer pNewTrack, TrackPointer pOldTrack) {
    // qDebug() << "DeckAttributes::slotLoadingTrack";
    emit loadingTrack(this, pNewTrack, pOldTrack);
}

void DeckAttributes::slotPlayerEmpty() {
    emit playerEmpty(this);
}

void DeckAttributes::slotRateChanged(double v) {
    Q_UNUSED(v);
    emit rateChanged(this);
}

void DeckAttributes::slotOrientationChanged(double v) {
    Q_UNUSED(v);
    emit orientationChanged(this);
}

TrackPointer DeckAttributes::getLoadedTrack() const {
    return m_pPlayer != nullptr ? m_pPlayer->getLoadedTrack() : TrackPointer();
}

AutoDJProcessor::AutoDJProcessor(
        QObject* pParent,
        UserSettingsPointer pConfig,
        PlayerManagerInterface* pPlayerManager,
        TrackCollectionManager* pTrackCollectionManager,
        int iAutoDJPlaylistId)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pAutoDJTableModel(nullptr),
          m_eState(ADJ_DISABLED),
          m_transitionProgress(0.0),
          m_transitionTime(kTransitionPreferenceDefault),
          m_pPlayerManager(pPlayerManager),
          m_coCrossfader(QStringLiteral("[Master]"), QStringLiteral("crossfader")),
          m_coCrossfaderReverse(QStringLiteral("[Mixer Profile]"), QStringLiteral("xFaderReverse")),
          m_shufflePlaylist(ConfigKey(kControlGroup, QStringLiteral("shuffle_playlist"))),
          m_skipNext(ConfigKey(kControlGroup, QStringLiteral("skip_next"))),
          m_addRandomTrack(ConfigKey(kControlGroup, QStringLiteral("add_random_track"))),
          m_fadeNow(ConfigKey(kControlGroup, QStringLiteral("fade_now"))),
          m_enabledAutoDJ(ConfigKey(kControlGroup, QStringLiteral("enabled"))) {
    m_pTrackCollectionManager = pTrackCollectionManager;
    m_pAutoDJTableModel = make_parented<PlaylistTableModel>(
            this, pTrackCollectionManager, "mixxx.db.model.autodj");
    m_pAutoDJTableModel->selectPlaylist(iAutoDJPlaylistId);
    m_pAutoDJTableModel->select();

    connect(&m_shufflePlaylist,
            &ControlPushButton::valueChanged,
            this,
            &AutoDJProcessor::controlShuffle);
    connect(&m_skipNext, &ControlObject::valueChanged, this, &AutoDJProcessor::controlSkipNext);
    connect(&m_addRandomTrack,
            &ControlObject::valueChanged,
            this,
            &AutoDJProcessor::controlAddRandomTrack);
    connect(&m_fadeNow, &ControlObject::valueChanged, this, &AutoDJProcessor::controlFadeNow);
    m_enabledAutoDJ.setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_enabledAutoDJ.connectValueChangeRequest(this,
            &AutoDJProcessor::controlEnableChangeRequest);

    connect(pPlayerManager,
            &PlayerManagerInterface::numberOfDecksChanged,
            this,
            &AutoDJProcessor::slotNumberOfDecksChanged);
    slotNumberOfDecksChanged(pPlayerManager->numberOfDecks());

    QString str_autoDjTransition = m_pConfig->getValueString(
            ConfigKey(kPreferenceGroup, kTransitionPreferenceName));
    if (!str_autoDjTransition.isEmpty()) {
        m_transitionTime = str_autoDjTransition.toDouble();
    }

    m_transitionMode = m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, kTransitionModePreferenceName),
            TransitionMode::FullIntroOutro);
}

void AutoDJProcessor::slotNumberOfDecksChanged(int decks) {
    m_decks.reserve(decks);
    // Add more decks if we have not all yet.
    // Mixxx does not support reducing the number of deck
    for (int i = static_cast<int>(m_decks.size()); i < decks; ++i) {
        BaseTrackPlayer* pPlayer = m_pPlayerManager->getDeckBase(i);
        // Shouldn't be possible.
        VERIFY_OR_DEBUG_ASSERT(pPlayer) {
            return;
        }
        m_decks.emplace_back(std::make_unique<DeckAttributes>(i, pPlayer));
    }
}

double AutoDJProcessor::getCrossfader() const {
    if (m_coCrossfaderReverse.toBool()) {
        return m_coCrossfader.get() * -1.0;
    }
    return m_coCrossfader.get();
}

void AutoDJProcessor::setCrossfader(double value) {
    if (m_coCrossfaderReverse.toBool()) {
        value *= -1.0;
    }
    m_coCrossfader.set(value);
}

AutoDJProcessor::AutoDJError AutoDJProcessor::shufflePlaylist(
        const QModelIndexList& selectedIndices) {
    QModelIndex exclude;
    if (m_eState != ADJ_DISABLED) {
        exclude = m_pAutoDJTableModel->index(0, 0);
    }
    m_pAutoDJTableModel->shuffleTracks(selectedIndices, exclude);
    return ADJ_OK;
}

AutoDJProcessor::AutoDJError AutoDJProcessor::smartSortPlaylist() {
    if (m_smartSortRunning) {
        return ADJ_OK;
    }

    // Snapshot the queue on this (GUI) thread. The worker thread only sees
    // plain TrackFeatures copies, never Track objects or the database.
    struct Row {
        TrackId id;
        int position;
        TrackFeatures features;
    };
    QList<Row> rows;
    const int positionColumn = m_pAutoDJTableModel->fieldIndex(
            ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    const int rowCount = m_pAutoDJTableModel->rowCount();
    rows.reserve(rowCount);
    for (int i = 0; i < rowCount; ++i) {
        const QModelIndex index = m_pAutoDJTableModel->index(i, 0);
        const TrackPointer pTrack = m_pAutoDJTableModel->getTrack(index);
        if (!pTrack) {
            emit smartSortFailed(tr("Could not read track %1 of the Auto DJ queue.").arg(i + 1));
            return ADJ_OK;
        }
        rows.append(Row{pTrack->getId(),
                index.siblingAtColumn(positionColumn).data().toInt(),
                TrackFeatures::fromTrack(pTrack)});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.position < b.position;
    });

    // Energy scores live in their own table (see EnergyStore).
    if (m_pTrackCollectionManager && m_pTrackCollectionManager->internalCollection()) {
        QList<TrackId> ids;
        ids.reserve(rows.size());
        for (const Row& row : std::as_const(rows)) {
            ids.append(row.id);
        }
        const QHash<TrackId, EnergyStore::Value> energies = EnergyStore::loadEnergies(
                m_pTrackCollectionManager->internalCollection()->database(), ids);
        for (Row& row : rows) {
            const EnergyStore::Value value = energies.value(row.id);
            row.features.energy = value.energy;
            row.features.energyIsManual = value.manual;
        }
    }

    if (rows.size() < 2) {
        emit smartSortFinished(static_cast<int>(rows.size()), 0, QStringList(), QStringList());
        return ADJ_OK;
    }

    QVector<TrackFeatures> features;
    QList<std::pair<TrackId, int>> snapshot;
    features.reserve(rows.size());
    snapshot.reserve(rows.size());
    for (const Row& row : std::as_const(rows)) {
        features.append(row.features);
        snapshot.append(std::make_pair(row.id, row.position));
    }

    // While Auto DJ runs, the top track is already loaded on the next deck,
    // so it must stay first.
    std::optional<TrackId> startId;
    if (m_eState != ADJ_DISABLED) {
        startId = rows.first().id;
    }

    // TODO(autodj-2): read weights from the Auto DJ preferences.
    const MixScorer scorer;

    m_smartSortRunning = true;
    auto* pWatcher = new QFutureWatcher<SequenceResult>(this);
    connect(pWatcher,
            &QFutureWatcher<SequenceResult>::finished,
            this,
            [this, pWatcher, snapshot]() {
                m_smartSortRunning = false;
                applySmartSortResult(pWatcher->result(), snapshot);
                pWatcher->deleteLater();
            });
    QVector<TrackFeatures> bridgeCandidates;
    if (m_pTrackCollectionManager && m_pTrackCollectionManager->internalCollection()) {
        bridgeCandidates = loadBridgeCandidates(
                m_pTrackCollectionManager->internalCollection()->database());
    }
    pWatcher->setFuture(QtConcurrent::run([features, startId, scorer, bridgeCandidates]() {
        return SmartSequencer(scorer).solve(features, startId, 2000, bridgeCandidates);
    }));
    return ADJ_OK;
}

bool AutoDJProcessor::isBeatmatchEnabled() const {
    return m_pConfig->getValue(ConfigKey(kPreferenceGroup, kBeatmatchPreference), true);
}

void AutoDJProcessor::setBeatmatchEnabled(bool enabled) {
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kBeatmatchPreference), enabled);
}

void AutoDJProcessor::beginSmartTransition(
        DeckAttributes* pFromDeck, DeckAttributes* pToDeck) {
    endSmartTransition(false); // safety: never two at once
    // If the outgoing track is still gliding back (a short track), hold its
    // tempo steady during this mix; it is reset once it has faded out.
    m_glide.pDeck = nullptr;
    if (!isBeatmatchEnabled() || !pFromDeck || !pToDeck) {
        return;
    }
    m_smart = SmartTransition();
    m_smart.active = true;
    m_smart.pFrom = pFromDeck;
    m_smart.pTo = pToDeck;
    m_smart.fromLowKill = readControl(eqKillKey(pFromDeck->group));
    m_smart.toLowKill = readControl(eqKillKey(pToDeck->group));

    // Tempo: play the incoming track at the outgoing track's tempo, but only
    // within the 5% rule; otherwise it stays a plain crossfade.
    const TrackPointer pToTrack = pToDeck->getLoadedTrack();
    const ConfigKey fromBpmKey(pFromDeck->group, QStringLiteral("bpm"));
    const double fromBpm = readControl(fromBpmKey); // includes its tempo change
    const double toTrackBpm = pToTrack ? pToTrack->getBpm() : 0.0;
    std::optional<double> ratio;
    if (!std::isnan(fromBpm)) {
        ratio = beatmatch::matchRatio(fromBpm, toTrackBpm, kBeatmatchTolerancePct);
    }
    const ConfigKey toRatioKey(pToDeck->group, QStringLiteral("rate_ratio"));
    if (ratio && ControlObject::exists(toRatioKey)) {
        const ConfigKey keylockKey(pToDeck->group, QStringLiteral("keylock"));
        const ConfigKey quantizeKey(pToDeck->group, QStringLiteral("quantize"));
        m_smart.toKeylock = readControl(keylockKey);
        m_smart.toQuantize = readControl(quantizeKey);
        if (!m_keylockBefore.contains(pToDeck->group)) {
            m_keylockBefore.insert(pToDeck->group, m_smart.toKeylock);
        }
        writeControl(keylockKey, 1.0);  // tempo change without pitch change
        writeControl(quantizeKey, 1.0); // start on a beat
        ControlObject::set(toRatioKey, *ratio);
        m_smart.beatmatched = true;
        m_smart.toRatio = *ratio;
        kLogger.info() << "Beatmatch" << pToDeck->group << "at ratio" << *ratio
                       << "(" << toTrackBpm << "->" << fromBpm << "BPM)";
    } else {
        kLogger.info() << "No beatmatch for" << pToDeck->group
                       << "(" << toTrackBpm << "vs" << fromBpm
                       << "BPM): plain crossfade";
    }
    updateSmartTransition(0.0); // incoming bass starts cut
}

void AutoDJProcessor::afterToDeckStarted() {
    if (!m_smart.active || !m_smart.beatmatched) {
        return;
    }
    // Line the beats up with the outgoing track. A push button only acts on
    // a change, so press and release.
    const ConfigKey phaseKey(m_smart.pTo->group, QStringLiteral("beatsync_phase"));
    writeControl(phaseKey, 1.0);
    writeControl(phaseKey, 0.0);
}

void AutoDJProcessor::updateSmartTransition(double progress) {
    if (!m_smart.active) {
        return;
    }
    const beatmatch::BassState bass = beatmatch::bassSwap(progress);
    if (!std::isnan(m_smart.fromLowKill)) {
        writeControl(eqKillKey(m_smart.pFrom->group), bass.fromLowKilled ? 1.0 : 0.0);
    }
    if (!std::isnan(m_smart.toLowKill)) {
        writeControl(eqKillKey(m_smart.pTo->group), bass.toLowKilled ? 1.0 : 0.0);
    }
}

void AutoDJProcessor::endSmartTransition(bool completed) {
    if (!m_smart.active) {
        return;
    }
    // Put the EQ kills back as the DJ had them.
    writeControl(eqKillKey(m_smart.pFrom->group), m_smart.fromLowKill);
    writeControl(eqKillKey(m_smart.pTo->group), m_smart.toLowKill);
    if (m_smart.beatmatched) {
        writeControl(ConfigKey(m_smart.pTo->group, QStringLiteral("quantize")),
                m_smart.toQuantize);
        if (completed) {
            // Ease the new track back to its own tempo, too slowly to hear.
            m_glide.pDeck = m_smart.pTo;
            m_glide.startRatio = m_smart.toRatio;
            m_glide.timer.start();
        }
    }
    if (completed) {
        // The outgoing track has stopped: put its deck back to normal tempo.
        resetDeckTempo(m_smart.pFrom);
    }
    m_smart = SmartTransition();
}

void AutoDJProcessor::updateGlide(DeckAttributes* pDeck) {
    if (!pDeck || pDeck != m_glide.pDeck) {
        return;
    }
    const double ratio = beatmatch::glideRatio(
            m_glide.startRatio, m_glide.timer.elapsed() / 1000.0);
    writeControl(ConfigKey(pDeck->group, QStringLiteral("rate_ratio")), ratio);
    if (ratio == 1.0) {
        m_glide.pDeck = nullptr;
        resetDeckTempo(pDeck); // back at its own tempo: restore key lock
    }
}

void AutoDJProcessor::alignTransitionToPhrases(DeckAttributes* pFromDeck,
        DeckAttributes* pToDeck,
        double fromDeckPositionSec) {
    const TrackPointer pFromTrack = pFromDeck->getLoadedTrack();
    const TrackPointer pToTrack = pToDeck->getLoadedTrack();
    if (!isBeatmatchEnabled() || !pFromTrack || !pToTrack) {
        return;
    }
    // calculateTransition runs on every cueing seek (while paused, for both
    // directions in turn): log each result once per pair of tracks.
    static QHash<QString, QString> s_lastLog;
    const QString pair = pFromTrack->getId().toString() + QChar('>') +
            pToTrack->getId().toString();
    const auto logOnce = [&](const QString& key, const QString& text) {
        if (s_lastLog.value(pair) != key) {
            s_lastLog.insert(pair, key);
            kLogger.info() << "Phrase align:" << pair << text;
        }
    };
    // A start just below 0 s is not "keep position": the intro start marker
    // can snap to a beat slightly before the track begins.
    if (pToDeck->startPos == kKeepPosition || m_transitionTime < 0.0 ||
            pFromDeck->fadeEndPos <= pFromDeck->fadeBeginPos) {
        // a special case (jump cut / keep position / pause between tracks)
        logOnce(QStringLiteral(" special"),
                QStringLiteral("skipped, special timing (start %1, fade %2 -> %3)")
                        .arg(pToDeck->startPos)
                        .arg(pFromDeck->fadeBeginPos)
                        .arg(pFromDeck->fadeEndPos));
        return;
    }
    const mixxx::BeatsPointer pFromBeats = pFromTrack->getBeats();
    const mixxx::BeatsPointer pToBeats = pToTrack->getBeats();
    const double fromBpm = pFromTrack->getBpm();
    const double toBpm = pToTrack->getBpm();
    if (!pFromBeats || !pToBeats || !(fromBpm > 0.0) || !(toBpm > 0.0)) {
        logOnce(QStringLiteral(" nogrid"), QStringLiteral("skipped, a track has no beat grid"));
        return;
    }
    // Only when the mix will really be beatmatched (the 5% rule).
    const double fromRatio = pFromDeck->rateRatio();
    if (!beatmatch::matchRatio(fromBpm * fromRatio, toBpm, kBeatmatchTolerancePct)) {
        logOnce(QStringLiteral(" tempo"),
                QStringLiteral("skipped, tempo too far apart (%1 vs %2 BPM)")
                        .arg(fromBpm * fromRatio)
                        .arg(toBpm));
        return;
    }
    // Seconds here are real time at each deck's current speed, the same
    // convention as the rest of calculateTransition.
    phrasealign::Grid from;
    from.firstBeatSec = framePositionToSeconds(pFromBeats->firstBeat(), pFromDeck);
    from.beatSec = 60.0 / (fromBpm * fromRatio);
    phrasealign::Grid to;
    to.firstBeatSec = framePositionToSeconds(pToBeats->firstBeat(), pToDeck);
    to.beatSec = 60.0 / (toBpm * pToDeck->rateRatio());

    // Keep the length the transition mode chose (intro/outro or the
    // seconds setting), rounded to whole 8-bar phrases.
    double wantedSec = pFromDeck->fadeEndPos - pFromDeck->fadeBeginPos;

    // The DJ can teach where the main beat kicks in by setting the incoming
    // track's Intro End marker. That beats the measured guess. It also makes
    // the Intro/Outro mode use the whole intro as the fade length, so use
    // the seconds setting for the length instead.
    double toMarkedBeatSec = -1.0;
    const mixxx::audio::FramePos introEnd = pToDeck->introEndPosition();
    if (introEnd.isValid() && introEnd <= pToDeck->trackEndPosition()) {
        toMarkedBeatSec = framePositionToSeconds(introEnd, pToDeck);
        if (m_transitionTime > 0.0) {
            wantedSec = m_transitionTime;
        }
    }
    const int bars = phrasealign::barsForSeconds(wantedSec, from.beatSec);

    // Where each track's "body" is (from the energy analysis): the fade must
    // be over before the outgoing track starts fading out on its own, and a
    // long quiet intro on the incoming track is partly skipped so its beat
    // kicks in as the fade ends.
    // Not fadeEndPos: after the first run that is our own earlier plan, and
    // a tiny tempo change (a glide) then made no phrase fit any more.
    double fromLimitSec = getOutroEndSecond(pFromDeck);
    double toBodyStartSec = -1.0;
    // The outgoing track's Outro Start marker (set by the analysis or by the
    // DJ) is where its energy starts to go: the fade must be over by then.
    const mixxx::audio::FramePos fromOutroStart = pFromDeck->outroStartPosition();
    const bool fromOutroMarked = fromOutroStart.isValid() &&
            fromOutroStart <= pFromDeck->trackEndPosition();
    if (fromOutroMarked) {
        fromLimitSec = std::min(fromLimitSec, framePositionToSeconds(fromOutroStart, pFromDeck));
    }
    QString introSource = QStringLiteral("Intro End marker");
    if (m_pTrackCollectionManager && m_pTrackCollectionManager->internalCollection()) {
        const QSqlDatabase db = m_pTrackCollectionManager->internalCollection()->database();
        if (!fromOutroMarked) {
            if (const auto fromBody = EnergyStore::loadBody(db, pFromTrack->getId())) {
                fromLimitSec = std::min(fromLimitSec, fromBody->endSec / fromRatio);
            }
        }
        if (const auto toBody = EnergyStore::loadBody(db, pToTrack->getId())) {
            toBodyStartSec = toBody->startSec / pToDeck->rateRatio();
        }
        if (toMarkedBeatSec >= 0.0) {
            const double markerTrackSec = toMarkedBeatSec * pToDeck->rateRatio();
            const auto autoMarkers = EnergyStore::loadAutoMarkers(db, pToTrack->getId());
            introSource = autoMarkers.introEndSec >= 0.0 &&
                            std::fabs(markerTrackSec - autoMarkers.introEndSec) < 0.02
                    ? QStringLiteral("Intro End marker, set by analysis")
                    : QStringLiteral("Intro End marker, set by DJ");
        }
    }
    const bool toBodyMarked = toMarkedBeatSec >= 0.0;
    if (toBodyMarked) {
        toBodyStartSec = toMarkedBeatSec;
    }
    // The incoming track starts no earlier than this. When we know where
    // its beat kicks in, use its intro start (first sound): the deck's cue
    // position is our own earlier plan (this function runs again after every
    // cueing seek), and re-using it pushed the start a whole phrase later
    // each time, until the beat came in at the START of the fade instead of
    // at its middle. Without a known beat, keep the cue position so a
    // manual cue by the DJ is respected.
    const double toEarliestSec = toBodyStartSec >= 0.0
            ? std::max(0.0, getIntroStartSecond(pToDeck))
            : pToDeck->startPos;
    const auto plan = phrasealign::plan(from,
            to,
            fromDeckPositionSec,
            fromLimitSec, // the fade must be over by here
            toEarliestSec,
            bars,
            toBodyStartSec,
            toBodyMarked);
    if (!plan) {
        logOnce(QStringLiteral(" none"),
                QStringLiteral("no phrase fits before %1 s (now at %2 s), keeping the plain timing")
                        .arg(fromLimitSec)
                        .arg(fromDeckPositionSec));
        return;
    }
    // Key on track time (not real time), so the outgoing track easing back
    // to its own tempo does not log a new line many times a second.
    logOnce(QStringLiteral(" %1 %2")
                    .arg(plan->fromFadeBeginSec * fromRatio, 0, 'f', 1)
                    .arg(plan->toStartSec * pToDeck->rateRatio(), 0, 'f', 1),
            QStringLiteral("%1 bars, fade %2 -> %3 s (limit %4 s), incoming starts at "
                           "%5 s (beat at %6 s, %7)")
                    .arg(bars)
                    .arg(plan->fromFadeBeginSec)
                    .arg(plan->fromFadeEndSec)
                    .arg(fromLimitSec)
                    .arg(plan->toStartSec)
                    .arg(toBodyStartSec)
                    .arg(toBodyMarked ? introSource : QStringLiteral("measured"))
                    .append(fromOutroMarked ? QStringLiteral(", limit from Outro Start")
                                            : QString()));
    pFromDeck->fadeBeginPos = plan->fromFadeBeginSec;
    pFromDeck->fadeEndPos = plan->fromFadeEndSec;
    pToDeck->startPos = plan->toStartSec;
    // A re-plan (e.g. after a tempo step) does not re-cue the waiting track
    // by itself. If it is not where the plan needs it, move it there, so its
    // beats and phrases line up. The seek re-plans once more; the answer is
    // then the same, so it stops.
    const double toDuration = getEndSecond(pToDeck);
    if (!pToDeck->isPlaying() && toDuration > 0.0) {
        const double toNowSec = pToDeck->playPosition() * toDuration;
        if (std::fabs(toNowSec - plan->toStartSec) > 0.05) {
            pToDeck->setPlayPosition(plan->toStartSec / toDuration);
        }
    }
}

void AutoDJProcessor::resetDeckTempo(DeckAttributes* pDeck) {
    if (!pDeck || !m_keylockBefore.contains(pDeck->group)) {
        return; // we never changed this deck
    }
    if (m_glide.pDeck == pDeck) {
        m_glide.pDeck = nullptr;
    }
    writeControl(ConfigKey(pDeck->group, QStringLiteral("rate_ratio")), 1.0);
    writeControl(ConfigKey(pDeck->group, QStringLiteral("keylock")),
            m_keylockBefore.take(pDeck->group));
}

bool AutoDJProcessor::setEnergyRating(const QList<TrackId>& trackIds, int rating) {
    if (!m_pTrackCollectionManager || !m_pTrackCollectionManager->internalCollection()) {
        return false;
    }
    return EnergyStore::setManualRating(
            m_pTrackCollectionManager->internalCollection()->database(), trackIds, rating);
}

std::pair<double, bool> AutoDJProcessor::energyOf(TrackId trackId) const {
    if (!m_pTrackCollectionManager || !m_pTrackCollectionManager->internalCollection()) {
        return {0.0, false};
    }
    const auto energies = EnergyStore::loadEnergies(
            m_pTrackCollectionManager->internalCollection()->database(), {trackId});
    const EnergyStore::Value value = energies.value(trackId);
    return {value.energy, value.manual};
}

int AutoDJProcessor::insertPendingBridges() {
    const QList<std::pair<int, TrackId>> bridges = m_pendingBridges;
    m_pendingBridges.clear();
    if (bridges.isEmpty() || !m_pTrackCollectionManager ||
            !m_pTrackCollectionManager->internalCollection()) {
        return 0;
    }
    // Only if the queue is still exactly the sorted order.
    const QList<std::pair<TrackId, int>> current = m_pAutoDJTableModel->getTrackIdsAndPositions();
    if (current.size() != m_pendingBridgeOrder.size()) {
        return 0;
    }
    for (int i = 0; i < current.size(); ++i) {
        if (current[i].first != m_pendingBridgeOrder[i]) {
            return 0;
        }
    }
    PlaylistDAO& playlistDao =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    const int playlistId = m_pAutoDJTableModel->getPlaylist();
    int added = 0;
    // Last gap first, so the positions of earlier gaps do not move.
    for (auto it = bridges.crbegin(); it != bridges.crend(); ++it) {
        const int k = it->first; // insert after the k-th track (from 1)
        VERIFY_OR_DEBUG_ASSERT(k >= 1 && k <= current.size()) {
            continue;
        }
        if (playlistDao.insertTrackIntoPlaylist(it->second, playlistId, current[k - 1].second + 1)) {
            ++added;
        }
    }
    m_pAutoDJTableModel->select();
    return added;
}

void AutoDJProcessor::applySmartSortResult(const SequenceResult& result,
        const QList<std::pair<TrackId, int>>& snapshot) {
    // The DJ (or Auto DJ itself) may have changed the queue while we sorted.
    if (m_pAutoDJTableModel->getTrackIdsAndPositions() != snapshot) {
        emit smartSortFailed(tr("The Auto DJ queue changed while sorting. Please try again."));
        return;
    }
    if (result.order.size() != snapshot.size()) {
        emit smartSortFailed(tr("Smart sort could not sort the Auto DJ queue."));
        return;
    }

    // A track can be in the queue more than once, so map each id to the
    // list of positions it holds and hand them out in turn.
    QHash<TrackId, QList<int>> positionsById;
    for (const auto& [id, position] : snapshot) {
        positionsById[id].append(position);
    }
    QList<std::pair<TrackId, int>> newOrder;
    newOrder.reserve(result.order.size());
    for (const TrackId& id : result.order) {
        QList<int>& positions = positionsById[id];
        VERIFY_OR_DEBUG_ASSERT(!positions.isEmpty()) {
            emit smartSortFailed(tr("Smart sort could not sort the Auto DJ queue."));
            return;
        }
        newOrder.append(std::make_pair(id, positions.takeFirst()));
    }

    m_pAutoDJTableModel->setTrackOrder(newOrder);
    m_pendingBridges = result.bestBridges;
    m_pendingBridgeOrder = result.order;
    emit smartSortFinished(static_cast<int>(result.order.size()),
            result.clashCount,
            result.warnings,
            result.orderLines);
}

void AutoDJProcessor::fadeNow() {
    if (m_eState != ADJ_IDLE) {
        // we cannot fade if AutoDj is disabled or already fading
        return;
    }

    double crossfader = getCrossfader();
    DeckAttributes* pLeftDeck = getLeftDeck();
    DeckAttributes* pRightDeck = getRightDeck();
    if (!pLeftDeck || !pRightDeck) {
        // User has changed the orientation, disable Auto DJ
        toggleAutoDJ(false);
        emit autoDJError(ADJ_NOT_TWO_DECKS);
        return;
    }

    DeckAttributes* pFromDeck;
    DeckAttributes* pToDeck;

    if (pLeftDeck->isPlaying() &&
            (!pRightDeck->isPlaying() || crossfader < 0.0)) {
        pFromDeck = pLeftDeck;
        pToDeck = pRightDeck;
    } else if (pRightDeck->isPlaying()) {
        pFromDeck = pRightDeck;
        pToDeck = pLeftDeck;
    } else {
        // Neither deck is playing. Fading now makes no sense.
        return;
    }

    pFromDeck->setRepeat(false);
    pFromDeck->isFromDeck = true;
    pToDeck->isFromDeck = false;

    const double fromDeckEndSecond = getEndSecond(pFromDeck);
    const double toDeckEndSecond = getEndSecond(pToDeck);
    // Since the end position is measured in seconds from 0:00 it is also
    // the track duration. Use this alias for better readability.
    const double fromDeckDuration = fromDeckEndSecond;
    const double toDeckDuration = toDeckEndSecond;
    if (toDeckDuration < kMinimumTrackDurationSec) {
        // Deck is empty or track too short, disable AutoDJ
        // This happens only if the user has changed deck orientation to such deck.
        toggleAutoDJ(false);
        emit autoDJError(ADJ_NOT_TWO_DECKS);
        return;
    }

    // playPosition() is in the range of 0..1
    const double fromDeckCurrentSecond = fromDeckDuration * pFromDeck->playPosition();
    const double toDeckCurrentSecond = toDeckDuration * pToDeck->playPosition();

    if (toDeckDuration - toDeckCurrentSecond < kMinimumTrackDurationSec) {
        // Remaining Track time is too short, user has has seeked near the end
        // Re-cue the track
        pToDeck->setPlayPosition(pToDeck->startPos);
    }

    pFromDeck->fadeBeginPos = fromDeckCurrentSecond;
    // Do not seek to a calculated start point; start the to deck from wherever
    // it is if the user has seeked since loading the track.
    pToDeck->startPos = toDeckCurrentSecond;

    // If the user presses "Fade now", assume they want to fade *now*, not later.
    // So if the spinbox time is negative, do not insert silence.
    double spinboxTime = fabs(m_transitionTime);

    double fadeTime;
    if (m_transitionMode == TransitionMode::FullIntroOutro ||
            m_transitionMode == TransitionMode::FadeAtOutroStart) {
        // Use the intro length as the transition time. If the user has seeked
        // away from the intro start since the track was loaded, start from
        // there and do not seek back to the intro start. If they have seeked
        // past the introEnd or the introEnd is not marked, fall back to the
        // spinbox time.
        double outroEnd = getOutroEndSecond(pFromDeck);
        double introEnd = getIntroEndSecond(pToDeck);
        double introStart = getIntroStartSecond(pToDeck);
        double timeUntilOutroEnd = outroEnd - fromDeckCurrentSecond;

        // IntroStart ends up being equal to introEnd when pToDeck is
        // paused and its introEnd marker is not set. getIntroEndSecond returns
        // introStart thus the two end up having equal values
        if (toDeckCurrentSecond >= introStart &&
                toDeckCurrentSecond <= introEnd &&
                introStart != introEnd) {
            double timeUntilIntroEnd = introEnd - toDeckCurrentSecond;
            // The fade must end by the outro end at the latest.
            fadeTime = math_min(timeUntilIntroEnd, timeUntilOutroEnd);
        } else {
            // If this is true, the fade should have already been started
            // so the user should not have been able to press the Fade button.
            VERIFY_OR_DEBUG_ASSERT(timeUntilOutroEnd > 0) {
                timeUntilOutroEnd = 0;
            }
            fadeTime = math_min(spinboxTime, timeUntilOutroEnd);
        }
    } else {
        fadeTime = spinboxTime;
    }

    fadeTime = math_min(fadeTime, fromDeckEndSecond - fromDeckCurrentSecond);
    fadeTime = math_min(fadeTime,
            (toDeckEndSecond - toDeckCurrentSecond) / 2); // for fade in and out

    pFromDeck->fadeEndPos = fromDeckCurrentSecond + fadeTime;

    // These are expected to be a fraction of the track length.
    pFromDeck->fadeBeginPos /= fromDeckDuration;
    pFromDeck->fadeEndPos /= fromDeckDuration;
    pToDeck->startPos /= toDeckDuration;

    VERIFY_OR_DEBUG_ASSERT(pFromDeck->fadeBeginPos <= 1) {
        pFromDeck->fadeBeginPos = 1;
    }
}

AutoDJProcessor::AutoDJError AutoDJProcessor::skipNext() {
    if (m_eState == ADJ_DISABLED) {
        emit autoDJError(ADJ_IS_INACTIVE);
        return ADJ_IS_INACTIVE;
    }
    // Load the next song from the queue.
    DeckAttributes* pLeftDeck = getLeftDeck();
    DeckAttributes* pRightDeck = getRightDeck();
    if (!pLeftDeck || !pRightDeck) {
        // User has changed the orientation, disable Auto DJ
        toggleAutoDJ(false);
        emit autoDJError(ADJ_NOT_TWO_DECKS);
        return ADJ_NOT_TWO_DECKS;
    }

    if (!pLeftDeck->isPlaying()) {
        removeLoadedTrackFromTopOfQueue(*pLeftDeck);
        loadNextTrackFromQueue(*pLeftDeck);
    } else if (!pRightDeck->isPlaying()) {
        removeLoadedTrackFromTopOfQueue(*pRightDeck);
        loadNextTrackFromQueue(*pRightDeck);
    } else {
        // If both decks are playing remove next track in playlist
        TrackId nextId = m_pAutoDJTableModel->getTrackId(m_pAutoDJTableModel->index(0, 0));
        TrackId leftId = pLeftDeck->getLoadedTrack()->getId();
        TrackId rightId = pRightDeck->getLoadedTrack()->getId();
        if (nextId == leftId || nextId == rightId) {
        // One of the playing tracks is still on top of playlist, remove second item
            m_pAutoDJTableModel->removeTrack(m_pAutoDJTableModel->index(1, 0));
        } else {
            m_pAutoDJTableModel->removeTrack(m_pAutoDJTableModel->index(0, 0));
        }
        maybeFillRandomTracks();
    }
    return ADJ_OK;
}

AutoDJProcessor::AutoDJError AutoDJProcessor::toggleAutoDJ(bool enable) {
    if (enable) { // Enable Auto DJ
        DeckAttributes* pLeftDeck = getLeftDeck();
        DeckAttributes* pRightDeck = getRightDeck();
        if (!pLeftDeck || !pRightDeck) {
            // Keep the current state.
            emitAutoDJStateChanged(m_eState);
            emit autoDJError(ADJ_NOT_TWO_DECKS);
            return ADJ_NOT_TWO_DECKS;
        }

        bool leftDeckPlaying = pLeftDeck->isPlaying();
        bool rightDeckPlaying = pRightDeck->isPlaying();

        if (leftDeckPlaying && rightDeckPlaying) {
            qDebug() << "One deck must be stopped before enabling Auto DJ mode";
            // Keep the current state.
            emitAutoDJStateChanged(m_eState);
            emit autoDJError(ADJ_BOTH_DECKS_PLAYING);
            return ADJ_BOTH_DECKS_PLAYING;
        }
        // Auto-DJ needs at least two decks
        DEBUG_ASSERT(m_decks.size() > 1);

        // TODO: This is a total band aid for making Auto DJ work with four decks.
        // We should design a nicer way to handle this.
        for (const auto& pDeck : m_decks) {
            VERIFY_OR_DEBUG_ASSERT(pDeck) {
                continue;
            }
            if (pDeck.get() == pLeftDeck) {
                continue;
            }
            if (pDeck.get() == pRightDeck) {
                continue;
            }
            if (pDeck->isPlaying()) {
                // Keep the current state.
                emitAutoDJStateChanged(m_eState);
                emit autoDJError(ADJ_UNUSED_DECK_PLAYING);
                return ADJ_UNUSED_DECK_PLAYING;
            }
        }

        if (pLeftDeck->index > 1 || pRightDeck->index > 1) {
            // Left and/or right deck is deck 3/4 which may not be visible.
            // Make sure it is, if the current skin is a 4-deck skin.
            ControlObject::set(
                    ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("show_4decks")), 1);
        }

        // Never load the same track if it is already playing
        if (leftDeckPlaying) {
            removeLoadedTrackFromTopOfQueue(*pLeftDeck);
        } else if (rightDeckPlaying) {
            removeLoadedTrackFromTopOfQueue(*pRightDeck);
        } else {
            // If the first track is already cued at a position in the first
            // 2/3 in on of the Auto DJ decks, start it.
            // If the track is paused at a later position, it is probably too
            // close to the end. In this case it is loaded again at the stored
            // cue point.
            if (pLeftDeck->playPosition() < 0.66 &&
                    removeLoadedTrackFromTopOfQueue(*pLeftDeck)) {
                pLeftDeck->play();
                leftDeckPlaying = true;
            } else if (pRightDeck->playPosition() < 0.66 &&
                    removeLoadedTrackFromTopOfQueue(*pRightDeck)) {
                pRightDeck->play();
                rightDeckPlaying = true;
            }
        }

        TrackPointer nextTrack = getNextTrackFromQueue();
        if (!nextTrack) {
            qDebug() << "Queue is empty now, disable Auto DJ";
            m_enabledAutoDJ.setAndConfirm(0.0);
            emitAutoDJStateChanged(m_eState);
            emit autoDJError(ADJ_QUEUE_EMPTY);
            return ADJ_QUEUE_EMPTY;
        }

        // Track is available so GO
        m_enabledAutoDJ.setAndConfirm(1.0);
        qDebug() << "Auto DJ enabled";

        m_coCrossfader.connectValueChanged(this, &AutoDJProcessor::crossfaderChanged);

        connect(pLeftDeck,
                &DeckAttributes::playPositionChanged,
                this,
                &AutoDJProcessor::playerPositionChanged);
        connect(pRightDeck,
                &DeckAttributes::playPositionChanged,
                this,
                &AutoDJProcessor::playerPositionChanged);

        connect(pLeftDeck,
                &DeckAttributes::playChanged,
                this,
                &AutoDJProcessor::playerPlayChanged);
        connect(pRightDeck,
                &DeckAttributes::playChanged,
                this,
                &AutoDJProcessor::playerPlayChanged);

        connect(pLeftDeck,
                &DeckAttributes::introStartPositionChanged,
                this,
                &AutoDJProcessor::playerIntroStartChanged);
        connect(pRightDeck,
                &DeckAttributes::introStartPositionChanged,
                this,
                &AutoDJProcessor::playerIntroStartChanged);

        connect(pLeftDeck,
                &DeckAttributes::introEndPositionChanged,
                this,
                &AutoDJProcessor::playerIntroEndChanged);
        connect(pRightDeck,
                &DeckAttributes::introEndPositionChanged,
                this,
                &AutoDJProcessor::playerIntroEndChanged);

        connect(pLeftDeck,
                &DeckAttributes::outroStartPositionChanged,
                this,
                &AutoDJProcessor::playerOutroStartChanged);
        connect(pRightDeck,
                &DeckAttributes::outroStartPositionChanged,
                this,
                &AutoDJProcessor::playerOutroStartChanged);

        connect(pLeftDeck,
                &DeckAttributes::outroEndPositionChanged,
                this,
                &AutoDJProcessor::playerOutroEndChanged);
        connect(pRightDeck,
                &DeckAttributes::outroEndPositionChanged,
                this,
                &AutoDJProcessor::playerOutroEndChanged);

        connect(pLeftDeck,
                &DeckAttributes::trackLoaded,
                this,
                &AutoDJProcessor::playerTrackLoaded);
        connect(pRightDeck,
                &DeckAttributes::trackLoaded,
                this,
                &AutoDJProcessor::playerTrackLoaded);

        connect(pLeftDeck,
                &DeckAttributes::loadingTrack,
                this,
                &AutoDJProcessor::playerLoadingTrack);
        connect(pRightDeck,
                &DeckAttributes::loadingTrack,
                this,
                &AutoDJProcessor::playerLoadingTrack);

        connect(pLeftDeck,
                &DeckAttributes::playerEmpty,
                this,
                &AutoDJProcessor::playerEmpty);
        connect(pRightDeck,
                &DeckAttributes::playerEmpty,
                this,
                &AutoDJProcessor::playerEmpty);

        connect(pLeftDeck,
                &DeckAttributes::rateChanged,
                this,
                &AutoDJProcessor::playerRateChanged);
        connect(pRightDeck,
                &DeckAttributes::rateChanged,
                this,
                &AutoDJProcessor::playerRateChanged);

        connect(pLeftDeck,
                &DeckAttributes::orientationChanged,
                this,
                &AutoDJProcessor::playerOrientationChanged);
        connect(pRightDeck,
                &DeckAttributes::orientationChanged,
                this,
                &AutoDJProcessor::playerOrientationChanged);

        connect(m_pAutoDJTableModel,
                &PlaylistTableModel::firstTrackChanged,
                this,
                &AutoDJProcessor::playlistFirstTrackChanged);

        if (!leftDeckPlaying && !rightDeckPlaying) {
            // Both decks are stopped. Load a track into deck 1 and start it
            // playing. Instruct playerPositionChanged to wait for a
            // playposition update from deck 1. playerPositionChanged for
            // ADJ_ENABLE_P1LOADED will set the crossfader left and remove the
            // loaded track from the queue and wait for the next call to
            // playerPositionChanged for deck1 after the track is loaded.
            m_eState = ADJ_ENABLE_P1LOADED;

            // Move crossfader to the left.
            setCrossfader(-1.0);

            // Load track into the left deck and play. Once it starts playing,
            // we will receive a playerPositionChanged update for deck 1 which
            // will load a track into the right deck and switch to IDLE mode.
            emitLoadTrackToPlayer(nextTrack, pLeftDeck->group, true);
        } else {
            // One of the two decks is playing. Switch into IDLE mode and wait
            // until the playing deck crosses posThreshold to start fading.
            m_eState = ADJ_IDLE;
            if (leftDeckPlaying) {
                // Load track into the right deck.
                emitLoadTrackToPlayer(nextTrack, pRightDeck->group, false);
                // Move crossfader to the left.
                setCrossfader(-1.0);
            } else {
                // Load track into the left deck.
                emitLoadTrackToPlayer(nextTrack, pLeftDeck->group, false);
                // Move crossfader to the right.
                setCrossfader(1.0);
            }
        }
        emitAutoDJStateChanged(m_eState);
    } else { // Disable Auto DJ
        endSmartTransition(false);
        m_glide.pDeck = nullptr; // leave the tempo where it is
        m_enabledAutoDJ.setAndConfirm(0.0);
        qDebug() << "Auto DJ disabled";
        m_eState = ADJ_DISABLED;
        disconnect(&m_coCrossfader,
                &ControlProxy::valueChanged,
                this,
                &AutoDJProcessor::crossfaderChanged);
        disconnect(m_pAutoDJTableModel,
                &PlaylistTableModel::firstTrackChanged,
                this,
                &AutoDJProcessor::playlistFirstTrackChanged);
        for (const auto& pDeck : m_decks) {
            pDeck->disconnect(this);
        }
        if (m_pConfig->getValue<bool>(ConfigKey(kPreferenceGroup,
                    QStringLiteral("center_xfader_when_disabling")))) {
            m_coCrossfader.set(0);
        }
        emitAutoDJStateChanged(m_eState);
    }
    return ADJ_OK;
}

void AutoDJProcessor::controlEnableChangeRequest(double value) {
    toggleAutoDJ(value > 0.0);
}

void AutoDJProcessor::controlFadeNow(double value) {
    if (value > 0.0) {
        fadeNow();
    }
}

void AutoDJProcessor::controlShuffle(double value) {
    if (value > 0.0) {
        shufflePlaylist(QModelIndexList());
    }
}

void AutoDJProcessor::controlSkipNext(double value) {
    if (value > 0.0) {
        skipNext();
    }
}

void AutoDJProcessor::controlAddRandomTrack(double value) {
    if (value > 0.0) {
        emit randomTrackRequested(1);
    }
}

void AutoDJProcessor::crossfaderChanged(double value) {
    if (m_eState == ADJ_IDLE) {
        // The user is changing the crossfader manually. If the user has
        // moved it all the way to the other side, make the deck faded away
        // from the new "to deck" by loading the next track into it.
        DeckAttributes* pFromDeck = getFromDeck();
        VERIFY_OR_DEBUG_ASSERT(pFromDeck) {
            // we have always a from deck in case of state IDLE
            return;
        }

        DeckAttributes* pToDeck = getOtherDeck(pFromDeck);
        if (!pToDeck) {
            // we have always a from deck in case of state IDLE
            // if the user has not changed the deck orientation
            return;
        }

        double crossfaderPosition = value * (m_coCrossfaderReverse.toBool() ? -1 : 1);
        if ((crossfaderPosition == 1.0 && pFromDeck->isLeft()) ||       // crossfader right
                (crossfaderPosition == -1.0 && pFromDeck->isRight())) { // crossfader left
            if (kLogger.debugEnabled()) {
                kLogger.debug() << "crossfaderChanged force-advance"
                                << "from" << pFromDeck->group
                                << "to" << pToDeck->group
                                << "xfader" << crossfaderPosition;
            }
            if (!pToDeck->isPlaying()) {
                if (getEndSecond(pToDeck) >= kMinimumTrackDurationSec) {
                    // Re-cue the track if the user has seeked it to the very end
                    if (pToDeck->playPosition() >= pToDeck->fadeBeginPos) {
                        pToDeck->setPlayPosition(pToDeck->startPos);
                    }
                    pToDeck->play();
                } else {
                    // Track in toDeck was ejected manually, stop.
                    toggleAutoDJ(false);
                    return;
                }
            }
            pFromDeck->stop();

            // Now that we have started the other deck playing, remove the track
            // that was "on deck" from the top of the queue.
            removeLoadedTrackFromTopOfQueue(*pToDeck);
            loadNextTrackFromQueue(*pFromDeck);
        }
    }
}

void AutoDJProcessor::playerPositionChanged(DeckAttributes* pAttributes,
                                            double thisPlayPosition) {
    // qDebug() << "player" << pAttributes->group << "PositionChanged(" << value << ")";
    if (m_eState == ADJ_DISABLED) {
        // nothing to do
        return;
    }

    updateGlide(pAttributes);

    DeckAttributes* thisDeck = pAttributes;
    DeckAttributes* otherDeck = getOtherDeck(thisDeck);
    if (!otherDeck) {
        // This happens if this deck has no orientation or
        // there is no deck with the opposite orientation
        return;
    }

    // Note: this can be a delayed call of playerPositionChanged() where
    // the track was playing, but is now stopped.
    bool thisDeckPlaying = thisDeck->isPlaying();
    bool otherDeckPlaying = otherDeck->isPlaying();

    // To switch out of ADJ_ENABLE_P1LOADED we wait for a playposition update
    // for either deck.
    if (m_eState == ADJ_ENABLE_P1LOADED) {
        DeckAttributes* leftDeck;
        DeckAttributes* rightDeck;

        if (thisDeck->isLeft()) {
            leftDeck = thisDeck;
            DEBUG_ASSERT(otherDeck->isRight());
            rightDeck = otherDeck;
        } else {
            DEBUG_ASSERT(thisDeck->isRight());
            rightDeck = thisDeck;
            DEBUG_ASSERT(otherDeck->isLeft());
            leftDeck = otherDeck;
        }

        // Note: If a playing deck has reached the end the play state is already reset
        bool leftDeckPlaying = leftDeck->isPlaying();
        bool rightDeckPlaying = rightDeck->isPlaying();
        bool leftDeckReachesEnd = thisDeck->isLeft() && thisPlayPosition >= 1.0;

        if (leftDeckPlaying || rightDeckPlaying || leftDeckReachesEnd) {
            // One of left and right is playing. Switch to IDLE mode and make
            // sure our thresholds are configured (by calling calculateTransition
            // for the playing deck).
            m_eState = ADJ_IDLE;

            if (!rightDeckPlaying) {
                // Only left deck playing!
                // In ADJ_ENABLE_P1LOADED mode we wait until the left deck
                // successfully starts playing. We don't know in toggleAutoDJ
                // whether the track will load successfully so we have to
                // wait. If the track fails to load then playerTrackLoadFailed
                // will remove it from the top of the queue and request another
                // track. Remove the left deck's current track from the queue
                // since it is the track we requested in toggleAutoDJ.
                removeLoadedTrackFromTopOfQueue(*leftDeck);

                // Load the next track into the right player since it is not
                // playing.
                loadNextTrackFromQueue(*rightDeck);

                // Note: calculateTransition() is called in playerTrackLoaded()
            } else {
                // At least right deck is playing
                // Set crossfade thresholds for right deck.
                if (kLogger.debugEnabled()) {
                    kLogger.debug() << "playerPositionChanged"
                                    << "right deck playing";
                }
                calculateTransition(rightDeck, leftDeck, false);
            }
            emitAutoDJStateChanged(m_eState);
        }
        return;
    }

    // In FADING states, we expect that both tracks are playing.
    // Normally the the fading fromDeck stops after the transition is over and
    // we need to replace it with a new track from the queue.
    if (m_eState == ADJ_LEFT_FADING || m_eState == ADJ_RIGHT_FADING) {
        // Once P1 or P2 has stopped switch out of fading mode to idle.
        // If the user stops the toDeck during a fade, let the fade continue
        // and do not load the next track.
        if (!otherDeckPlaying && otherDeck->isFromDeck) {
            // Force crossfader all the way to the (non fading) toDeck.
            if (m_eState == ADJ_RIGHT_FADING) {
                setCrossfader(-1.0);
            } else {
                setCrossfader(1.0);
            }
            if (kLogger.debugEnabled()) {
                kLogger.debug() << "playerPositionChanged" << thisDeck->group
                                << "fade complete" << autoDJStateName(m_eState)
                                << "-> IDLE";
            }
            endSmartTransition(true);
            m_eState = ADJ_IDLE;
            // Invalidate threshold calculated for the old otherDeck
            // This avoids starting a fade back before the new track is
            // loaded into the otherDeck
            thisDeck->fadeBeginPos = 1.0;
            thisDeck->fadeEndPos = 1.0;
            otherDeck->isFromDeck = false;
            // Load the next track to otherDeck.
            loadNextTrackFromQueue(*otherDeck);
            emitAutoDJStateChanged(m_eState);
            return;
        }
    }

    const bool fromDeckAtFadeOrEnd = fromDeckReachedFadeOrEnd(
            thisDeck, thisPlayPosition, getEndSecond(thisDeck));

    if (m_eState == ADJ_IDLE) {
        if (!thisDeckPlaying && thisPlayPosition < 1) {
            // this is a cueing seek, recalculate the transition, from the
            // new position.
            // This can be our own seek to startPos or a random seek by a user.
            // we need to call calculateTransition() because we are not sure.
            // If using the full track mode with a transition time of 0,
            // thisDeckPlaying will be false but the transition should not be
            // recalculated here.
            // Don't adjust transition when reaching the end. In this case it is
            // always stopped. The engine may also stop play before playposition
            // reaches 1.0, so treat a from-deck stopped near EOF as the fade
            // point rather than a cueing seek (which would swap from/to roles).
            if (fromDeckAtFadeOrEnd) {
                if (kLogger.debugEnabled()) {
                    kLogger.debug() << "playerPositionChanged" << thisDeck->group
                                    << "from-deck stopped at EOF, start transition"
                                    << "pos" << thisPlayPosition
                                    << "fadeBegin" << thisDeck->fadeBeginPos;
                }
            } else {
                if (kLogger.debugEnabled()) {
                    kLogger.debug() << "playerPositionChanged" << thisDeck->group
                                    << "cueing seek" << thisPlayPosition;
                }
                calculateTransition(otherDeck, thisDeck, false);
            }
        } else if (thisDeck->isRepeat()) {
            // repeat pauses auto DJ
            return;
        }
    }

    // If we are past this deck's posThreshold then:
    // - transition into fading mode, play the other deck and fade to it.
    // - check if fading is done and stop the deck
    // - update the crossfader
    if (thisDeck->isFromDeck && !otherDeck->loading &&
            (thisPlayPosition >= thisDeck->fadeBeginPos ||
                    (!thisDeckPlaying && fromDeckAtFadeOrEnd))) {
        if (m_eState == ADJ_IDLE) {
            if (thisDeckPlaying || thisPlayPosition >= 1.0 || fromDeckAtFadeOrEnd) {
                // Set the state as FADING.
                m_eState = thisDeck->isLeft() ? ADJ_LEFT_FADING : ADJ_RIGHT_FADING;
                if (kLogger.debugEnabled()) {
                    kLogger.debug() << "playerPositionChanged" << thisDeck->group
                                    << "start fade" << autoDJStateName(m_eState)
                                    << "pos" << thisPlayPosition
                                    << "fadeBegin" << thisDeck->fadeBeginPos
                                    << "fadeEnd" << thisDeck->fadeEndPos
                                    << "playing" << thisDeckPlaying;
                }
                m_transitionProgress = 0.0;
                emitAutoDJStateChanged(m_eState);

                const double toDeckFadeDistance =
                        (thisDeck->fadeEndPos - thisDeck->fadeBeginPos) *
                        getEndSecond(thisDeck) / getEndSecond(otherDeck);
                // Re-cue the track if the user has seeked forward and will miss the fadeBeginPos
                if (otherDeck->playPosition() >= otherDeck->fadeBeginPos - toDeckFadeDistance) {
                    otherDeck->setPlayPosition(otherDeck->startPos);
                }

                if (m_crossfaderStartCenter) {
                    setCrossfader(0.0);
                } else if (thisDeck->fadeBeginPos >= thisDeck->fadeEndPos) {
                    setCrossfader(thisDeck->isLeft() ? 1.0 : -1.0);
                }

                beginSmartTransition(thisDeck, otherDeck);
                if (!otherDeckPlaying) {
                    otherDeck->play();
                }
                afterToDeckStarted();

                // Now that we have started the other deck playing, remove the track
                // that was "on deck" from the top of the queue.
                // Note: This is a DB call and takes long.
                removeLoadedTrackFromTopOfQueue(*otherDeck);
            } else {
                if (kLogger.debugEnabled()) {
                    kLogger.debug() << "playerPositionChanged"
                                    << pAttributes->group << thisPlayPosition
                                    << "but not playing";
                }
            }
        }

        double crossfaderTarget;
        if (m_eState == ADJ_LEFT_FADING) {
            crossfaderTarget = 1.0;

        } else if (m_eState == ADJ_RIGHT_FADING) {
            crossfaderTarget = -1.0;
        } else {
            // this happens if the not playing track is cued into the outro region,
            // calculated for the swapped roles.
            return;
        }

        double currentCrossfader = getCrossfader();

        if (currentCrossfader == crossfaderTarget) {
            // We are done, the fading (from) track is silenced.
            // We don't handle mode switches here since that's handled by
            // the next playerPositionChanged call otherDeck (see the
            // P1/P2FADING case above).
            thisDeck->stop();
            m_transitionProgress = 1.0;
            updateSmartTransition(1.0);
            // Note: If the user has stopped the toDeck during the transition.
            // this deck just stops as well. In this case a stopped AutoDJ is accepted
            // because the use did it intentionally
        } else {
            // We are in Fading state.
            // Calculate the current transitionProgress, the place between begin
            // and end position and the step we have taken since the last call
            double transitionProgress = (thisPlayPosition - thisDeck->fadeBeginPos) /
                    (thisDeck->fadeEndPos - thisDeck->fadeBeginPos);
            double transitionStep = transitionProgress - m_transitionProgress;
            if (transitionStep > 0.0) {
                // We have made progress.
                // Backward seeks pause the transitions; forward seeks speed up
                // the transitions. If there has been a seek beyond endPos, end
                // the transition immediately.
                double remainingCrossfader = crossfaderTarget - currentCrossfader;
                double adjustment = remainingCrossfader /
                        (1.0 - m_transitionProgress) * transitionStep;
                // we move the crossfader linearly with
                // movements in this track's play position.
                setCrossfader(currentCrossfader + adjustment);
                updateSmartTransition(transitionProgress);
            }
            m_transitionProgress = transitionProgress;
            // if we are at 1.0 here, we need an additional callback until the last
            // step is processed and we can stop the deck.
        }
    } else if (kLogger.debugEnabled() &&
            (thisPlayPosition >= thisDeck->fadeBeginPos || fromDeckAtFadeOrEnd)) {
        kLogger.debug() << "playerPositionChanged skip fade" << thisDeck->group
                        << "pos" << thisPlayPosition
                        << "fadeBegin" << thisDeck->fadeBeginPos
                        << "isFromDeck" << thisDeck->isFromDeck
                        << "otherLoading" << otherDeck->loading
                        << "playing" << thisDeckPlaying
                        << "state" << autoDJStateName(m_eState);
    }
}

TrackPointer AutoDJProcessor::getNextTrackFromQueue() {
    // Get the track at the top of the playlist.
    bool randomQueueEnabled = m_pConfig->getValue<bool>(
            ConfigKey(kPreferenceGroup, QStringLiteral("EnableRandomQueue")));
    int minAutoDJCrateTracks =
            m_pConfig->getValueString(ConfigKey(kPreferenceGroup,
                                              QStringLiteral("RandomQueueMinimumAllowed")))
                    .toInt();
    int tracksToAdd = minAutoDJCrateTracks - m_pAutoDJTableModel->rowCount();
    // In case we start off with < minimum tracks
    if (randomQueueEnabled && (tracksToAdd > 0)) {
        emit randomTrackRequested(tracksToAdd);
    }

    while (true) {
        TrackPointer pNextTrack = m_pAutoDJTableModel->getTrack(
                m_pAutoDJTableModel->index(0, 0));

        if (pNextTrack) {
            if (pNextTrack->getFileInfo().checkFileExists()) {
                return pNextTrack;
            } else {
                // Remove missing track from auto DJ playlist.
                qWarning() << "Auto DJ: Skip missing track" << pNextTrack->getLocation();
                m_pAutoDJTableModel->removeTrack(
                        m_pAutoDJTableModel->index(0, 0));
                // Don't "Requeue" missing tracks to avoid andless loops
                maybeFillRandomTracks();
            }
        } else {
            // We're out of tracks. Return the null TrackPointer.
            return pNextTrack;
        }
    }
}

bool AutoDJProcessor::loadNextTrackFromQueue(const DeckAttributes& deck, bool play) {
    TrackPointer nextTrack = getNextTrackFromQueue();

    // We ran out of tracks in the queue.
    if (!nextTrack) {
        // Disable AutoDJ.
        toggleAutoDJ(false);

        // And eject track (nextTrack is null) as "End of auto DJ warning"
        emitLoadTrackToPlayer(nextTrack, deck.group, false);
        return false;
    }

    emitLoadTrackToPlayer(nextTrack, deck.group, play);
    return true;
}

bool AutoDJProcessor::removeLoadedTrackFromTopOfQueue(const DeckAttributes& deck) {
    return removeTrackFromTopOfQueue(deck.getLoadedTrack());
}

bool AutoDJProcessor::removeTrackFromTopOfQueue(TrackPointer pTrack) {
    // No track to test for.
    if (!pTrack) {
        return false;
    }

    TrackId trackId(pTrack->getId());

    // Loaded track is not a library track.
    if (!trackId.isValid()) {
        return false;
    }

    // Get the track id at the top of the playlist.
    TrackId nextId(m_pAutoDJTableModel->getTrackId(
            m_pAutoDJTableModel->index(0, 0)));

    // No track at the top of the queue.
    if (!nextId.isValid()) {
        return false;
    }

    // If the loaded track is not the next track in the queue then do nothing.
    if (trackId != nextId) {
        return false;
    }

    // Remove the top track.
    m_pAutoDJTableModel->removeTrack(m_pAutoDJTableModel->index(0, 0));

    // Re-queue if configured.
    if (m_pConfig->getValueString(ConfigKey(kPreferenceGroup, QStringLiteral("Requeue"))).toInt()) {
        m_pAutoDJTableModel->appendTrack(nextId);
    }

    maybeFillRandomTracks();
    return true;
}

void AutoDJProcessor::maybeFillRandomTracks() {
    int minAutoDJCrateTracks =
            m_pConfig->getValueString(ConfigKey(kPreferenceGroup,
                                              QStringLiteral("RandomQueueMinimumAllowed")))
                    .toInt();
    bool randomQueueEnabled =
            m_pConfig->getValueString(
                             ConfigKey(kPreferenceGroup,
                                     QStringLiteral("EnableRandomQueue")))
                    .toInt() == 1;

    int tracksToAdd = minAutoDJCrateTracks - m_pAutoDJTableModel->rowCount();
    if (randomQueueEnabled && (tracksToAdd > 0)) {
        qDebug() << "Randomly adding tracks";
        emit randomTrackRequested(tracksToAdd);
    }
}

void AutoDJProcessor::playerPlayChanged(DeckAttributes* thisDeck, bool playing) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerPlayChanged" << thisDeck->group << playing;
    }

    if (m_eState != ADJ_IDLE) {
        // We don't want to recalculate a running transition
        return;
    }

    if (thisDeck->loading) {
        // Note: When loading a new deck this signal arrives before the
        // playerTrackLoaded();
        return;
    }

    DeckAttributes* otherDeck = getOtherDeck(thisDeck);
    if (!otherDeck) {
        // This happens if all decks have center orientation
        return;
    }

    if (playing) {
        if (!otherDeck->isPlaying()) {
            // In case both decks were stopped and now this one just started, make
            // this deck the "from deck".
            calculateTransition(thisDeck, getOtherDeck(thisDeck), false);
        }
    } else {
        // Deck paused
        if (fromDeckReachedFadeOrEnd(
                    thisDeck, thisDeck->playPosition(), getEndSecond(thisDeck))) {
            // Engine may stop play at EOF before playposition reaches fadeBeginPos
            // or 1.0. Start the same IDLE->FADING path used when position crosses
            // the threshold. Pass at least fadeBeginPos so the fade gate matches.
            if (kLogger.debugEnabled()) {
                kLogger.debug() << "playerPlayChanged" << thisDeck->group
                                << "from-deck stopped at fade/EOF, start transition"
                                << "pos" << thisDeck->playPosition()
                                << "fadeBegin" << thisDeck->fadeBeginPos;
            }
            playerPositionChanged(thisDeck,
                    math_max(thisDeck->playPosition(), thisDeck->fadeBeginPos));
            return;
        }
        // This may happen if the user has previously pressed play on the "to deck"
        // before fading, for example to adjust the intro/outro cues, and lets the
        // deck play until the end, seek back to the start point instead of keeping
        if (thisDeck->playPosition() >= 1.0 && !thisDeck->isFromDeck) {
            // toDeck has stopped at the end. Recalculate the transition, because
            // it has been done from a now irrelevant previous position.
            // This forces the other deck to be the fromDeck.
            thisDeck->startPos = kKeepPosition;
            calculateTransition(otherDeck, thisDeck, true);
            if (thisDeck->startPos != kKeepPosition) {
                // Note: this seek will trigger the playerPositionChanged slot
                // which may calls the calculateTransition() again without seek = true;
                thisDeck->setPlayPosition(thisDeck->startPos);
            }
        }
    }
}

void AutoDJProcessor::playerIntroStartChanged(DeckAttributes* pAttributes, double position) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerIntroStartChanged" << pAttributes->group << position;
    }
    // nothing to do, because we want not to re-cue the toDeck and the from
    // Deck has already passed the intro
}

void AutoDJProcessor::playerIntroEndChanged(DeckAttributes* pAttributes, double position) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerIntroEndChanged" << pAttributes->group << position;
    }

    if (m_eState != ADJ_IDLE) {
        // We don't want to recalculate a running transition
        return;
    }

    if (pAttributes->isFromDeck) {
        // We have already passed the intro
        return;
    }
    DeckAttributes* fromDeck = getFromDeck();
    if (!fromDeck) {
        return;
    }
    calculateTransition(fromDeck, getOtherDeck(fromDeck), false);
}

void AutoDJProcessor::playerOutroStartChanged(DeckAttributes* pAttributes, double position) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerOutroStartChanged" << pAttributes->group << position;
    }

    if (m_eState != ADJ_IDLE) {
        // We don't want to recalculate a running transition
        return;
    }

    DeckAttributes* fromDeck = getFromDeck();
    if (!fromDeck) {
        return;
    }
    calculateTransition(fromDeck, getOtherDeck(fromDeck), false);
}

void AutoDJProcessor::playerOutroEndChanged(DeckAttributes* pAttributes, double position) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerOutroEndChanged" << pAttributes->group << position;
    }

    if (m_eState != ADJ_IDLE) {
        // We don't want to recalculate a running transition
        return;
    }

    DeckAttributes* fromDeck = getFromDeck();
    if (!fromDeck) {
        return;
    }
    calculateTransition(fromDeck, getOtherDeck(fromDeck), false);
}

double AutoDJProcessor::getIntroStartSecond(DeckAttributes* pDeck) {
    const mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
    const mixxx::audio::FramePos introStartPosition = pDeck->introStartPosition();
    const mixxx::audio::FramePos introEndPosition = pDeck->introEndPosition();
    if (!introStartPosition.isValid() || introStartPosition > trackEndPosition) {
        double firstSoundSecond = getFirstSoundSecond(pDeck);
        if (!introEndPosition.isValid() || introEndPosition > trackEndPosition) {
            // No intro start and intro end set, use First Sound.
            return firstSoundSecond;
        }
        double introEndSecond = framePositionToSeconds(introEndPosition, pDeck);
        if (m_transitionTime >= 0) {
            return introEndSecond - m_transitionTime;
        }
        return introEndSecond;
    }
    return framePositionToSeconds(introStartPosition, pDeck);
}

double AutoDJProcessor::getIntroEndSecond(DeckAttributes* pDeck) {
    const mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
    const mixxx::audio::FramePos introEndPosition = pDeck->introEndPosition();
    if (!introEndPosition.isValid() || introEndPosition > trackEndPosition) {
        // Assume a zero length intro if introEnd is not set.
        // The introStart is automatically placed by AnalyzerSilence, so use
        // that as a fallback if the user has not placed outroStart. If it has
        // not been placed, getIntroStartPosition will return 0:00.
        return getIntroStartSecond(pDeck);
    }
    return framePositionToSeconds(introEndPosition, pDeck);
}

double AutoDJProcessor::getOutroStartSecond(DeckAttributes* pDeck) {
    const mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
    const mixxx::audio::FramePos outroStartPosition = pDeck->outroStartPosition();
    if (!outroStartPosition.isValid() || outroStartPosition > trackEndPosition) {
        // Assume a zero length outro if outroStart is not set.
        // The outroEnd is automatically placed by AnalyzerSilence, so use
        // that as a fallback if the user has not placed outroStart. If it has
        // not been placed, getOutroEndPosition will return the end of the track.
        return getOutroEndSecond(pDeck);
    }
    return framePositionToSeconds(outroStartPosition, pDeck);
}

double AutoDJProcessor::getOutroEndSecond(DeckAttributes* pDeck) {
    const mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
    const mixxx::audio::FramePos outroStartPosition = pDeck->outroStartPosition();
    const mixxx::audio::FramePos outroEndPosition = pDeck->outroEndPosition();
    if (!outroEndPosition.isValid() || outroEndPosition > trackEndPosition) {
        double lastSoundSecond = getLastSoundSecond(pDeck);
        DEBUG_ASSERT(lastSoundSecond <= framePositionToSeconds(trackEndPosition, pDeck));
        if (!outroStartPosition.isValid() || outroStartPosition > trackEndPosition) {
            // No outro start and outro end set, use Last Sound.
            return lastSoundSecond;
        }
        // Try to find a better Outro End using Outro Start and transition time
        double outroStartSecond = framePositionToSeconds(outroStartPosition, pDeck);
        if (m_transitionTime >= 0 && lastSoundSecond > outroStartSecond) {
            double outroEndFromTime = outroStartSecond + m_transitionTime;
            if (outroEndFromTime < lastSoundSecond) {
                // The outroEnd is automatically placed by AnalyzerSilence at the last sound
                // Here the user has removed it, but has placed a outro start.
                // Use the transition time instead of the dismissed last sound position.
                return outroEndFromTime;
            }
            return lastSoundSecond;
        }
        return outroStartSecond;
    }
    return framePositionToSeconds(outroEndPosition, pDeck);
}

double AutoDJProcessor::getFirstSoundSecond(DeckAttributes* pDeck) {
    TrackPointer pTrack = pDeck->getLoadedTrack();
    if (!pTrack) {
        return 0.0;
    }

    CuePointer pFromTrackN60dBSound = pTrack->findCueByType(mixxx::CueType::N60dBSound);
    if (pFromTrackN60dBSound) {
        const mixxx::audio::FramePos firstSound = pFromTrackN60dBSound->getPosition();
        if (firstSound.isValid()) {
            const mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
            if (firstSound <= trackEndPosition) {
                return framePositionToSeconds(firstSound, pDeck);
            } else {
                qWarning() << "-60 dB Sound Cue starts after track end in:"
                           << pTrack->getLocation()
                           << "Using the first sample instead.";
            }
        }
    }
    return 0.0;
}

double AutoDJProcessor::getLastSoundSecond(DeckAttributes* pDeck) {
    TrackPointer pTrack = pDeck->getLoadedTrack();
    if (!pTrack) {
        return 0.0;
    }

    const mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
    CuePointer pFromTrackN60dBSound = pTrack->findCueByType(mixxx::CueType::N60dBSound);
    if (pFromTrackN60dBSound && pFromTrackN60dBSound->getLengthFrames() > 0.0) {
        const mixxx::audio::FramePos lastSound = pFromTrackN60dBSound->getEndPosition();
        if (lastSound > mixxx::audio::FramePos(0.0)) {
            if (lastSound <= trackEndPosition) {
                return framePositionToSeconds(lastSound, pDeck);
            } else {
                qWarning() << "-60 dB Sound Cue ends after track end in:"
                           << pTrack->getLocation()
                           << "Using the last sample instead.";
            }
        }
    }
    return framePositionToSeconds(trackEndPosition, pDeck);
}

double AutoDJProcessor::getEndSecond(DeckAttributes* pDeck) {
    TrackPointer pTrack = pDeck->getLoadedTrack();
    if (!pTrack) {
        return 0.0;
    }

    mixxx::audio::FramePos trackEndPosition = pDeck->trackEndPosition();
    return framePositionToSeconds(trackEndPosition, pDeck);
}

double AutoDJProcessor::framePositionToSeconds(
        mixxx::audio::FramePos position, DeckAttributes* pDeck) {
    mixxx::audio::SampleRate sampleRate = pDeck->sampleRate();
    if (!sampleRate.isValid() || !position.isValid()) {
        return 0.0;
    }

    return position.value() / sampleRate / pDeck->rateRatio();
}

void AutoDJProcessor::calculateTransition(DeckAttributes* pFromDeck,
        DeckAttributes* pToDeck,
        bool seekToStartPoint) {
    VERIFY_OR_DEBUG_ASSERT(pFromDeck && pToDeck) {
        return;
    }
    if (pFromDeck->loading || pToDeck->loading) {
        // don't use halve new halve old data during
        // changing of tracks
        return;
    }

    // We require ADJ_IDLE to prevent changing the thresholds in the middle of a
    // fade.
    VERIFY_OR_DEBUG_ASSERT(m_eState == ADJ_IDLE) {
        return;
    }

    const double fromDeckEndPosition = getEndSecond(pFromDeck);
    const double toDeckEndPosition = getEndSecond(pToDeck);
    // Since the end position is measured in seconds from 0:00 it is also
    // the track duration. Use this alias for better readability.
    const double fromDeckDuration = fromDeckEndPosition;
    const double toDeckDuration = toDeckEndPosition;

    VERIFY_OR_DEBUG_ASSERT(fromDeckDuration >= kMinimumTrackDurationSec) {
        // Track has no duration or too short. This should not happen, because short
        // tracks are skipped after load. Play ToDeck immediately.
        pFromDeck->fadeBeginPos = 0;
        pFromDeck->fadeEndPos = 0;
        pToDeck->startPos = kKeepPosition;
        return;
    }
    if (toDeckDuration == 0) {
        // This is a seek call to zero after ejecting the track
        // this signal is received before the track pointer becomes null
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(toDeckDuration >= kMinimumTrackDurationSec) {
        // Track has no duration or too short. This should not happen, because short
        // tracks are skipped after load.
        loadNextTrackFromQueue(*pToDeck, false);
        return;
    }

    // Within this function, the outro refers to the outro of the currently
    // playing track and the intro refers to the intro of the next track.

    double outroEnd = getOutroEndSecond(pFromDeck);
    double outroStart = getOutroStartSecond(pFromDeck);
    const double fromDeckPosition = fromDeckDuration * pFromDeck->playPosition();

    VERIFY_OR_DEBUG_ASSERT(outroEnd <= fromDeckEndPosition) {
        outroEnd = fromDeckEndPosition;
    }

    if (fromDeckPosition > outroStart) {
        // We have already passed outroStart
        // This can happen if we have just enabled auto DJ
        outroStart = fromDeckPosition;
        if (fromDeckPosition > outroEnd) {
            outroEnd = math_min(outroStart + fabs(m_transitionTime), fromDeckEndPosition);
        }
    }
    double outroLength = outroEnd - outroStart;

    double toDeckPositionSeconds = toDeckDuration * pToDeck->playPosition();
    // Store here a possible fadeBeginPos for the transition after next
    // This is used to check if it will be possible or a re-cue is required.
    // here it is done for FullIntroOutro and FadeAtOutroStart.
    // It is adjusted below for the other modes.
    pToDeck->fadeEndPos = getOutroEndSecond(pToDeck);
    double toDeckOutroStartSecond = getOutroStartSecond(pToDeck);
    if (pToDeck->fadeEndPos == toDeckOutroStartSecond) {
        // outro not defined, use transition time.
        toDeckOutroStartSecond -= m_transitionTime;
    }
    pToDeck->fadeBeginPos = toDeckOutroStartSecond;

    double toDeckStartSeconds = toDeckPositionSeconds;
    const double introStart = getIntroStartSecond(pToDeck);
    const double introEnd = getIntroEndSecond(pToDeck);
    if (seekToStartPoint || toDeckPositionSeconds >= pToDeck->fadeBeginPos) {
        // toDeckPosition >= pToDeck->fadeBeginPos happens when the
        // user has seeked or played the to track behind fadeBeginPos of
        // the fade after the next.
        // In this case we recue the track just before the transition.
        toDeckStartSeconds = introStart;
    }

    double introLength = 0;

    // introEnd is equal introStart in case it has not yet been set
    if (toDeckStartSeconds < introEnd && introStart < introEnd) {
        // Limit the intro length that results from a revers seek
        // to a reasonable values. If the seek was too big, ignore it.
        introLength = introEnd - toDeckStartSeconds;
        if (introLength > (introEnd - introStart) * 2 &&
                introLength > (introEnd - introStart) + m_transitionTime &&
                introLength > outroLength) {
            introLength = 0;
        }
    }

    if (kLogger.debugEnabled()) {
        kLogger.debug() << "calculateTransition"
                        << "introLength" << introLength
                        << "outroLength" << outroLength;
    }

    m_crossfaderStartCenter = false;
    switch (m_transitionMode) {
    case TransitionMode::FullIntroOutro: {
        // Use the outro or intro length for the transition time, whichever is
        // shorter. Let the full outro and intro play; do not cut off any part
        // of either.
        //
        // In the diagrams below,
        // - is part of a track outside the outro/intro,
        // o is part of the outro
        // i is part of the intro
        // | marks the boundaries of the transition
        //
        // When outro > intro:
        // ------ooo|ooo|
        //          |iii|------
        //
        // When outro < intro:
        // ------|ooo|
        //       |iii|iii-----
        //
        // If only the outro or intro length is marked but not both, use the one
        // that is marked for the transition time. If neither is marked, fall
        // back to the transition time from the spinbox.
        double transitionLength = introLength;
        if (outroLength > 0) {
            if (transitionLength <= 0 || transitionLength > outroLength) {
                // Use outro length when the intro is not defined or longer
                // than the outro.
                transitionLength = outroLength;
            }
        }
        if (transitionLength > 0) {
            const double transitionEnd = toDeckStartSeconds + transitionLength;
            if (transitionEnd > pToDeck->fadeBeginPos) {
                // End intro before next outro starts
                transitionLength = pToDeck->fadeBeginPos - toDeckStartSeconds;
                VERIFY_OR_DEBUG_ASSERT(transitionLength > 0) {
                    // We seek to intro start above in this case so this never happens
                    transitionLength = 1;
                }
            }
            pFromDeck->fadeBeginPos = outroEnd - transitionLength;
            pFromDeck->fadeEndPos = outroEnd;
            pToDeck->startPos = toDeckStartSeconds;
        } else {
            useFixedFadeTime(pFromDeck, pToDeck, fromDeckPosition, outroEnd, toDeckStartSeconds);
        }
    } break;
    case TransitionMode::FadeAtOutroStart: {
        // Use the outro or intro length for the transition time, whichever is
        // shorter. If the outro is longer than the intro, cut off the end
        // of the outro.
        //
        // In the diagrams below,
        // - is part of a track outside the outro/intro,
        // o is part of the outro
        // i is part of the intro
        // | marks the boundaries of the transition
        //
        // When outro > intro:
        // ------|ooo|ooo
        //       |iii|------
        //
        // When outro < intro:
        // ------|ooo|
        //       |iii|iii-----
        //
        // If only the outro or intro length is marked but not both, use the one
        // that is marked for the transition time. If neither is marked, fall
        // back to the transition time from the spinbox.
        double transitionLength = outroLength;
        if (transitionLength > 0) {
            if (introLength > 0) {
                if (outroLength > introLength) {
                    // Cut off end of outro
                    transitionLength = introLength;
                }
            }
            const double transitionEnd = toDeckStartSeconds + transitionLength;
            if (transitionEnd > pToDeck->fadeBeginPos) {
                // End intro before next outro starts
                transitionLength = pToDeck->fadeBeginPos - toDeckStartSeconds;
                VERIFY_OR_DEBUG_ASSERT(transitionLength > 0) {
                    // We seek to intro start above in this case so this never happens
                    transitionLength = 1;
                }
            }
            pFromDeck->fadeBeginPos = outroStart;
            pFromDeck->fadeEndPos = outroStart + transitionLength;
            pToDeck->startPos = toDeckStartSeconds;
        } else if (introLength > 0) {
            transitionLength = introLength;
            pFromDeck->fadeBeginPos = outroEnd - transitionLength;
            pFromDeck->fadeEndPos = outroEnd;
            pToDeck->startPos = toDeckStartSeconds;
        } else {
            useFixedFadeTime(pFromDeck, pToDeck, fromDeckPosition, outroEnd, toDeckStartSeconds);
        }
    } break;
    case TransitionMode::FixedStartCenterSkipSilence:
        m_crossfaderStartCenter = true;
        // fall through intended!
        [[fallthrough]];
    case TransitionMode::FixedSkipSilence: {
        double toDeckStartSecond;
        pToDeck->fadeBeginPos = getLastSoundSecond(pToDeck);
        if (seekToStartPoint || toDeckPositionSeconds >= pToDeck->fadeBeginPos) {
            // toDeckPosition >= pToDeck->fadeBeginPos happens when the
            // user has seeked or played the to track behind fadeBeginPos of
            // the fade after the next.
            // In this case we recue the track just before the transition.
            toDeckStartSecond = getFirstSoundSecond(pToDeck);
        } else {
            toDeckStartSecond = toDeckPositionSeconds;
        }
        useFixedFadeTime(
                pFromDeck,
                pToDeck,
                fromDeckPosition,
                getLastSoundSecond(pFromDeck),
                toDeckStartSecond);
    } break;
    case TransitionMode::FixedFullTrack:
    default: {
        double startPoint;
        pToDeck->fadeBeginPos = toDeckEndPosition;
        if (seekToStartPoint || toDeckPositionSeconds >= pToDeck->fadeBeginPos) {
            // toDeckPosition >= pToDeck->fadeBeginPos happens when the
            // user has seeked or played the to track behind fadeBeginPos of
            // the fade after the next.
            // In this case we recue the track just before the transition.
            startPoint = 0.0;
        } else {
            startPoint = toDeckPositionSeconds;
        }
        useFixedFadeTime(pFromDeck, pToDeck, fromDeckPosition, fromDeckEndPosition, startPoint);
        }
    }

    // Auto DJ 2.0 Phase 2: put a beatmatched fade on phrase boundaries.
    alignTransitionToPhrases(pFromDeck, pToDeck, fromDeckPosition);

    // These are expected to be a fraction of the track length.
    pFromDeck->fadeBeginPos /= fromDeckDuration;
    pFromDeck->fadeEndPos /= fromDeckDuration;
    pToDeck->startPos /= toDeckDuration;
    pToDeck->fadeBeginPos /= toDeckDuration;
    pToDeck->fadeEndPos /= toDeckDuration;

    pFromDeck->isFromDeck = true;
    pToDeck->isFromDeck = false;

    VERIFY_OR_DEBUG_ASSERT(pFromDeck->fadeBeginPos <= 1) {
        pFromDeck->fadeBeginPos = 1;
    }

    if (kLogger.debugEnabled()) {
        kLogger.debug() << "calculateTransition" << pFromDeck->group
                        << pFromDeck->fadeBeginPos << pFromDeck->fadeEndPos
                        << pToDeck->startPos;
    }
}

void AutoDJProcessor::useFixedFadeTime(
        DeckAttributes* pFromDeck,
        DeckAttributes* pToDeck,
        double fromDeckSecond,
        double fadeEndSecond,
        double toDeckStartSecond) {
    if (m_transitionTime > 0.0) {
        // Guard against the next track being too short. This transition must finish
        // before the next transition starts.
        double toDeckOutroStart = pToDeck->fadeBeginPos;
        if (pToDeck->fadeBeginPos >= pToDeck->fadeEndPos) {
            // no outro defined, the toDeck will also use the transition time
            toDeckOutroStart -= m_transitionTime;
        }
        if (toDeckOutroStart <= toDeckStartSecond + kMinimumTrackDurationSec) {
            // we have already passed the outro start
            // Check OutroEnd as alternative, which is for all transition mode
            // better than directly defaulting to duration()
            double end = getOutroEndSecond(pToDeck);
            if (end <= toDeckStartSecond + kMinimumTrackDurationSec) {
                // we have also passed the outro end
                end = getEndSecond(pToDeck);
                VERIFY_OR_DEBUG_ASSERT(end > toDeckStartSecond + kMinimumTrackDurationSec) {
                    // as last resort move start point
                    // The caller makes sure that this never happens
                    toDeckStartSecond = end - kMinimumTrackDurationSec;
                }
            }
            // use the remaining time for fading
            toDeckOutroStart = (end - toDeckStartSecond) / 2 + toDeckStartSecond;
        }
        double transitionTime = math_min(toDeckOutroStart - toDeckStartSecond,
                m_transitionTime);
        VERIFY_OR_DEBUG_ASSERT(transitionTime >= kMinimumTrackDurationSec / 2) {
            transitionTime = kMinimumTrackDurationSec / 2;
        }
        // Note: pFromDeck->fadeBeginPos >= pFromDeck->fadeEndPos is handled in
        // playerPositionChanged() causing a jump cut.
        pFromDeck->fadeBeginPos = math_max(fadeEndSecond - transitionTime, fromDeckSecond);
        pFromDeck->fadeEndPos = fadeEndSecond;
        pToDeck->startPos = toDeckStartSecond;
    } else {
        pFromDeck->fadeBeginPos = fadeEndSecond;
        pFromDeck->fadeEndPos = fadeEndSecond;
        pToDeck->startPos = toDeckStartSecond + m_transitionTime;
    }
}

void AutoDJProcessor::playerTrackLoaded(DeckAttributes* pDeck, TrackPointer pTrack) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerTrackLoaded" << pDeck->group
                        << (pTrack ? pTrack->getLocation() : "(null)");
    }

    pDeck->loading = false;

    // Since the end position is measured in seconds from 0:00 it is also
    // the track duration.
    double duration = getEndSecond(pDeck);
    if (duration < kMinimumTrackDurationSec) {
        qWarning() << "Skip track with" << duration << "Duration"
                   << pTrack->getLocation();
        // Remove Track with duration smaller than two callbacks
        removeTrackFromTopOfQueue(pTrack);

        // Load the next track. If we are the first AutoDJ track
        // (ADJ_ENABLE_P1LOADED state) then play the track.
        loadNextTrackFromQueue(*pDeck, m_eState == ADJ_ENABLE_P1LOADED);
    } else if (m_eState == ADJ_IDLE) {
        // this deck has just changed the track so it becomes the toDeck
        DeckAttributes* fromDeck = getOtherDeck(pDeck);
        // check if this deck has suitable alignment
        if (fromDeck && getOtherDeck(fromDeck) != pDeck) {
            if (kLogger.debugEnabled()) {
                kLogger.debug() << "playerTrackLoaded()" << pDeck->group << "but not a toDeck";
            }
            // User has changed the orientation, disable Auto DJ
            toggleAutoDJ(false);
            emit autoDJError(ADJ_NOT_TWO_DECKS);
            return;
        }
        pDeck->startPos = kKeepPosition;
        calculateTransition(fromDeck, pDeck, true);
        if (pDeck->startPos != kKeepPosition) {
            // Note: this seek will trigger the playerPositionChanged slot
            // which may call the calculateTransition() again without seek = true;
            pDeck->setPlayPosition(pDeck->startPos);
        }
        // we are here in the relative domain 0..1
        if (!fromDeck->isPlaying() && fromDeck->playPosition() >= 1.0) {
            // repeat a probably missed update
            playerPositionChanged(fromDeck, 1.0);
        }
    } else if (m_eState == ADJ_LEFT_FADING) {
        if (pDeck == getRightDeck()) {
            // restore the play state lost during loading
            pDeck->play();
        }
    } else if (m_eState == ADJ_RIGHT_FADING) {
        if (pDeck == getLeftDeck()) {
            // restore the play state lost during loading
            pDeck->play();
        }
    }
}

void AutoDJProcessor::playerLoadingTrack(DeckAttributes* pDeck,
        TrackPointer pNewTrack, TrackPointer pOldTrack) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerLoadingTrack" << pDeck->group
                        << "new:" << (pNewTrack ? pNewTrack->getLocation() : "(null)")
                        << "old:" << (pOldTrack ? pOldTrack->getLocation() : "(null)");
    }

    pDeck->loading = true;

    // The Deck is loading an new track

    // There are four conditions under which we load a track.
    // 1) We are enabling AutoDJ and no decks are playing. Mode is
    //    ADJ_ENABLE_P1LOADED.
    // 2) After #1, we load a track into the other deck. Mode is ADJ_IDLE.
    // 3) We are enabling AutoDJ and a single deck is playing. Mode is ADJ_IDLE.
    // 4) We have just completed fading from one deck to another. Mode is
    //    ADJ_IDLE.

    if (!pNewTrack) {
        // If a track is ejected because of a manual eject command or a load failure
        // this track seems to be undesired. Remove the bad track from the queue.
        removeTrackFromTopOfQueue(pOldTrack);

        // Wait until the track is fully unloaded and the playerEmpty()
        // slot is called before loading an alternative track.
    }
}

void AutoDJProcessor::playerEmpty(DeckAttributes* pDeck) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerEmpty()" << pDeck->group;
    }

    // The Deck has ejected a track and no new one is loaded.
    // This happens if loading fails or the user manually ejected the track
    // and would normally stop the AutoDJ flow, which is not desired.
    // It should be safe to load a new track from the queue. The only case where
    // we request a load-and-play is case #1 currently so we can easily test for
    // this based on the mode.

    // Load the next track. If we are the first AutoDJ track
    // (ADJ_ENABLE_P1LOADED state) then play the track.
    loadNextTrackFromQueue(*pDeck, m_eState == ADJ_ENABLE_P1LOADED);
}

void AutoDJProcessor::playerRateChanged(DeckAttributes* pAttributes) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerRateChanged" << pAttributes->group;
    }

    if (m_eState != ADJ_IDLE) {
        // We don't want to recalculate a running transition
        return;
    }

    DeckAttributes* fromDeck = getFromDeck();
    if (!fromDeck) {
        return;
    }
    calculateTransition(fromDeck, getOtherDeck(fromDeck), false);
}

void AutoDJProcessor::playerOrientationChanged(DeckAttributes* pAttributes) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playerOrientationChanged" << pAttributes->group;
    }

    if (m_eState != ADJ_DISABLED) {
        // Disable auto DJ and emit the error explaining that we no longer have two valid decks.
        toggleAutoDJ(false);
        emit autoDJError(ADJ_NOT_TWO_DECKS);
    }
}

void AutoDJProcessor::playlistFirstTrackChanged() {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "playlistFirstTrackChanged";
    }
    if (m_eState != ADJ_DISABLED) {
        DeckAttributes* pLeftDeck = getLeftDeck();
        DeckAttributes* pRightDeck = getRightDeck();

        if (!pLeftDeck->isPlaying()) {
            loadNextTrackFromQueue(*pLeftDeck);
        } else if (!pRightDeck->isPlaying()) {
            loadNextTrackFromQueue(*pRightDeck);
        }
    }
}

void AutoDJProcessor::setTransitionTime(int time) {
    if (kLogger.debugEnabled()) {
        kLogger.debug() << "setTransitionTime" << time;
    }

    // Update the transition time first.
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kTransitionPreferenceName),
            time);
    m_transitionTime = time;

    // Then re-calculate fade thresholds for the decks.
    if (m_eState == ADJ_IDLE) {
        DeckAttributes* pLeftDeck = getLeftDeck();
        DeckAttributes* pRightDeck = getRightDeck();
        if (!pLeftDeck || !pRightDeck) {
            // User has changed the orientation, disable Auto DJ
            toggleAutoDJ(false);
            emit autoDJError(ADJ_NOT_TWO_DECKS);
            return;
        }
        if (pLeftDeck->isPlaying()) {
            calculateTransition(pLeftDeck, pRightDeck, false);
        }
        if (pRightDeck->isPlaying()) {
            calculateTransition(pRightDeck, pLeftDeck, false);
        }
    }
}

void AutoDJProcessor::setTransitionMode(TransitionMode newMode) {
    m_pConfig->set(ConfigKey(kPreferenceGroup, kTransitionModePreferenceName),
            ConfigValue(static_cast<int>(newMode)));
    m_transitionMode = newMode;

    if (m_eState != ADJ_IDLE) {
        // We don't want to recalculate a running transition
        return;
    }

    // Then re-calculate fade thresholds for the decks.
    DeckAttributes* pLeftDeck = getLeftDeck();
    DeckAttributes* pRightDeck = getRightDeck();

    if (!pLeftDeck || !pRightDeck) {
        // User has changed the orientation, disable Auto DJ
        toggleAutoDJ(false);
        emit autoDJError(ADJ_NOT_TWO_DECKS);
        return;
    }

    if (pLeftDeck->isPlaying() && !pRightDeck->isPlaying()) {
        calculateTransition(pLeftDeck, pRightDeck, true);
        if (pRightDeck->startPos != kKeepPosition) {
            // Note: this seek will trigger the playerPositionChanged slot
            // which may calls the calculateTransition() again without seek = true;
            pRightDeck->setPlayPosition(pRightDeck->startPos);
        }
    } else if (pRightDeck->isPlaying() && pLeftDeck->isPlaying()) {
        calculateTransition(pRightDeck, pLeftDeck, true);
        if (pLeftDeck->startPos != kKeepPosition) {
            // Note: this seek will trigger the playerPositionChanged slot
            // which may calls the calculateTransition() again without seek = true;
            pLeftDeck->setPlayPosition(pLeftDeck->startPos);
        }
    } else {
        // user has manually started the other deck or stopped both.
        // don't know what to do.
    }
}

DeckAttributes* AutoDJProcessor::getLeftDeck() {
    // find first left deck
    for (const auto& pDeck : m_decks) {
        if (pDeck->isLeft()) {
            return pDeck.get();
        }
    }
    return nullptr;
}

DeckAttributes* AutoDJProcessor::getRightDeck() {
    // find first right deck
    for (const auto& pDeck : m_decks) {
        if (pDeck->isRight()) {
            return pDeck.get();
        }
    }
    return nullptr;
}

DeckAttributes* AutoDJProcessor::getOtherDeck(
        const DeckAttributes* pThisDeck) {
    if (pThisDeck->isLeft()) {
        return getRightDeck();
    }
    if (pThisDeck->isRight()) {
        return getLeftDeck();
    }
    return nullptr;
}

DeckAttributes* AutoDJProcessor::getFromDeck() {
    for (const auto& pDeck : m_decks) {
        if (pDeck->isFromDeck) {
            return pDeck.get();
        }
    }
    return nullptr;
}

bool AutoDJProcessor::nextTrackLoaded() {
    if (m_eState == ADJ_DISABLED) {
        // AutoDJ always loads the top track (again) if enabled
        return false;
    }

    DeckAttributes* pLeftDeck = getLeftDeck();
    DeckAttributes* pRightDeck = getRightDeck();
    if (!pLeftDeck || !pRightDeck) {
        return false;
    }

    bool leftDeckPlaying = pLeftDeck->isPlaying();
    bool rightDeckPlaying = pRightDeck->isPlaying();

    // Calculate idle deck
    TrackPointer loadedTrack;
    if (leftDeckPlaying && !rightDeckPlaying) {
        loadedTrack = pRightDeck->getLoadedTrack();
    } else if (!leftDeckPlaying && rightDeckPlaying) {
        loadedTrack = pLeftDeck->getLoadedTrack();
    } else if (getCrossfader() < 0.0) {
        loadedTrack = pRightDeck->getLoadedTrack();
    } else {
        loadedTrack = pLeftDeck->getLoadedTrack();
    }

    return loadedTrack == getNextTrackFromQueue();
}
