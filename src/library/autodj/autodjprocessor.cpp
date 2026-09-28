#include "library/autodj/autodjprocessor.h"

#include <QFutureWatcher>
#include <QHash>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "analyzer/analyzerenergy.h"
#include "engine/channels/enginedeck.h"
#include "control/controlobject.h"
#include "library/autodj/smart/beatmatch.h"
#include "library/autodj/smart/downbeat.h"
#include "library/autodj/smart/vocalmap.h"
#include "waveform/waveform.h"
#include "library/autodj/smart/bridgefinder.h"
#include "library/autodj/smart/phrasealign.h"
#include "library/autodj/smart/energystore.h"
#include "library/autodj/smart/mixscorer.h"
#include "library/autodj/smart/smartsequencer.h"
#include "library/autodj/smart/trackfeatures.h"
#include "library/columncache.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
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

// Auto DJ 2.0 plus Video Mixing, phase 2 helpers. Controls may be missing (e.g. in tests or
// when the EQ rack is not loaded), so every access checks first.
const QString kBeatmatchPreference = QStringLiteral("SmartBeatmatch");
const QString kKeyMorphPreference = QStringLiteral("SmartKeyMorph");
constexpr int kKeyMorphDefault = 1; // semitones
constexpr int kKeyMorphMax = 2;
constexpr double kBeatmatchTolerancePct = 5.0; // the DJ's 5% rule
// Safety net for an outgoing track without a beat grid: a short switch of
// this length at its end, instead of Mixxx's long default fade (two beats
// that do not match, for many seconds).
constexpr double kNoGridSwitchSec = 4.0;
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();

ConfigKey eqKillKey(const QString& deckGroup) {
    // parameter1 = low band; button_parameter1 = its kill switch.
    return ConfigKey(QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup),
            QStringLiteral("button_parameter1"));
}

// Mid (2) and high (3) EQ gain of a deck: 1.0 = unchanged.
ConfigKey eqGainKey(const QString& deckGroup, int band) {
    return ConfigKey(QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup),
            QStringLiteral("parameter%1").arg(band));
}

double readControl(const ConfigKey& key) {
    return ControlObject::exists(key) ? ControlObject::get(key) : kMissing;
}

void writeControl(const ConfigKey& key, double value) {
    if (!std::isnan(value) && ControlObject::exists(key)) {
        ControlObject::set(key, value);
    }
}

// Auto DJ 2.0 plus Video Mixing: stem mixes. The deck's stem controls
// (src/stems/stemcontrols.*) set the part volumes of a song that plays from
// its stem file; [ChannelN],stem_ready says whether it does.
// Auto DJ > Stems: 0 = original mix (never the parts), 1 = stem mix,
// 2 = stem mix with singing detection (default). Older settings only had
// the on/off switch AutoDJStems.
constexpr int kStemMixOriginal = 0;
constexpr int kStemMixSinging = 2;
int stemMixMode(const UserSettingsPointer& pConfig) {
    const int mode = pConfig->getValue(
            ConfigKey(QStringLiteral("[Stems]"), QStringLiteral("AutoDJStemMode")), -1);
    if (mode >= kStemMixOriginal && mode <= kStemMixSinging) {
        return mode;
    }
    return pConfig->getValue(ConfigKey(QStringLiteral("[Stems]"), QStringLiteral("AutoDJStems")), true)
            ? kStemMixSinging
            : kStemMixOriginal;
}

bool stemMixesEnabled(const UserSettingsPointer& pConfig) {
    return stemMixMode(pConfig) != kStemMixOriginal;
}

// Singing detection: where the song sings, from its waveform with parts
// (made from its stem file; parts in the order drums, bass, other,
// vocals). Nothing when that waveform is not there (yet).
std::optional<std::vector<vocalmap::Section>> singingOf(const TrackPointer& pTrack) {
    if (!pTrack || pTrack->getStemInfo().size() < 4) {
        return std::nullopt;
    }
    const ConstWaveformPointer pWaveform = pTrack->getWaveform();
    const double seconds = pTrack->getDuration();
    if (!pWaveform || !pWaveform->hasStem() || !(seconds > 0.0)) {
        return std::nullopt;
    }
    const int frames = pWaveform->getDataSize() / 2; // left and right
    if (frames < 100) {
        return std::nullopt;
    }
    const WaveformData* pData = pWaveform->data();
    std::vector<float> vocal(frames);
    std::vector<float> rest(frames);
    for (int f = 0; f < frames; ++f) {
        const WaveformData& l = pData[2 * f];
        const WaveformData& r = pData[2 * f + 1];
        vocal[f] = std::max(l.stems[3], r.stems[3]) / 255.0f;
        float band = 0.0f;
        for (int s = 0; s < 3; ++s) {
            band = std::max(band, std::max(l.stems[s], r.stems[s]) / 255.0f);
        }
        rest[f] = band;
    }
    return vocalmap::find(vocal, rest, frames / seconds);
}

/// Where the kick starts after the track's beat lines, in seconds at its
/// own speed: from its drum part, or the low band of songs without parts.
/// 0 when unclear.
double kickOffsetOf(const TrackPointer& pTrack, const phrasealign::Grid& grid) {
    if (!pTrack || !grid.isValid()) {
        return 0.0;
    }
    const ConstWaveformPointer pWaveform = pTrack->getWaveform();
    const double seconds = pTrack->getDuration();
    if (!pWaveform || !(seconds > 0.0)) {
        return 0.0;
    }
    const int frames = pWaveform->getDataSize() / 2; // left and right
    if (frames < 100) {
        return 0.0;
    }
    const bool drums = pWaveform->hasStem() && pTrack->getStemInfo().size() >= 4;
    const WaveformData* pData = pWaveform->data();
    std::vector<float> level(frames);
    for (int f = 0; f < frames; ++f) {
        const WaveformData& l = pData[2 * f];
        const WaveformData& r = pData[2 * f + 1];
        level[f] = (drums ? std::max(l.stems[0], r.stems[0])
                          : std::max(l.filtered.low, r.filtered.low)) /
                255.0f;
    }
    std::vector<double> beats;
    for (int n = 0; n < 20000; ++n) {
        const double t = grid.beatTime(n);
        if (t > seconds) {
            break;
        }
        if (t >= 0.0) {
            beats.push_back(t);
        }
    }
    return beatmatch::kickOffset(level, frames / seconds, beats).value_or(0.0);
}

QString minutesText(double seconds) {
    const int s = static_cast<int>(std::lround(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

bool stemsReady(const QString& deckGroup) {
    return readControl(ConfigKey(deckGroup, QStringLiteral("stem_ready"))) > 0.5;
}

double readStem(const QString& deckGroup, const char* pItem, double fallback) {
    const double value = readControl(ConfigKey(deckGroup, QString::fromLatin1(pItem)));
    return std::isnan(value) ? fallback : value;
}

void writeStem(const QString& deckGroup, const char* pItem, double value) {
    writeControl(ConfigKey(deckGroup, QString::fromLatin1(pItem)), value);
}

// ECHO OUT on the outgoing vocals (a push button: press and release).
void stemEchoOut(const QString& deckGroup) {
    writeStem(deckGroup, "stem_echo_out", 1.0);
    writeStem(deckGroup, "stem_echo_out", 0.0);
}

// Auto DJ 2.0 plus Video Mixing: every library track with a known key and BPM whose file
// still exists, as bridge candidates. Read straight from the database so
// no Track objects are loaded for the whole library.
// The beat grid checks, for marking tracks Auto DJ cannot beatmatch.
struct GridCheckRow {
    double bpm = 0.0;
    double driftBeats = -1.0;
};
QHash<TrackId, GridCheckRow> loadGridChecks(const QSqlDatabase& db) {
    QHash<TrackId, GridCheckRow> rows;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT track_id, bpm, drift_beats FROM autodj_grid_check "
            "WHERE version IN (:version, :mapVersion, :oldVersion, :oldMapVersion)"));
    query.bindValue(QStringLiteral(":version"), EnergyCalculator::kGridCheckVersion);
    query.bindValue(QStringLiteral(":mapVersion"), EnergyCalculator::kGridCheckMapVersion);
    query.bindValue(QStringLiteral(":oldVersion"), EnergyCalculator::kGridCheckOldVersion);
    query.bindValue(QStringLiteral(":oldMapVersion"), EnergyCalculator::kGridCheckOldMapVersion);
    if (query.exec()) {
        while (query.next()) {
            GridCheckRow row;
            row.bpm = query.value(1).toDouble();
            row.driftBeats = query.value(2).isNull() ? -1.0 : query.value(2).toDouble();
            rows.insert(TrackId(query.value(0)), row);
        }
    }
    return rows;
}

// A grid check only counts for the grid it was made on (same tempo).
void markGrid(TrackFeatures* pTrack, const QHash<TrackId, GridCheckRow>& checks) {
    const auto it = checks.constFind(pTrack->id);
    pTrack->gridUnsteady = it != checks.constEnd() &&
            std::fabs(it->bpm - pTrack->bpm) < 0.01 &&
            it->driftBeats > EnergyCalculator::kGridMaxDriftBeats;
}

QVector<TrackFeatures> loadBridgeCandidates(const QSqlDatabase& db) {
    QVector<TrackFeatures> candidates;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
                "SELECT library.id, library.bpm, library.key_id, "
                "library.artist, library.title, library.genre, track_locations.location "
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
        f.artist = artist;
        f.title = title;
        f.genre = query.value(5).toString().trimmed();
        f.isVideo = TrackFeatures::isVideoFile(query.value(6).toString());
        candidates.append(f);
        ids.append(f.id);
    }
    const QHash<TrackId, EnergyStore::Value> energies = EnergyStore::loadEnergies(db, ids);
    const QHash<TrackId, GridCheckRow> gridChecks = loadGridChecks(db);
    for (TrackFeatures& f : candidates) {
        const EnergyStore::Value value = energies.value(f.id);
        f.energy = value.energy;
        f.energyIsManual = value.manual;
        markGrid(&f, gridChecks);
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
    m_glideTicker.setInterval(100);
    connect(&m_glideTicker, &QTimer::timeout, this, [this]() {
        updateGlide(m_glide.pDeck);
    });
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
        // And which tracks cannot be beatmatched (beat grid check).
        const QHash<TrackId, GridCheckRow> gridChecks =
                loadGridChecks(m_pTrackCollectionManager->internalCollection()->database());
        for (Row& row : rows) {
            const EnergyStore::Value value = energies.value(row.id);
            row.features.energy = value.energy;
            row.features.energyIsManual = value.manual;
            markGrid(&row.features, gridChecks);
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

int AutoDJProcessor::keyMorphLimit() const {
    return std::clamp(m_pConfig->getValue(ConfigKey(kPreferenceGroup, kKeyMorphPreference),
                              kKeyMorphDefault),
            0,
            kKeyMorphMax);
}

bool AutoDJProcessor::beatEntryAtStart() const {
    return m_pConfig->getValue(ConfigKey(kPreferenceGroup, QStringLiteral("BeatEntryAtStart")),
            true);
}

void AutoDJProcessor::setBeatEntryAtStart(bool atStart) {
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, QStringLiteral("BeatEntryAtStart")), atStart);
    kLogger.info() << "The new song's beat comes in at the"
                   << (atStart ? "start" : "middle") << "of the mix";
}

void AutoDJProcessor::setKeyMorphLimit(int semitones) {
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kKeyMorphPreference),
            std::clamp(semitones, 0, kKeyMorphMax));
}

AutoDJProcessor::LiveSuggestions AutoDJProcessor::liveSuggestions(int count) {
    LiveSuggestions result;
    const int deckIndex = PlayerInfo::instance().getCurrentPlayingDeck();
    const TrackPointer pTrack = PlayerInfo::instance().getCurrentPlayingTrack();
    if (deckIndex < 0 || !pTrack || !m_pTrackCollectionManager ||
            !m_pTrackCollectionManager->internalCollection()) {
        return result;
    }
    result.deckGroup = PlayerManager::groupForDeck(deckIndex);
    TrackFeatures now = TrackFeatures::fromTrack(pTrack);
    // What is heard right now: the deck's tempo (tempo fader included) and
    // key (key lock, pitch and key morph included).
    const double liveBpm = readControl(ConfigKey(result.deckGroup, QStringLiteral("bpm")));
    if (liveBpm > 0.0) {
        now.bpm = liveBpm;
    }
    const double liveKey = readControl(ConfigKey(result.deckGroup, QStringLiteral("key")));
    if (!std::isnan(liveKey)) {
        const auto key = KeyUtils::keyFromNumericValue(liveKey);
        if (key != mixxx::track::io::key::INVALID &&
                mixxx::track::io::key::ChromaticKey_IsValid(key)) {
            now.camelotNumber =
                    TrackFeatures::camelotFromOpenKey(KeyUtils::keyToOpenKeyNumber(key));
            now.camelotMinor = !KeyUtils::keyIsMajor(key);
        }
    }
    const QSqlDatabase db = m_pTrackCollectionManager->internalCollection()->database();
    const QHash<TrackId, EnergyStore::Value> energies = EnergyStore::loadEnergies(db, {now.id});
    if (energies.contains(now.id)) {
        now.energy = energies.value(now.id).energy;
        now.energyIsManual = energies.value(now.id).manual;
    }
    result.now = now;

    // Never suggest a track that is loaded, or was played this session.
    QSet<TrackId> excludeIds;
    QSet<QString> excludeNames;
    for (const auto& pDeck : m_decks) {
        if (const TrackPointer pLoaded = pDeck->getLoadedTrack()) {
            excludeIds.insert(pLoaded->getId());
            excludeNames.insert(BridgeFinder::nameKey(TrackFeatures::fromTrack(pLoaded)));
        }
    }
    // Played this session: heard live (our own list), or marked played in
    // the database.
    excludeIds.unite(m_playedLive);
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("SELECT id FROM library WHERE played = 1"))) {
        while (query.next()) {
            excludeIds.insert(TrackId(query.value(0)));
        }
    }
    const QVector<TrackFeatures> library = loadBridgeCandidates(db);
    // Nor another version or a cover of a song that was played.
    for (const TrackFeatures& t : library) {
        if (excludeIds.contains(t.id) && !t.displayName.isEmpty()) {
            excludeNames.insert(BridgeFinder::nameKey(t));
        }
    }
    // The same energy direction and artist rule as Smart Fill.
    const int energyChoice = m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, QStringLiteral("SmartFillEnergy")), 0);
    MixScoreWeights weights;
    weights.direction = energyChoice == 1
            ? MixScoreWeights::EnergyDirection::Hold
            : (energyChoice == 2 ? MixScoreWeights::EnergyDirection::Wave
                                 : MixScoreWeights::EnergyDirection::Build);
    const bool avoidArtist = m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, QStringLiteral("SmartFillAvoidSameArtist")), true);
    result.next = BridgeFinder(MixScorer(weights))
                          .suggestNext(now,
                                  library,
                                  excludeIds,
                                  excludeNames,
                                  count,
                                  avoidArtist);
    return result;
}

void AutoDJProcessor::notePlayedLive(const TrackPointer& pTrack) {
    if (pTrack && pTrack->getId().isValid()) {
        m_playedLive.insert(pTrack->getId());
    }
}

bool AutoDJProcessor::loadOnFreeDeck(TrackId trackId, QString* pMessage) {
    if (m_eState != ADJ_DISABLED) {
        *pMessage = tr("Auto DJ is on. Turn it off to load songs by hand.");
        return false;
    }
    const int liveIndex = PlayerInfo::instance().getCurrentPlayingDeck();
    const QString liveGroup = liveIndex >= 0 ? PlayerManager::groupForDeck(liveIndex) : QString();
    DeckAttributes* pFree = nullptr;
    for (DeckAttributes* pDeck : {getLeftDeck(), getRightDeck()}) {
        if (pDeck && pDeck->group != liveGroup && !pDeck->isPlaying()) {
            pFree = pDeck;
            break;
        }
    }
    if (!pFree) {
        *pMessage = tr("No free deck: both decks are playing.");
        return false;
    }
    const TrackPointer pTrack = m_pTrackCollectionManager
            ? m_pTrackCollectionManager->getTrackById(trackId)
            : TrackPointer();
    if (!pTrack) {
        *pMessage = tr("Could not open that track.");
        return false;
    }
    emitLoadTrackToPlayer(pTrack, pFree->group, false);
    kLogger.info() << "Live Assistant: loaded" << pTrack->getInfo() << "on" << pFree->group;
    *pMessage = tr("Loaded on %1: %2").arg(pFree->group, pTrack->getInfo());
    return true;
}

int AutoDJProcessor::currentKeyShift(DeckAttributes* pDeck) const {
    if (!pDeck || !m_keyShift.contains(pDeck->group)) {
        return 0;
    }
    const TrackPointer pTrack = pDeck->getLoadedTrack();
    const KeyShift shift = m_keyShift.value(pDeck->group);
    return pTrack && pTrack->getId() == shift.trackId ? shift.semitones : 0;
}

namespace {
// Where a deck is, in seconds of its track at its own speed (-1 = unknown).
double trackSecond(const DeckAttributes* pDeck) {
    const mixxx::audio::SampleRate sampleRate = pDeck->sampleRate();
    const mixxx::audio::FramePos end = pDeck->trackEndPosition();
    if (!sampleRate.isValid() || !end.isValid()) {
        return -1.0;
    }
    return pDeck->playPosition() * end.value() / sampleRate;
}
} // namespace

void AutoDJProcessor::beginSmartTransition(
        DeckAttributes* pFromDeck, DeckAttributes* pToDeck) {
    m_fadeNowLimit = FadeNowLimit(); // the mix has started
    endSmartTransition(false); // safety: never two at once
    // If the outgoing track is still gliding back (a short track), hold its
    // tempo steady during this mix; it is reset once it has faded out.
    m_glide.pDeck = nullptr;
    if (!isBeatmatchEnabled() || !pFromDeck || !pToDeck) {
        // No beatmatch: the outgoing vocals leave with an echo, which
        // covers the change.
        if (pFromDeck && pToDeck && stemMixesEnabled(m_pConfig) &&
                stemsReady(pFromDeck->group) && outgoingSingsInMix(pFromDeck)) {
            kLogger.info() << "Stem mix" << pFromDeck->group << ": echo out of the vocals";
            stemEchoOut(pFromDeck->group);
        }
        return;
    }
    m_smart = SmartTransition();
    m_smart.active = true;
    m_smart.pFrom = pFromDeck;
    m_smart.pTo = pToDeck;
    m_smart.fromLowKill = readControl(eqKillKey(pFromDeck->group));
    m_smart.toLowKill = readControl(eqKillKey(pToDeck->group));
    m_smart.fromMid = readControl(eqGainKey(pFromDeck->group, 2));
    m_smart.fromHigh = readControl(eqGainKey(pFromDeck->group, 3));
    m_smart.toMid = readControl(eqGainKey(pToDeck->group, 2));
    m_smart.toHigh = readControl(eqGainKey(pToDeck->group, 3));

    // Tempo: play the incoming track at the outgoing track's tempo, but only
    // within the 5% rule; otherwise it stays a plain crossfade.
    const TrackPointer pToTrack = pToDeck->getLoadedTrack();
    const ConfigKey fromBpmKey(pFromDeck->group, QStringLiteral("bpm"));
    const double fromBpm = readControl(fromBpmKey); // includes its tempo change
    // The incoming tempo where it starts: with a beat map that bends, this
    // is not the same as its average BPM.
    const phrasealign::Grid toGrid = gridFor(pToTrack);
    const double toStartSec = trackSecond(pToDeck);
    const double toTrackBpm = toGrid.isValid() && toStartSec >= 0.0
            ? 60.0 / toGrid.steadyBeatSecAt(toStartSec)
            : (pToTrack ? pToTrack->getBpm() : 0.0);
    std::optional<double> ratio;
    if (!std::isnan(fromBpm)) {
        ratio = beatmatch::matchRatio(fromBpm, toTrackBpm, kBeatmatchTolerancePct);
    }
    // A beat grid that drifts off the beat would make a messy beatmatch:
    // then a plain fade instead (the same decision as the phrase plan).
    QString gridWhy;
    const bool gridsOk = gridsAllowBeatmatch(pFromDeck->getLoadedTrack(), pToTrack, &gridWhy);
    // The phrase plan already decided whether this mix is beatmatched, and
    // placed the incoming song for it (its beat in the middle of the fade,
    // or at its end for a switch). Follow that decision, so the plan and the
    // mix never disagree near the 5% limit.
    const TrackPointer pFromTrackNow = pFromDeck->getLoadedTrack();
    const bool plannedSwitch = m_plannedMix.valid && !m_plannedMix.matched && pFromTrackNow &&
            pToTrack && m_plannedMix.fromId == pFromTrackNow->getId() &&
            m_plannedMix.toId == pToTrack->getId();
    if (plannedSwitch && ratio) {
        // The plan was made earlier, from tempos measured then. If the two
        // songs are now clearly within the 5% rule (under 4%, so the plan
        // and the mix do not flip-flop at the limit), beatmatch after all.
        if (gridsOk && std::fabs(*ratio - 1.0) < 0.04) {
            kLogger.info() << "Beatmatch after all" << pToDeck->group
                           << ": the plan chose a switch, but the tempos are now"
                           << toTrackBpm << "vs" << fromBpm << "BPM";
        } else {
            ratio.reset();
        }
    }
    if (!gridsOk) {
        ratio.reset();
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
        // Beat lock: a beat map means the tempo bends (a live drummer), so
        // one fixed speed would slowly drift off the beat. Only 1:1 (not
        // half or double time).
        m_smart.fromGrid = gridFor(pFromDeck->getLoadedTrack());
        m_smart.toGrid = toGrid;
        const bool oneToOne = fromBpm > 0.0 &&
                std::fabs(*ratio * toTrackBpm / fromBpm - 1.0) < 0.1;
        m_smart.beatLock = oneToOne && m_smart.fromGrid.isValid() && m_smart.toGrid.isValid() &&
                (m_smart.fromGrid.isMap() || m_smart.toGrid.isMap());
        // Kick line-up: beat analysis puts some songs' lines at the start
        // of the kick and others 30-40 ms before it. Lock the kicks.
        if (oneToOne && m_smart.fromGrid.isValid() && m_smart.toGrid.isValid()) {
            const double fromSecNow = trackSecond(pFromDeck);
            const double fromKick = kickOffsetOf(pFromDeck->getLoadedTrack(), m_smart.fromGrid);
            const double toKick = kickOffsetOf(pToTrack, m_smart.toGrid);
            auto inBeats = [](double sec, double beatSec) {
                const double beats = beatSec > 0.0 ? sec / beatSec : 0.0;
                return std::fabs(beats) <= 0.15 ? beats : 0.0; // more: not a kick
            };
            const double fromBeats = inBeats(fromKick,
                    m_smart.fromGrid.beatSecAt(std::max(0.0, fromSecNow)));
            const double toBeats = inBeats(toKick,
                    m_smart.toGrid.beatSecAt(std::max(0.0, toStartSec)));
            m_smart.kickShiftBeats = toBeats - fromBeats;
            kLogger.info() << "Kick line-up: outgoing kick" << std::lround(fromKick * 1000.0)
                           << "ms after its beat lines, incoming"
                           << std::lround(toKick * 1000.0) << "ms: incoming lines"
                           << m_smart.kickShiftBeats << "beats ahead";
            if (std::fabs(m_smart.kickShiftBeats) > beatmatch::kLockDeadBeats) {
                m_smart.beatLock = true;
            }
        }
        if (m_smart.beatLock) {
            kLogger.info() << "Beat lock" << pToDeck->group
                           << ": a beat map bends the tempo"
                           << (m_smart.fromGrid.isMap() ? "(outgoing)" : "")
                           << (m_smart.toGrid.isMap() ? "(incoming)" : "")
                           << ", the incoming speed follows the outgoing beats";
        }
        // Key morph: if the keys clash, pitch the incoming track a little
        // (key lock is on, so its tempo is not touched) so the two fit. It
        // keeps that key to the end of the track: gliding the pitch back
        // would be heard as the song going out of tune.
        const int limit = keyMorphLimit();
        m_keyShift.remove(pToDeck->group);
        if (limit > 0) {
            // The outgoing track plays in its own key plus its own morph.
            const TrackFeatures from = MixScorer::shiftKey(
                    TrackFeatures::fromTrack(pFromDeck->getLoadedTrack()),
                    currentKeyShift(pFromDeck));
            const TrackFeatures to = TrackFeatures::fromTrack(pToTrack);
            const int shift = MixScorer::keyMorphSemitones(from, to, limit);
            if (shift != 0) {
                m_smart.toKeyShift = shift;
                m_keyShift.insert(pToDeck->group, KeyShift{pToTrack->getId(), shift});
                kLogger.info() << "Key morph" << pToDeck->group << to.camelotText()
                               << "->" << MixScorer::shiftKey(to, shift).camelotText()
                               << "(" << shift << "semitone ) to fit" << from.camelotText();
            } else if (from.hasKey() && to.hasKey() &&
                    MixScorer::camelotCost(from, to, 0.0, false) >= MixScorer::kClashKeyCost) {
                kLogger.info() << "Key morph: no shift of up to" << limit << "semitone fits"
                               << to.camelotText() << "to" << from.camelotText();
            }
        }
    } else if (!gridsOk) {
        kLogger.info() << "No beatmatch for" << pToDeck->group << ":" << gridWhy
                       << ": plain crossfade";
    } else if (plannedSwitch) {
        kLogger.info() << "No beatmatch for" << pToDeck->group
                       << ": the phrase plan chose a switch (" << toTrackBpm << "vs" << fromBpm
                       << "BPM now)";
    } else {
        kLogger.info() << "No beatmatch for" << pToDeck->group
                       << "(" << toTrackBpm << "vs" << fromBpm
                       << "BPM): plain crossfade";
    }
    // Stem mix: when both songs have their parts, the parts cross over
    // instead of the EQ. Not beatmatched: the outgoing vocals echo out.
    if (stemMixesEnabled(m_pConfig)) {
        const bool fromParts = stemsReady(pFromDeck->group);
        const bool toParts = stemsReady(pToDeck->group);
        if (m_smart.beatmatched && fromParts && toParts) {
            m_smart.stems = true;
            auto save = [](const QString& g) {
                SmartTransition::StemLevels l;
                l.vocals = readStem(g, "stem_vocals", 1.0);
                l.instrumental = readStem(g, "stem_instrumental", 1.0);
                l.drums = readStem(g, "stem_drums", 1.0);
                l.bass = readStem(g, "stem_bass", 1.0);
                l.vocalsKill = readStem(g, "stem_vocals_kill", 0.0);
                l.instrumentalKill = readStem(g, "stem_instrumental_kill", 0.0);
                l.drumsKill = readStem(g, "stem_drums_kill", 0.0);
                return l;
            };
            m_smart.fromStems = save(pFromDeck->group);
            m_smart.toStems = save(pToDeck->group);
            // During the mix the knobs carry the levels (kills off).
            for (const QString& g : {pFromDeck->group, pToDeck->group}) {
                writeStem(g, "stem_vocals_kill", 0.0);
                writeStem(g, "stem_instrumental_kill", 0.0);
                writeStem(g, "stem_drums_kill", 0.0);
            }
            m_smart.fadeDrums = m_pConfig->getValue(
                    ConfigKey(QStringLiteral("[Stems]"), QStringLiteral("AutoDJFadeDrums")), true);
            kLogger.info() << "Stem mix" << pFromDeck->group << "->" << pToDeck->group
                           << (m_smart.fadeDrums
                                              ? ": instrumental first, drums fade across, bass "
                                                "swaps in the middle, vocals never together"
                                              : ": instrumental first, drums + bass swap in "
                                                "the middle, vocals never together");
            if (stemMixMode(m_pConfig) == kStemMixSinging) {
                planVocals(pFromDeck, pToDeck);
            }
        } else if (!m_smart.beatmatched && fromParts && !outgoingSingsInMix(pFromDeck)) {
            kLogger.info() << "Stem mix" << pFromDeck->group
                           << ": no singing at the end, no echo out needed";
        } else if (!m_smart.beatmatched && fromParts) {
            kLogger.info() << "Stem mix" << pFromDeck->group << ": echo out of the vocals";
            stemEchoOut(pFromDeck->group);
        } else if (m_smart.beatmatched && (fromParts || toParts)) {
            kLogger.info() << "No stem mix: only" << (fromParts ? "the outgoing" : "the incoming")
                           << "song has its parts (EQ mix)";
        }
    }
    updateSmartTransition(0.0); // incoming bass starts cut
}

bool AutoDJProcessor::outgoingSingsInMix(DeckAttributes* pFromDeck) {
    // Only with singing detection and a known vocal map; otherwise assume
    // the song sings (the safe choice: its vocals are handled).
    if (stemMixMode(m_pConfig) != kStemMixSinging || !pFromDeck) {
        return true;
    }
    const TrackPointer pTrack = pFromDeck->getLoadedTrack();
    const auto sections = singingOf(pTrack);
    if (!sections) {
        return true;
    }
    const double seconds = pTrack->getDuration();
    return vocalmap::singsBetween(*sections,
            pFromDeck->fadeBeginPos * seconds,
            pFromDeck->fadeEndPos * seconds);
}

void AutoDJProcessor::planVocals(DeckAttributes* pFromDeck, DeckAttributes* pToDeck) {
    const TrackPointer pFrom = pFromDeck->getLoadedTrack();
    const TrackPointer pTo = pToDeck->getLoadedTrack();
    const auto fromSinging = singingOf(pFrom);
    const auto toSinging = singingOf(pTo);
    if (!fromSinging || !toSinging) {
        kLogger.info() << "Singing detection: no vocal map yet for"
                       << (!fromSinging ? pFromDeck->group : pToDeck->group)
                       << "(its waveform with parts is not ready): plain stem mix";
        return;
    }
    // The mix, in each song's own seconds.
    const double fromStart = pFromDeck->fadeBeginPos * pFrom->getDuration();
    const double fromEnd = pFromDeck->fadeEndPos * pFrom->getDuration();
    const double length = std::max(0.1, fromEnd - fromStart);
    const double toStart = pToDeck->playPosition() * pTo->getDuration();
    const double toEnd = toStart + length;

    beatmatch::VocalPlan plan;
    plan.fromSings = vocalmap::singsBetween(*fromSinging, fromStart, fromEnd);
    plan.toSings = vocalmap::singsBetween(*toSinging, toStart, toEnd);
    plan.swapAt = 0.5;
    QString swapText;
    if (plan.fromSings && plan.toSings) {
        // Hand the vocals over when the outgoing singer ends a line.
        const double quiet = vocalmap::quietMoment(
                *fromSinging, fromStart + 0.3 * length, fromEnd - 0.15 * length);
        if (quiet >= 0.0) {
            plan.swapAt = (quiet - fromStart) / length;
            swapText = QStringLiteral(", vocals handed over at %1 (end of a line)")
                               .arg(minutesText(quiet));
        } else {
            swapText = QStringLiteral(", no gap in the singing: vocals handed over halfway");
        }
    }
    m_smart.vocalPlan = plan;
    m_smart.vocalPlanned = true;
    const double toFirst = vocalmap::nextSinging(*toSinging, toStart);
    kLogger.info().noquote()
            << "Singing detection:" << pFromDeck->group
            << (plan.fromSings ? "sings" : "does not sing") << "during the mix,"
            << pToDeck->group << (plan.toSings ? "sings" : "does not sing")
            << (toFirst >= 0.0 ? QStringLiteral("(its singing starts at %1)").arg(minutesText(toFirst))
                               : QStringLiteral("(no singing ahead)"))
            << swapText;
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
    m_smart.progress = progress;
    followBeats();
    if (m_smart.stems) {
        // The parts cross over (the EQ stays as the DJ set it).
        const beatmatch::StemBlend b = m_smart.vocalPlanned
                ? beatmatch::stemBlend(progress, m_smart.vocalPlan, m_smart.fadeDrums)
                : beatmatch::stemBlend(progress, m_smart.fadeDrums);
        auto level = [](double knob, double kill) {
            return kill > 0.5 ? 0.0 : knob;
        };
        const auto& f = m_smart.fromStems;
        const auto& t = m_smart.toStems;
        const QString& from = m_smart.pFrom->group;
        const QString& to = m_smart.pTo->group;
        writeStem(from, "stem_vocals", level(f.vocals, f.vocalsKill) * b.fromVocals);
        writeStem(from, "stem_instrumental", level(f.instrumental, f.instrumentalKill) * b.fromInstrumental);
        writeStem(from, "stem_drums", level(f.drums, f.drumsKill) * b.fromDrums);
        writeStem(from, "stem_bass", f.bass * b.fromBass);
        writeStem(to, "stem_vocals", level(t.vocals, t.vocalsKill) * b.toVocals);
        writeStem(to, "stem_instrumental", level(t.instrumental, t.instrumentalKill) * b.toInstrumental);
        writeStem(to, "stem_drums", level(t.drums, t.drumsKill) * b.toDrums);
        // The bass (inside the instrumental) swaps with the drums.
        writeStem(to, "stem_bass", t.bass * b.toBass);
    } else {
        const beatmatch::BassState bass = beatmatch::bassSwap(progress);
        if (!std::isnan(m_smart.fromLowKill)) {
            writeControl(eqKillKey(m_smart.pFrom->group), bass.fromLowKilled ? 1.0 : 0.0);
        }
        if (!std::isnan(m_smart.toLowKill)) {
            writeControl(eqKillKey(m_smart.pTo->group), bass.toLowKilled ? 1.0 : 0.0);
        }
    }
    // Key morph. Written on every update, because switching key lock on
    // can reset the pitch in the engine a moment after we set it.
    if (m_smart.toKeyShift != 0) {
        const ConfigKey pitchKey(m_smart.pTo->group, QStringLiteral("pitch_adjust"));
        const double now = readControl(pitchKey);
        if (!std::isnan(now) && std::fabs(now - m_smart.toKeyShift) > 0.01) {
            writeControl(pitchKey, m_smart.toKeyShift);
        }
    }
    // Full EQ transition: the mids and highs cross over gradually, relative
    // to where the DJ had them (a missing control stays NaN = untouched).
    if (m_smart.stems) {
        return; // no EQ blend on top of a stem mix
    }
    const beatmatch::EqBlend eq = beatmatch::eqBlend(progress);
    writeControl(eqGainKey(m_smart.pFrom->group, 2), m_smart.fromMid * eq.fromMidHigh);
    writeControl(eqGainKey(m_smart.pFrom->group, 3), m_smart.fromHigh * eq.fromMidHigh);
    writeControl(eqGainKey(m_smart.pTo->group, 2), m_smart.toMid * eq.toMidHigh);
    writeControl(eqGainKey(m_smart.pTo->group, 3), m_smart.toHigh * eq.toMidHigh);
}

void AutoDJProcessor::endSmartTransition(bool completed) {
    if (!m_smart.active) {
        return;
    }
    // Put the EQ kills back as the DJ had them.
    writeControl(eqKillKey(m_smart.pFrom->group), m_smart.fromLowKill);
    writeControl(eqKillKey(m_smart.pTo->group), m_smart.toLowKill);
    writeControl(eqGainKey(m_smart.pFrom->group, 2), m_smart.fromMid);
    writeControl(eqGainKey(m_smart.pFrom->group, 3), m_smart.fromHigh);
    writeControl(eqGainKey(m_smart.pTo->group, 2), m_smart.toMid);
    writeControl(eqGainKey(m_smart.pTo->group, 3), m_smart.toHigh);
    if (m_smart.stems) {
        // The parts back as the DJ had them.
        auto restore = [](const QString& g, const SmartTransition::StemLevels& l) {
            writeStem(g, "stem_vocals", l.vocals);
            writeStem(g, "stem_instrumental", l.instrumental);
            writeStem(g, "stem_drums", l.drums);
            writeStem(g, "stem_bass", l.bass);
            writeStem(g, "stem_vocals_kill", l.vocalsKill);
            writeStem(g, "stem_instrumental_kill", l.instrumentalKill);
            writeStem(g, "stem_drums_kill", l.drumsKill);
        };
        restore(m_smart.pFrom->group, m_smart.fromStems);
        restore(m_smart.pTo->group, m_smart.toStems);
        kLogger.info() << "Stem mix" << m_smart.pTo->group << (completed ? "done" : "stopped");
    }
    if (m_smart.beatLock) {
        kLogger.info() << "Beat lock" << m_smart.pTo->group << "done: ended at ratio"
                       << m_smart.toRatio << ", largest slip"
                       << m_smart.worstSlipBeats << "beats";
    }
    if (m_smart.beatmatched) {
        writeControl(ConfigKey(m_smart.pTo->group, QStringLiteral("quantize")),
                m_smart.toQuantize);
        if (completed) {
            // Ease the new track back to its own tempo, too slowly to hear.
            startGlide(m_smart.pTo, m_smart.toRatio);
        }
    }
    if (completed) {
        // The outgoing track has stopped: put its deck back to normal tempo.
        resetDeckTempo(m_smart.pFrom, true);
    }
    m_smart = SmartTransition();
}

void AutoDJProcessor::followBeats() {
    if (!m_smart.beatLock || !m_smart.pFrom->isPlaying() || !m_smart.pTo->isPlaying()) {
        return;
    }
    const double fromSec = trackSecond(m_smart.pFrom);
    const double toSec = trackSecond(m_smart.pTo);
    const double fromRatio = m_smart.pFrom->rateRatio();
    if (fromSec < 0.0 || toSec < 0.0 || !(fromRatio > 0.0)) {
        return;
    }
    // Jump watch: each deck should have moved on by the time passed times
    // its speed. A bigger difference means something moved it.
    if (!m_smart.lockClock.isValid()) {
        m_smart.lockClock.start();
    }
    const qint64 nowMs = m_smart.lockClock.elapsed();
    bool glitch = false;
    if (m_smart.lastMs >= 0) {
        const double passed = (nowMs - m_smart.lastMs) / 1000.0;
        const double fromJump =
                (fromSec - m_smart.lastFromSec) - passed * fromRatio;
        const double toJump =
                (toSec - m_smart.lastToSec) - passed * m_smart.pTo->rateRatio();
        // A late update (the screen thread was busy) or a position that
        // jumped: the positions read now may not be from the same moment,
        // so they are not acted on for a moment (in real mixes this came
        // about 3 s into the mix and made a good lock jump out of line).
        if (passed > 0.25 || std::fabs(fromJump) > 0.04 || std::fabs(toJump) > 0.04) {
            glitch = true;
        }
        if (std::fabs(fromJump) > 0.04 || std::fabs(toJump) > 0.04) {
            kLogger.info() << "Beat lock jump watch: after" << passed * 1000.0
                           << "ms" << m_smart.pFrom->group << "moved"
                           << std::lround(fromJump * 1000.0) << "ms extra,"
                           << m_smart.pTo->group << "moved"
                           << std::lround(toJump * 1000.0) << "ms extra (at"
                           << fromSec << "s /" << toSec << "s)";
        }
    }
    m_smart.lastMs = nowMs;
    m_smart.lastFromSec = fromSec;
    m_smart.lastToSec = toSec;
    // A beat map that goes wrong where the music stops (fade-outs read as
    // 159 or 235 BPM) must not steer the lock: it pushed a real mix half a
    // beat apart. There, hold the two steady tempos together and wait.
    {
        const double fromLocal = m_smart.fromGrid.beatSecAt(fromSec);
        const double fromSteady = m_smart.fromGrid.steadyBeatSecAt(fromSec);
        const double toLocal = m_smart.toGrid.beatSecAt(toSec);
        const double toSteady = m_smart.toGrid.steadyBeatSecAt(toSec);
        const bool wrong = !(fromSteady > 0.0) || !(toSteady > 0.0) ||
                std::fabs(fromLocal / fromSteady - 1.0) > 0.08 ||
                std::fabs(toLocal / toSteady - 1.0) > 0.08;
        if (wrong) {
            if (!m_smart.lockPaused) {
                m_smart.lockPaused = true;
                kLogger.info() << "Beat lock" << m_smart.pTo->group
                               << ": paused, the beat lines go wrong here ("
                               << (fromSteady > 0.0 ? 60.0 / fromLocal : 0.0) << "BPM outgoing,"
                               << (toSteady > 0.0 ? 60.0 / toLocal : 0.0)
                               << "BPM incoming): the steady tempos are held together";
            }
            if (fromSteady > 0.0 && toSteady > 0.0) {
                const double steadyRatio = toSteady / (fromSteady / fromRatio);
                if (std::fabs(steadyRatio - 1.0) <= 0.1 &&
                        std::fabs(steadyRatio - m_smart.toRatio) > 1e-5) {
                    ControlObject::set(ConfigKey(m_smart.pTo->group, QStringLiteral("rate_ratio")),
                            steadyRatio);
                    m_smart.toRatio = steadyRatio;
                }
            }
            m_smart.lastMs = -1; // jump watch starts again afterwards
            return;
        }
        if (m_smart.lockPaused) {
            m_smart.lockPaused = false;
            kLogger.info() << "Beat lock" << m_smart.pTo->group << ": beat lines good again";
        }
    }
    // Lines: how far the two songs' beat lines are apart. Slip: how far
    // their kicks are apart (the lines plus the kick line-up).
    const double fromBeat = m_smart.fromGrid.beatAt(fromSec);
    const double toBeat = m_smart.toGrid.beatAt(toSec);
    double lineSlip = fromBeat - toBeat;
    lineSlip -= std::round(lineSlip);
    double slip = 0.0;
    const double ratio = beatmatch::followRatio(fromBeat + m_smart.kickShiftBeats,
            m_smart.fromGrid.beatSecAt(fromSec) / fromRatio,
            toBeat,
            m_smart.toGrid.beatSecAt(toSec),
            &slip);
    ++m_smart.lockUpdates;
    if (glitch) {
        m_smart.distrustUntil = m_smart.lockUpdates + 5;
        m_smart.bigSlipCount = 0;
    }
    const bool trusted = m_smart.lockUpdates > m_smart.distrustUntil;
    // The first second is the phase sync settling in; not counted.
    if (trusted && m_smart.lockUpdates > 50) {
        m_smart.worstSlipBeats = std::max(m_smart.worstSlipBeats, std::fabs(slip));
    }
    if (!trusted) {
        return; // keep the speed as it is until the readings are steady
    }
    if (std::fabs(lineSlip) > beatmatch::kResyncBeats &&
            std::fabs(slip) > beatmatch::kResyncBeats) {
        ++m_smart.bigSlipCount;
    } else {
        m_smart.bigSlipCount = 0;
    }
    // The slip once per bar in the log, so a drift can be traced afterwards.
    const int bar = static_cast<int>(std::floor(fromBeat / 4.0));
    if (bar != m_smart.loggedBar) {
        m_smart.loggedBar = bar;
        kLogger.info() << "Beat lock" << m_smart.pTo->group << "bar" << bar
                       << ": kick slip" << slip << "beats (lines" << lineSlip
                       << "), ratio" << m_smart.toRatio;
    }
    // The lines clearly off early in the mix, while the incoming song is
    // still quiet: line them up now instead of pulling back slowly (heard
    // as a clash for several beats). The engine does the jump itself, at
    // the exact moment (reading the position here and jumping from here
    // came up to one audio buffer late). The kick line-up, a few ms, is
    // then done by the speed. A few times at most, never back to back.
    // Only when it is off in 5 readings in a row: one odd reading is not
    // a reason to move the song.
    if (m_smart.bigSlipCount >= 5 && m_smart.progress < 0.3 &&
            m_smart.resyncs < 3 && m_smart.lockUpdates > 5 &&
            m_smart.lockUpdates - m_smart.lastResyncUpdate > 25) {
        const ConfigKey phaseKey(m_smart.pTo->group, QStringLiteral("beatsync_phase"));
        writeControl(phaseKey, 1.0);
        writeControl(phaseKey, 0.0);
        ++m_smart.resyncs;
        m_smart.lastResyncUpdate = m_smart.lockUpdates;
        m_smart.bigSlipCount = 0;
        kLogger.info() << "Beat lock" << m_smart.pTo->group << ": lines" << lineSlip
                       << "beats apart early in the mix, lined up again";
        return;
    }
    // Safety: never more than 10% off the track's own speed.
    if (std::fabs(ratio - 1.0) > 0.1 || std::fabs(ratio - m_smart.toRatio) < 1e-5) {
        return;
    }
    ControlObject::set(ConfigKey(m_smart.pTo->group, QStringLiteral("rate_ratio")), ratio);
    m_smart.toRatio = ratio; // the glide back starts from here
}

void AutoDJProcessor::startGlide(DeckAttributes* pDeck, double startRatio) {
    m_glide.pDeck = pDeck;
    m_glide.startRatio = startRatio;
    m_glide.lastWritten = startRatio;
    const TrackPointer pTrack = pDeck ? pDeck->getLoadedTrack() : TrackPointer();
    m_glide.trackId = pTrack ? pTrack->getId() : TrackId();
    m_glide.timer.start();
    m_glideTicker.start();
}

void AutoDJProcessor::updateGlide(DeckAttributes* pDeck) {
    if (!pDeck || pDeck != m_glide.pDeck) {
        if (!m_glide.pDeck) {
            m_glideTicker.stop();
        }
        return;
    }
    const TrackPointer pTrack = pDeck->getLoadedTrack();
    if (!pTrack || pTrack->getId() != m_glide.trackId) {
        // Another track was loaded: it keeps its own tempo.
        m_glide.pDeck = nullptr;
        m_glideTicker.stop();
        return;
    }
    const ConfigKey rateKey(pDeck->group, QStringLiteral("rate_ratio"));
    const double now = readControl(rateKey);
    if (!std::isnan(now) && std::fabs(now - m_glide.lastWritten) > 0.001) {
        // The DJ moved the tempo: their deck now, stop gliding.
        kLogger.info() << "Tempo glide on" << pDeck->group << "stopped: tempo changed by hand";
        m_glide.pDeck = nullptr;
        m_glideTicker.stop();
        return;
    }
    const double ratio = beatmatch::glideRatio(
            m_glide.startRatio, m_glide.timer.elapsed() / 1000.0);
    writeControl(rateKey, ratio);
    m_glide.lastWritten = ratio;
    if (ratio == 1.0) {
        m_glide.pDeck = nullptr;
        m_glideTicker.stop();
        resetDeckTempo(pDeck, false); // back at its own tempo: restore key lock
    }
}

void AutoDJProcessor::alignTransitionToPhrases(DeckAttributes* pFromDeck,
        DeckAttributes* pToDeck,
        double fromDeckPositionSec) {
    m_lastAlignApplied = false;
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
    const bool fromHasGrid = pFromBeats && fromBpm > 0.0 &&
            gridFor(pFromTrack).isValid();
    const bool toHasGrid = pToBeats && toBpm > 0.0 &&
            gridFor(pToTrack).isValid();
    if (!fromHasGrid && !toHasGrid) {
        // Nothing is known about either beat (e.g. a library that was never
        // analysed): Mixxx's own timing, as without Auto DJ 2.0 plus Video
        // Mixing.
        logOnce(QStringLiteral(" nogrid"),
                QStringLiteral("skipped, neither track has a beat grid"));
        return;
    }
    // Safety net: an outgoing track without a beat grid (never analysed, or a
    // broken file) next to one that has a grid cannot be put on phrases.
    // Instead of Mixxx's long default fade (two beats that do not match, for
    // many seconds), a short switch just before its outro (or its end).
    if (!fromHasGrid) {
        double endSec = getOutroEndSecond(pFromDeck);
        const mixxx::audio::FramePos outroStart = pFromDeck->outroStartPosition();
        if (outroStart.isValid() && outroStart <= pFromDeck->trackEndPosition()) {
            endSec = std::min(endSec, framePositionToSeconds(outroStart, pFromDeck));
        }
        const double beginSec = std::max(fromDeckPositionSec, endSec - kNoGridSwitchSec);
        if (!(endSec > beginSec)) {
            logOnce(QStringLiteral(" nogrid"),
                    QStringLiteral("no beat grid on the outgoing track and no time left: "
                                   "keeping the plain timing"));
            return;
        }
        const double toStartSec = std::max(0.0, getIntroStartSecond(pToDeck));
        pFromDeck->fadeBeginPos = beginSec;
        pFromDeck->fadeEndPos = endSec;
        pToDeck->startPos = toStartSec;
        m_lastAlignApplied = true;
        logOnce(QStringLiteral(" nogrid %1").arg(endSec * pFromDeck->rateRatio(), 0, 'f', 1),
                QStringLiteral("NO beat grid on the outgoing track: short switch, fade %1 -> "
                               "%2 s, incoming starts at %3 s")
                        .arg(beginSec)
                        .arg(endSec)
                        .arg(toStartSec));
        const double toDuration = getEndSecond(pToDeck);
        if (!pToDeck->isPlaying() && toDuration > 0.0) {
            const double toNowSec = pToDeck->playPosition() * toDuration;
            if (std::fabs(toNowSec - toStartSec) > 0.05) {
                pToDeck->setPlayPosition(toStartSec / toDuration);
            }
        }
        return;
    }
    // An incoming track without a grid is fine: the mix is simply not
    // beatmatched (its beat comes in as the fade ends, see planUnmatched).
    // Beatmatched only within the 5% rule. Otherwise the outgoing side is
    // still placed on its phrases and the incoming intro is still skipped,
    // but the new beat comes in as the fade ends (no clashing beats).
    const double fromRatio = pFromDeck->rateRatio();
    const double toRatio = pToDeck->rateRatio();
    // A grid that drifts off the beat counts as "not matched" too.
    QString gridWhy;
    const bool gridsOk = gridsAllowBeatmatch(pFromTrack, pToTrack, &gridWhy);
    // Seconds here are real time at each deck's current speed, the same
    // convention as the rest of calculateTransition. A beat map (the tempo
    // bends with the music) gives the time of every beat.
    const phrasealign::Grid from = gridFor(pFromTrack).atSpeed(fromRatio);
    const phrasealign::Grid to = gridFor(pToTrack).atSpeed(toRatio);
    if (!from.isValid()) {
        logOnce(QStringLiteral(" nogrid"), QStringLiteral("skipped, the outgoing track has no beat grid"));
        return;
    }

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

    // Where each track's "body" is (from the energy analysis): the fade must
    // be over before the outgoing track starts fading out on its own, and a
    // long quiet intro on the incoming track is partly skipped so its beat
    // kicks in as the fade ends.
    // Not fadeEndPos: after the first run that is our own earlier plan, and
    // a tiny tempo change (a glide) then made no phrase fit any more.
    double fromLimitSec = getOutroEndSecond(pFromDeck);
    // Fade Now pressed: the fade is over one fade length after the next
    // phrase start, so the plan starts it at that phrase.
    const bool fadeNowPending = m_fadeNowLimit.trackSec >= 0.0 &&
            m_fadeNowLimit.trackId == pFromTrack->getId();
    if (fadeNowPending) {
        fromLimitSec = std::min(fromLimitSec, m_fadeNowLimit.trackSec / fromRatio);
    }
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
    const int bars = phrasealign::barsForSeconds(wantedSec, from.steadyBeatSecAt(fromLimitSec));
    // Beatmatched only within the 5% rule, compared at the tempo each track
    // has where the mix happens (a beat map may bend away from its average).
    // An outgoing deck still easing back to its own tempo after the previous
    // mix will be there by the time this mix starts.
    const double fromMixRatio = m_glide.pDeck == pFromDeck ? 1.0 : fromRatio;
    // The steady tempo there: a beat map often goes wrong where the music
    // stops (Xymox "A Million Things": 159 and 235 BPM in its fade-out),
    // which made mixes of songs 2-4% apart quick switches.
    const double fromMixBpm =
            60.0 / from.steadyBeatSecAt(fromLimitSec) * fromMixRatio / fromRatio;
    const double toMixBpm = to.isValid()
            ? 60.0 / (to.steadyBeatSecAt(toBodyStartSec >= 0.0 ? toBodyStartSec : toEarliestSec) *
                             toRatio)
            : 0.0;
    const bool matched = gridsOk && toHasGrid && to.isValid() &&
            beatmatch::matchRatio(fromMixBpm, toMixBpm, kBeatmatchTolerancePct).has_value();
    const QString notMatchedWhy = !to.isValid()
            ? QStringLiteral("the incoming track has no beat grid")
            : (gridsOk ? QStringLiteral("%1 vs %2 BPM")
                                 .arg(fromMixBpm, 0, 'f', 1)
                                 .arg(toMixBpm, 0, 'f', 1)
                       : gridWhy);
    const auto plan = matched
            ? phrasealign::plan(from,
                      to,
                      fromDeckPositionSec,
                      fromLimitSec, // the fade must be over by here
                      toEarliestSec,
                      bars,
                      toBodyStartSec,
                      true, // body start is precise (analysis v5)
                      beatEntryAtStart() ? 0.0 : 0.5)
            : phrasealign::planUnmatched(from,
                      to,
                      fromDeckPositionSec,
                      fromLimitSec,
                      toEarliestSec,
                      bars,
                      toBodyStartSec,
                      true); // body start is precise (analysis v5)
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
            QString(matched ? QString()
                            : QStringLiteral("NOT beatmatched (%1), new beat at the END "
                                             "of the fade: ")
                                      .arg(notMatchedWhy)) +
                    QStringLiteral("%1 bars, fade %2 -> %3 s (limit %4 s), incoming starts at "
                                   "%5 s (beat at %6 s, %7)")
                            .arg(plan->bars) // an unmatched mix may be a quick switch
                    .arg(plan->fromFadeBeginSec)
                    .arg(plan->fromFadeEndSec)
                    .arg(fromLimitSec)
                    .arg(plan->toStartSec)
                    .arg(toBodyStartSec)
                    .arg(toBodyMarked ? introSource : QStringLiteral("measured"))
                    .append(fromOutroMarked ? QStringLiteral(", limit from Outro Start")
                                            : QString()));
    m_plannedMix.valid = true;
    m_plannedMix.fromId = pFromTrack->getId();
    m_plannedMix.toId = pToTrack->getId();
    m_plannedMix.matched = matched;
    pFromDeck->fadeBeginPos = plan->fromFadeBeginSec;
    pFromDeck->fadeEndPos = plan->fromFadeEndSec;
    pToDeck->startPos = plan->toStartSec;
    m_lastAlignApplied = true;
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

phrasealign::Grid AutoDJProcessor::gridFor(const TrackPointer& pTrack) const {
    phrasealign::Grid grid = AnalyzerEnergy::beatGrid(pTrack);
    if (!grid.isValid() || !m_pTrackCollectionManager ||
            !m_pTrackCollectionManager->internalCollection()) {
        return grid;
    }
    double bpm = 0.0;
    double firstBeatSec = 0.0;
    if (!AnalyzerEnergy::gridOf(pTrack, &bpm, &firstBeatSec)) {
        return grid;
    }
    const auto found = EnergyStore::loadDownbeat(
            m_pTrackCollectionManager->internalCollection()->database(), pTrack->getId());
    if (!found || !found->sure || found->phase <= 0 || !found->isFor(bpm, firstBeatSec)) {
        return grid; // the grid's first line is beat 1, or not sure
    }
    if (grid.isMap()) {
        const phrasealign::Grid moved =
                phrasealign::Grid::fromBeats(downbeat::fromBeatOne(grid.beats, found->phase));
        return moved.isValid() ? moved : grid;
    }
    grid.firstBeatSec += found->phase * grid.beatSec;
    return grid;
}

bool AutoDJProcessor::gridsAllowBeatmatch(const TrackPointer& pFromTrack,
        const TrackPointer& pToTrack,
        QString* pWhy) const {
    if (!m_pTrackCollectionManager || !m_pTrackCollectionManager->internalCollection()) {
        return true;
    }
    const QSqlDatabase db = m_pTrackCollectionManager->internalCollection()->database();
    for (const TrackPointer& pTrack : {pFromTrack, pToTrack}) {
        if (!pTrack) {
            continue;
        }
        double bpm = 0.0;
        double firstBeatSec = 0.0;
        AnalyzerEnergy::gridOf(pTrack, &bpm, &firstBeatSec);
        const auto check = EnergyStore::loadGridCheck(db, pTrack->getId());
        // Not checked yet, or the DJ changed the grid since: trust it.
        if (check && check->isFor(bpm, firstBeatSec, gridFor(pTrack).isMap()) &&
                check->driftBeats > EnergyCalculator::kGridMaxDriftBeats) {
            if (pWhy) {
                *pWhy = QStringLiteral("the beat grid of \"%1\" drifts %2 beats off the music")
                                .arg(pTrack->getInfo())
                                .arg(check->driftBeats, 0, 'f', 2);
            }
            return false;
        }
    }
    return true;
}

bool AutoDJProcessor::tryPhraseFadeNow() {
    DeckAttributes* pLeftDeck = getLeftDeck();
    DeckAttributes* pRightDeck = getRightDeck();
    if (!pLeftDeck || !pRightDeck) {
        return false;
    }
    // Same choice of decks as fadeNow().
    DeckAttributes* pFromDeck;
    DeckAttributes* pToDeck;
    if (pLeftDeck->isPlaying() && (!pRightDeck->isPlaying() || getCrossfader() < 0.0)) {
        pFromDeck = pLeftDeck;
        pToDeck = pRightDeck;
    } else if (pRightDeck->isPlaying()) {
        pFromDeck = pRightDeck;
        pToDeck = pLeftDeck;
    } else {
        return false;
    }
    if (pToDeck->isPlaying()) {
        return false; // both playing: let Mixxx handle it as before
    }
    const TrackPointer pFromTrack = pFromDeck->getLoadedTrack();
    if (!pFromTrack || !pToDeck->getLoadedTrack()) {
        return false;
    }
    const mixxx::BeatsPointer pBeats = pFromTrack->getBeats();
    const double bpm = pFromTrack->getBpm();
    const double duration = getEndSecond(pFromDeck);
    if (!pBeats || !(bpm > 0.0) || !(duration > 0.0)) {
        return false;
    }
    const double ratio = pFromDeck->rateRatio();
    const phrasealign::Grid grid = gridFor(pFromTrack).atSpeed(ratio);
    if (!grid.isValid()) {
        return false;
    }
    const double nowSec = pFromDeck->playPosition() * duration;
    const double wantedSec = m_transitionTime > 0.0 ? m_transitionTime : 16.0;
    const int bars = phrasealign::barsForSeconds(wantedSec, grid.beatSecAt(nowSec));
    const double limitSec = phrasealign::fadeNowLimitSec(grid, nowSec, bars);
    if (limitSec < 0.0) {
        return false;
    }
    m_fadeNowLimit.trackId = pFromTrack->getId();
    m_fadeNowLimit.trackSec = limitSec * ratio;
    pFromDeck->setRepeat(false);
    pFromDeck->isFromDeck = true;
    pToDeck->isFromDeck = false;
    calculateTransition(pFromDeck, pToDeck, false);
    if (!m_lastAlignApplied) {
        m_fadeNowLimit = FadeNowLimit();
        kLogger.info() << "Fade now: no phrase fits, fading right away";
        return false;
    }
    const double beginSec = pFromDeck->fadeBeginPos * duration;
    kLogger.info() << "Fade now: waits for the next phrase, the mix starts at" << beginSec
                   << "s (in" << beginSec - nowSec << "s)";
    return true;
}

QList<std::pair<QString, QString>> AutoDJProcessor::smartFillSources() const {
    QList<std::pair<QString, QString>> sources;
    if (!m_pTrackCollectionManager || !m_pTrackCollectionManager->internalCollection()) {
        return sources;
    }
    const QSqlDatabase db = m_pTrackCollectionManager->internalCollection()->database();
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("SELECT id, name FROM crates ORDER BY name"))) {
        while (query.next()) {
            sources.append(std::make_pair(QStringLiteral("crate:") + query.value(0).toString(),
                    tr("Crate: %1").arg(query.value(1).toString())));
        }
    }
    // hidden = 0: the DJ's own playlists (not Auto DJ or history).
    if (query.exec(QStringLiteral(
                "SELECT id, name FROM Playlists WHERE hidden = 0 ORDER BY name"))) {
        while (query.next()) {
            sources.append(std::make_pair(QStringLiteral("playlist:") + query.value(0).toString(),
                    tr("Playlist: %1").arg(query.value(1).toString())));
        }
    }
    return sources;
}

QStringList AutoDJProcessor::smartFill(int count,
        MixScoreWeights::EnergyDirection energy,
        bool avoidSameArtist,
        const QString& source) {
    QStringList added;
    if (count <= 0 || !m_pTrackCollectionManager ||
            !m_pTrackCollectionManager->internalCollection()) {
        return added;
    }
    const QSqlDatabase db = m_pTrackCollectionManager->internalCollection()->database();
    const QVector<TrackFeatures> allTracks = loadBridgeCandidates(db);
    // Only take songs from the chosen crate or playlist (if any). The start
    // track (last queued) may come from anywhere.
    QVector<TrackFeatures> library = allTracks;
    const QString kind = source.section(QChar(':'), 0, 0);
    const QString sourceId = source.section(QChar(':'), 1);
    if (kind == QStringLiteral("crate") || kind == QStringLiteral("playlist")) {
        QSqlQuery query(db);
        query.prepare(kind == QStringLiteral("crate")
                        ? QStringLiteral("SELECT track_id FROM crate_tracks WHERE crate_id = :id")
                        : QStringLiteral(
                                  "SELECT track_id FROM PlaylistTracks WHERE playlist_id = :id"));
        query.bindValue(QStringLiteral(":id"), sourceId.toInt());
        QSet<TrackId> members;
        if (query.exec()) {
            while (query.next()) {
                members.insert(TrackId(query.value(0)));
            }
        }
        library.clear();
        for (const TrackFeatures& t : allTracks) {
            if (members.contains(t.id)) {
                library.append(t);
            }
        }
        kLogger.info() << "Smart fill: taking songs from" << source << "-" << library.size()
                       << "usable tracks";
    }
    QHash<TrackId, const TrackFeatures*> byId;
    for (const TrackFeatures& t : allTracks) {
        byId.insert(t.id, &t);
    }
    // Never add a track that is queued or loaded, or another copy of it.
    QSet<TrackId> excludeIds;
    QSet<QString> excludeNames;
    const auto exclude = [&](const TrackId& id) {
        excludeIds.insert(id);
        if (const TrackFeatures* p = byId.value(id)) {
            if (!p->displayName.isEmpty()) {
                excludeNames.insert(BridgeFinder::nameKey(*p));
            }
        }
    };
    const QList<std::pair<TrackId, int>> queue = m_pAutoDJTableModel->getTrackIdsAndPositions();
    for (const auto& entry : queue) {
        exclude(entry.first);
    }
    // Start from: the last queued track; else the playing track; else any
    // loaded track; else pick an opening track (see below).
    TrackId lastId;
    TrackId loadedId;
    for (const auto& pDeck : m_decks) {
        if (const TrackPointer pTrack = pDeck->getLoadedTrack()) {
            exclude(pTrack->getId());
            if (pDeck->isPlaying()) {
                lastId = pTrack->getId();
            }
            if (!loadedId.isValid()) {
                loadedId = pTrack->getId();
            }
        }
    }
    if (!lastId.isValid()) {
        lastId = loadedId;
    }
    if (!queue.isEmpty()) {
        lastId = queue.last().first;
    }
    const TrackFeatures* pLast = byId.value(lastId);
    QList<TrackFeatures> chain;
    std::optional<TrackFeatures> opener;
    if (!pLast) {
        if (!queue.isEmpty()) {
            kLogger.info() << "Smart fill: the last queued track has no key or BPM";
            return added;
        }
        // Nothing to start from: open with a calm track when building energy.
        opener = BridgeFinder::pickStart(library,
                excludeIds,
                excludeNames,
                energy == MixScoreWeights::EnergyDirection::Build,
                QRandomGenerator::global()->generate());
        if (!opener) {
            return added;
        }
        exclude(opener->id);
        chain.append(*opener);
        pLast = &*opener;
    }
    MixScoreWeights weights;
    weights.direction = energy;
    chain += BridgeFinder(MixScorer(weights))
                     .extend(*pLast,
                             library,
                             excludeIds,
                             excludeNames,
                             count - static_cast<int>(chain.size()),
                             avoidSameArtist,
                             // never 0 (0 = no randomness)
                             QRandomGenerator::global()->generate() | 1u);
    PlaylistDAO& playlistDao = m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    const int playlistId = m_pAutoDJTableModel->getPlaylist();
    for (const TrackFeatures& t : chain) {
        if (playlistDao.appendTrackToPlaylist(t.id, playlistId)) {
            added << (opener && t.id == opener->id
                            ? tr("Opening track (chosen for you): %1")
                                      .arg(MixScorer::trackText(t))
                            : MixScorer::trackText(t));
        }
    }
    m_pAutoDJTableModel->select();
    kLogger.info() << "Smart fill: added" << added.size() << "tracks";
    return added;
}

double AutoDJProcessor::skipToMix() {
    // Land this long before the mix, so the end of the track is heard first.
    constexpr double kLeadSec = 10.0;
    if (m_eState != ADJ_IDLE) {
        return 0.0;
    }
    DeckAttributes* pFromDeck = getFromDeck();
    if (!pFromDeck || !pFromDeck->isPlaying()) {
        return 0.0;
    }
    const double duration = getEndSecond(pFromDeck);
    if (!(duration > 0.0)) {
        return 0.0;
    }
    // fadeBeginPos is a fraction of the track here (see calculateTransition).
    const double nowSec = pFromDeck->playPosition() * duration;
    const double targetSec = pFromDeck->fadeBeginPos * duration - kLeadSec;
    if (targetSec <= nowSec) {
        return 0.0;
    }
    kLogger.info() << "Skip to mix:" << pFromDeck->group << "jumps from" << nowSec
                   << "s to" << targetSec << "s";
    pFromDeck->setPlayPosition(targetSec / duration);
    return targetSec - nowSec;
}

void AutoDJProcessor::resetDeckTempo(DeckAttributes* pDeck, bool trackDone) {
    if (!pDeck || !m_keylockBefore.contains(pDeck->group)) {
        return; // we never changed this deck
    }
    if (m_glide.pDeck == pDeck) {
        m_glide.pDeck = nullptr;
    }
    writeControl(ConfigKey(pDeck->group, QStringLiteral("rate_ratio")), 1.0);
    if (m_keyShift.contains(pDeck->group)) {
        if (!trackDone) {
            return; // keep key lock on while the key morph plays
        }
        // Played out: the deck back to the track's own key.
        writeControl(ConfigKey(pDeck->group, QStringLiteral("pitch_adjust")), 0.0);
        m_keyShift.remove(pDeck->group);
    }
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
    const QList<std::pair<int, QList<TrackId>>> bridges = m_pendingBridges;
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
        // One or two tracks: inserting the last one first at the same
        // position leaves them in their order.
        const QList<TrackId>& ids = it->second;
        for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
            if (playlistDao.insertTrackIntoPlaylist(*id, playlistId, current[k - 1].second + 1)) {
                ++added;
            }
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
    m_bridgeGaps = result.gaps;
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
    // Auto DJ 2.0 plus Video Mixing: with beatmatch on, wait for the next phrase so the mix
    // stays on the beat. Falls back to fading right away if that fails.
    if (isBeatmatchEnabled() && tryPhraseFadeNow()) {
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
        m_fadeNowLimit = FadeNowLimit(); // a pending Fade Now is cancelled
        // A glide in progress carries on (m_glideTicker): the last track of
        // the queue still eases back to its own tempo after Auto DJ stops.
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

    // Auto DJ 2.0 plus Video Mixing: the queue ran empty and the last song
    // has now ended with nothing on the other deck: switch Auto DJ off.
    if (m_eState == ADJ_IDLE && thisDeck->getLoadedTrack() && !otherDeck->getLoadedTrack() &&
            !otherDeckPlaying &&
            (thisPlayPosition >= 1.0 || (!thisDeckPlaying && fromDeckAtFadeOrEnd))) {
        kLogger.info() << "The last song has ended and the queue is empty: Auto DJ off";
        toggleAutoDJ(false);
        return;
    }

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
        fillQueue(tracksToAdd);
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
        // Auto DJ 2.0 plus Video Mixing: while a song is still playing, Auto DJ
        // stays on, so the DJ can add more songs to the queue during the last
        // one; they are then loaded and mixed as usual. It switches itself
        // off when that last song ends (see playerPositionChanged). Only with
        // nothing playing does it switch off at once, as Mixxx does.
        const DeckAttributes* pLeft = getLeftDeck();
        const DeckAttributes* pRight = getRightDeck();
        const bool somethingPlaying =
                (pLeft && pLeft->isPlaying()) || (pRight && pRight->isPlaying());
        if (!somethingPlaying) {
            toggleAutoDJ(false);
        } else if (!m_queueEmptyLogged) {
            m_queueEmptyLogged = true;
            kLogger.info() << "Queue empty: Auto DJ stays on until the last song ends";
        }
        // Empty the free deck (nextTrack is null) as "End of auto DJ warning",
        // so the finished song is not played again. Only if it holds a track:
        // ejecting calls this function again (playerEmpty).
        if (deck.getLoadedTrack()) {
            emitLoadTrackToPlayer(nextTrack, deck.group, false);
        }
        return false;
    }
    m_queueEmptyLogged = false;

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
        fillQueue(tracksToAdd);
    }
}

void AutoDJProcessor::fillQueue(int tracksToAdd) {
    // Auto DJ 2.0 plus Video Mixing: top up with Smart Fill (the DJ's saved Smart Fill
    // options), and only fall back to random tracks for whatever Smart Fill
    // could not find, so the music never stops.
    if (m_pConfig->getValue(ConfigKey(kPreferenceGroup, QStringLiteral("SmartFillAuto")), true)) {
        const int energyChoice = m_pConfig->getValue(
                ConfigKey(kPreferenceGroup, QStringLiteral("SmartFillEnergy")), 0);
        const auto energy = energyChoice == 1
                ? MixScoreWeights::EnergyDirection::Hold
                : (energyChoice == 2 ? MixScoreWeights::EnergyDirection::Wave
                                     : MixScoreWeights::EnergyDirection::Build);
        const bool avoidArtist = m_pConfig->getValue(
                ConfigKey(kPreferenceGroup, QStringLiteral("SmartFillAvoidSameArtist")), true);
        const QString source = m_pConfig->getValue(
                ConfigKey(kPreferenceGroup, QStringLiteral("SmartFillSource")), QString());
        const QStringList added = smartFill(tracksToAdd, energy, avoidArtist, source);
        kLogger.info() << "Auto fill: Smart Fill added" << added.size() << "of" << tracksToAdd;
        tracksToAdd -= static_cast<int>(added.size());
        if (tracksToAdd <= 0) {
            return;
        }
        kLogger.info() << "Auto fill: nothing else mixes smoothly, adding" << tracksToAdd
                       << "random tracks";
    } else {
        qDebug() << "Randomly adding tracks";
    }
    emit randomTrackRequested(tracksToAdd);
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

    // Auto DJ 2.0 plus Video Mixing, phase 2: put a beatmatched fade on phrase boundaries.
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
    // When this track's analysis finishes (e.g. the beat grid check of a
    // track analysed on load), plan the mix again with the new results.
    if (pTrack) {
        connect(pTrack.get(),
                &Track::analyzed,
                this,
                &AutoDJProcessor::trackAnalyzed,
                Qt::UniqueConnection);
    }

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

void AutoDJProcessor::trackAnalyzed() {
    const Track* pTrack = qobject_cast<const Track*>(sender());
    if (!pTrack || m_eState != ADJ_IDLE) {
        return; // not while a mix is running
    }
    DeckAttributes* pFromDeck = getFromDeck();
    DeckAttributes* pToDeck = pFromDeck ? getOtherDeck(pFromDeck) : nullptr;
    if (!pToDeck) {
        return;
    }
    const TrackPointer pFrom = pFromDeck->getLoadedTrack();
    const TrackPointer pTo = pToDeck->getLoadedTrack();
    if (pFrom.get() != pTrack && pTo.get() != pTrack) {
        return; // no longer on a deck
    }
    kLogger.info() << "Analysis of" << pTrack->getInfo()
                   << "finished: planning the mix again";
    calculateTransition(pFromDeck, pToDeck, false);
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

double AutoDJProcessor::secondsUntilMix() {
    if (m_eState == ADJ_DISABLED) {
        return -1.0;
    }
    if (m_eState == ADJ_LEFT_FADING || m_eState == ADJ_RIGHT_FADING) {
        return 0.0;
    }
    DeckAttributes* pFromDeck = getFromDeck();
    if (!pFromDeck || !pFromDeck->isPlaying()) {
        return -1.0;
    }
    const mixxx::audio::SampleRate sampleRate = pFromDeck->sampleRate();
    const mixxx::audio::FramePos end = pFromDeck->trackEndPosition();
    if (!sampleRate.isValid() || !end.isValid() || pFromDeck->fadeBeginPos > 1.0) {
        return -1.0;
    }
    // fadeBeginPos is a fraction of the track once the mix is planned.
    const double durationSec = end.value() / sampleRate;
    return std::max(0.0, (pFromDeck->fadeBeginPos - pFromDeck->playPosition()) * durationSec);
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
