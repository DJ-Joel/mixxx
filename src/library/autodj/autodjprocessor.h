#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

#include "audio/frame.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "control/pollingcontrolproxy.h"
#include "engine/channels/enginechannel.h"
#include "library/playlisttablemodel.h"
#include "library/autodj/smart/beatmatch.h"
#include "library/autodj/smart/bridgefinder.h"
#include "library/autodj/smart/phrasealign.h"
#include "library/autodj/smart/smartsequencer.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/class.h"
#include "util/parented_ptr.h"

class TrackCollectionManager;
class PlayerManagerInterface;
class BaseTrackPlayer;
typedef QList<QModelIndex> QModelIndexList;

class DeckAttributes : public QObject {
    Q_OBJECT
  public:
    DeckAttributes(int index,
            BaseTrackPlayer* pPlayer);
    virtual ~DeckAttributes();

    bool isLeft() const {
        return m_orientation.get() == static_cast<double>(EngineChannel::LEFT);
    }

    bool isRight() const {
        return m_orientation.get() == static_cast<double>(EngineChannel::RIGHT);
    }

    bool isPlaying() const {
        return m_play.toBool();
    }

    void stop() {
        m_play.set(0.0);
    }

    void play() {
        m_play.set(1.0);
    }

    double playPosition() const {
        return m_playPos.get();
    }

    void setPlayPosition(double playpos) {
        m_playPos.set(playpos);
    }

    bool isRepeat() const {
        return m_repeat.toBool();
    }

    void setRepeat(bool enabled) {
        m_repeat.set(enabled ? 1.0 : 0.0);
    }

    mixxx::audio::FramePos introStartPosition() const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(m_introStartPos.get());
    }

    mixxx::audio::FramePos introEndPosition() const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(m_introEndPos.get());
    }

    mixxx::audio::FramePos outroStartPosition() const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(m_outroStartPos.get());
    }

    mixxx::audio::FramePos outroEndPosition() const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(m_outroEndPos.get());
    }

    mixxx::audio::SampleRate sampleRate() const {
        return mixxx::audio::SampleRate::fromDouble(m_sampleRate.get());
    }

    mixxx::audio::FramePos trackEndPosition() const {
        return mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(m_trackSamples.get());
    }

    double rateRatio() const {
        return m_rateRatio.get();
    }

    TrackPointer getLoadedTrack() const;

  signals:
    void playChanged(DeckAttributes* pDeck, bool playing);
    void playPositionChanged(DeckAttributes* pDeck, double playPosition);
    void introStartPositionChanged(DeckAttributes* pDeck, double introStartPosition);
    void introEndPositionChanged(DeckAttributes* pDeck, double introEndPosition);
    void outroStartPositionChanged(DeckAttributes* pDeck, double outtroStartPosition);
    void outroEndPositionChanged(DeckAttributes* pDeck, double outroEndPosition);
    void trackLoaded(DeckAttributes* pDeck, TrackPointer pTrack);
    void loadingTrack(DeckAttributes* pDeck, TrackPointer pNewTrack, TrackPointer pOldTrack);
    void playerEmpty(DeckAttributes* pDeck);
    void rateChanged(DeckAttributes* pDeck);
    void orientationChanged(DeckAttributes* pDeck);

  private slots:
    void slotPlayPosChanged(double v);
    void slotPlayChanged(double v);
    void slotIntroStartPositionChanged(double v);
    void slotIntroEndPositionChanged(double v);
    void slotOutroStartPositionChanged(double v);
    void slotOutroEndPositionChanged(double v);
    void slotTrackLoaded(TrackPointer pTrack);
    void slotLoadingTrack(TrackPointer pNewTrack, TrackPointer pOldTrack);
    void slotPlayerEmpty();
    void slotRateChanged(double v);
    void slotOrientationChanged(double v);

  public:
    int index;
    QString group;
    double startPos;     // Set in toDeck nature
    double fadeBeginPos; // set in fromDeck nature
    double fadeEndPos;   // set in fromDeck nature
    bool isFromDeck;
    bool loading; // The data is inconsistent during loading a deck

  private:
    ControlProxy m_orientation;
    ControlProxy m_playPos;
    ControlProxy m_play;
    ControlProxy m_repeat;
    ControlProxy m_introStartPos;
    ControlProxy m_introEndPos;
    ControlProxy m_outroStartPos;
    ControlProxy m_outroEndPos;
    ControlProxy m_trackSamples;
    ControlProxy m_sampleRate;
    ControlProxy m_rateRatio;
    BaseTrackPlayer* m_pPlayer;
};

class AutoDJProcessor : public QObject {
    Q_OBJECT
  public:
    enum AutoDJState {
        ADJ_IDLE = 0,
        ADJ_LEFT_FADING,
        ADJ_RIGHT_FADING,
        ADJ_ENABLE_P1LOADED,
        ADJ_ENABLE_P1PLAYING,
        ADJ_DISABLED
    };

    enum AutoDJError {
        ADJ_OK = 0,
        ADJ_IS_INACTIVE,
        ADJ_QUEUE_EMPTY,
        ADJ_BOTH_DECKS_PLAYING,
        ADJ_UNUSED_DECK_PLAYING,
        ADJ_NOT_TWO_DECKS
    };

    enum class TransitionMode {
        FullIntroOutro,
        FadeAtOutroStart,
        FixedFullTrack,
        FixedSkipSilence,
        FixedStartCenterSkipSilence
    };

    AutoDJProcessor(QObject* pParent,
                    UserSettingsPointer pConfig,
                    PlayerManagerInterface* pPlayerManager,
                    TrackCollectionManager* pTrackCollectionManager,
                    int iAutoDJPlaylistId);
    virtual ~AutoDJProcessor() = default;

    AutoDJState getState() const {
        return m_eState;
    }

    double getTransitionTime() const {
        return m_transitionTime;
    }

    TransitionMode getTransitionMode() const {
        return m_transitionMode;
    }

    PlaylistTableModel* getTableModel() const {
        return m_pAutoDJTableModel;
    }

    bool nextTrackLoaded();

    /// Auto DJ 2.0 plus Video Mixing: seconds until the next mix starts (the
    /// outgoing song reaches its fade point); 0 while mixing; -1 when Auto
    /// DJ is off or no mix is planned.
    double secondsUntilMix();

    void setTransitionTime(int seconds);

    void setTransitionMode(TransitionMode newMode);

    AutoDJError shufflePlaylist(const QModelIndexList& selectedIndices);
    /// Auto DJ 2.0 plus Video Mixing: reorder the queue for smooth key/BPM/energy flow.
    /// Runs in the background; the result arrives as smartSortFinished()
    /// or smartSortFailed(). While Auto DJ runs, the first track stays first.
    AutoDJError smartSortPlaylist();
    bool isSmartSortRunning() const {
        return m_smartSortRunning;
    }
    /// Auto DJ 2.0 plus Video Mixing: the DJ's own energy rating, 1..10. 0 clears it.
    bool setEnergyRating(const QList<TrackId>& trackIds, int rating);
    /// Energy of one track: {value 1..10, rated by DJ?}. value 0 = unknown.
    std::pair<double, bool> energyOf(TrackId trackId) const;
    /// Bridge tracks suggested by the last Smart Sort, not yet added.
    int pendingBridgeCount() const {
        return static_cast<int>(m_pendingBridges.size());
    }
    /// Auto DJ 2.0 plus Video Mixing, phase 2: beatmatched transitions with a bass swap.
    bool isBeatmatchEnabled() const;
    void setBeatmatchEnabled(bool enabled);
    // Auto DJ 2.0 plus Video Mixing key morph: pitch a beatmatched incoming track by up to this
    // many semitones (0 = off) when its key clashes with the outgoing one.
    int keyMorphLimit() const;
    void setKeyMorphLimit(int semitones);

    // Auto DJ 2.0 plus Video Mixing Live Assistant: the best next tracks for the deck that is
    // playing live (the one heard most), from the whole library.
    struct LiveSuggestions {
        QString deckGroup; // empty = nothing is playing
        TrackFeatures now; // with the deck's live tempo and key
        QList<NextSuggestion> next;
    };
    LiveSuggestions liveSuggestions(int count);
    // Remembers a track that was heard live this session, so the Live
    // Assistant never suggests it again. (Mixxx's own "played" mark only
    // reaches the database later, when the track leaves memory.)
    void notePlayedLive(const TrackPointer& pTrack);
    // Loads a track, paused, on the deck that is not playing. Returns false
    // (with the reason in pMessage) if there is no free deck or Auto DJ is on.
    bool loadOnFreeDeck(TrackId trackId, QString* pMessage);

    /// For testing mixes quickly: jumps the playing track to a few seconds
    /// before its planned mix, so the mix itself still happens exactly as
    /// planned (unlike seeking into or past it). Returns the seconds skipped,
    /// or 0 if there was nothing to skip (Auto DJ off, already mixing, or
    /// already close to the mix).
    double skipToMix();

    /// Every clash of the last Smart Sort with all its bridge options.
    const QList<BridgeGap>& bridgeGaps() const {
        return m_bridgeGaps;
    }
    /// The DJ's choice of bridges, as (k, track): insert after the k-th
    /// track (from 1). Replaces the automatic best picks.
    void choosePendingBridges(const QList<std::pair<int, QList<TrackId>>>& picks) {
        m_pendingBridges = picks;
    }
    /// Smart Fill: appends up to `count` library tracks that each mix
    /// smoothly after the one before, starting from the last queued track
    /// (or the playing one if the queue is empty). Returns their names.
    /// Another version of a queued song is never added (BridgeFinder::nameKey).
    /// @param energy build up / keep level / up and down
    /// @param avoidSameArtist never the same artist twice in a row
    /// @param source "" = whole library, "crate:<id>" or "playlist:<id>"
    QStringList smartFill(int count,
            MixScoreWeights::EnergyDirection energy,
            bool avoidSameArtist,
            const QString& source);
    /// Crates and playlists Smart Fill can take songs from:
    /// (source key for smartFill, name to show).
    QList<std::pair<QString, QString>> smartFillSources() const;
    /// Tops up the queue when it runs low: Smart Fill first (config
    /// [Auto DJ] SmartFillAuto, default on), random tracks for the rest.
    void fillQueue(int tracksToAdd);

    /// Adds the suggested bridge tracks into the gaps they bridge.
    /// Returns how many were added (0 if the queue changed meanwhile).
    int insertPendingBridges();
    AutoDJError skipNext();
    void fadeNow();
    AutoDJError toggleAutoDJ(bool enable);

  signals:
#ifdef __STEM__
    void loadTrackToPlayer(TrackPointer pTrack,
            const QString& group,
            mixxx::StemChannelSelection stemMask,
            bool play);
#else
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);
#endif
    void autoDJStateChanged(AutoDJProcessor::AutoDJState state);
    void autoDJError(AutoDJProcessor::AutoDJError error);
    void transitionTimeChanged(int time);
    void randomTrackRequested(int tracksToAdd);
    /// clashCount = transitions that could not be made smooth.
    /// orderLines = the whole new running order, with clash notes.
    void smartSortFinished(int trackCount,
            int clashCount,
            const QStringList& warnings,
            const QStringList& orderLines);
    void smartSortFailed(const QString& message);

  private slots:
    void crossfaderChanged(double value);
    void playerPositionChanged(DeckAttributes* pDeck, double position);
    void playerPlayChanged(DeckAttributes* pDeck, bool playing);
    void playerIntroStartChanged(DeckAttributes* pDeck, double position);
    void playerIntroEndChanged(DeckAttributes* pDeck, double position);
    void playerOutroStartChanged(DeckAttributes* pDeck, double position);
    void playerOutroEndChanged(DeckAttributes* pDeck, double position);
    void playerTrackLoaded(DeckAttributes* pDeck, TrackPointer pTrack);
    void playerLoadingTrack(DeckAttributes* pDeck, TrackPointer pNewTrack, TrackPointer pOldTrack);
    void playerEmpty(DeckAttributes* pDeck);
    void playerRateChanged(DeckAttributes* pDeck);
    void playerOrientationChanged(DeckAttributes* pDeck);
    void playlistFirstTrackChanged();

    void controlEnableChangeRequest(double value);
    void controlFadeNow(double value);
    void controlShuffle(double value);
    void controlSkipNext(double value);
    void controlAddRandomTrack(double value);
    void slotNumberOfDecksChanged(int decks);

  protected:
    // The following virtual signal wrappers are used for testing
    virtual void emitLoadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play) {
        emit loadTrackToPlayer(pTrack, group,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                play);
    }
    virtual void emitAutoDJStateChanged(AutoDJProcessor::AutoDJState state) {
        emit autoDJStateChanged(state);
    }

  private:
    // Gets or sets the crossfader position while normalizing it so that -1 is
    // all the way mixed to the left side and 1 is all the way mixed to the
    // right side. (prevents AutoDJ logic from having to check for hamster mode
    // every time)
    double getCrossfader() const;
    void setCrossfader(double value);

    // Following functions return seconds computed from samples or -1 if
    // track in deck has invalid sample rate (<= 0)
    double getIntroStartSecond(DeckAttributes* pDeck);
    double getIntroEndSecond(DeckAttributes* pDeck);
    double getOutroStartSecond(DeckAttributes* pDeck);
    double getOutroEndSecond(DeckAttributes* pDeck);
    double getFirstSoundSecond(DeckAttributes* pDeck);
    double getLastSoundSecond(DeckAttributes* pDeck);
    double getEndSecond(DeckAttributes* pDeck);
    double framePositionToSeconds(mixxx::audio::FramePos position, DeckAttributes* pDeck);

    TrackPointer getNextTrackFromQueue();
    bool loadNextTrackFromQueue(const DeckAttributes& pDeck, bool play = false);
    void calculateTransition(DeckAttributes* pFromDeck,
            DeckAttributes* pToDeck,
            bool seekToStartPoint);
    void useFixedFadeTime(
            DeckAttributes* pFromDeck,
            DeckAttributes* pToDeck,
            double fromDeckSecond,
            double fadeEndSecond,
            double toDeckStartSecond);
    DeckAttributes* getLeftDeck();
    DeckAttributes* getRightDeck();
    DeckAttributes* getOtherDeck(const DeckAttributes* pThisDeck);
    DeckAttributes* getFromDeck();

    // Removes the track loaded to the player group from the top of the AutoDJ
    // queue if it is present.
    bool removeLoadedTrackFromTopOfQueue(const DeckAttributes& deck);

    // Removes the provided track from the top of the AutoDJ queue if it is
    // present.
    bool removeTrackFromTopOfQueue(TrackPointer pTrack);
    void maybeFillRandomTracks();
    void applySmartSortResult(const SequenceResult& result,
            const QList<std::pair<TrackId, int>>& snapshot);
    UserSettingsPointer m_pConfig;
    parented_ptr<PlaylistTableModel> m_pAutoDJTableModel;

    AutoDJState m_eState;
    double m_transitionProgress;
    double m_transitionTime; // the desired value set by the user
    TransitionMode m_transitionMode;
    bool m_crossfaderStartCenter;

    PlayerManagerInterface* m_pPlayerManager;
    std::vector<std::unique_ptr<DeckAttributes>> m_decks;

    ControlProxy m_coCrossfader;
    PollingControlProxy m_coCrossfaderReverse;

    ControlPushButton m_shufflePlaylist;
    ControlPushButton m_skipNext;
    ControlPushButton m_addRandomTrack;
    ControlPushButton m_fadeNow;
    ControlPushButton m_enabledAutoDJ;

    bool m_smartSortRunning = false;
    // Best bridge per clash from the last Smart Sort: (k, track) = insert
    // after the k-th track of m_pendingBridgeOrder (counted from 1).
    QList<std::pair<int, QList<TrackId>>> m_pendingBridges; // one or two tracks each
    QList<TrackId> m_pendingBridgeOrder;
    QList<BridgeGap> m_bridgeGaps;
    // Fade Now with beatmatch on: the mix waits for the next phrase of this
    // track. trackSec is the "fade over by" limit, in the track's own time.
    struct FadeNowLimit {
        TrackId trackId;
        double trackSec = -1.0;
    };
    FadeNowLimit m_fadeNowLimit;
    bool m_lastAlignApplied = false;
    bool tryPhraseFadeNow();
    // Auto DJ 2.0 plus Video Mixing: false if the beat grid check found that one of the two
    // tracks has a grid that drifts off the beat (then a plain fade).
    bool gridsAllowBeatmatch(const TrackPointer& pFromTrack,
            const TrackPointer& pToTrack,
            QString* pWhy) const;

    // Auto DJ 2.0 plus Video Mixing, phase 2: beatmatch + bass swap during a fade.
    void beginSmartTransition(DeckAttributes* pFromDeck, DeckAttributes* pToDeck);
    /// Singing detection: does the outgoing song sing during the mix? (true
    /// when not known or not in that mode)
    bool outgoingSingsInMix(DeckAttributes* pFromDeck);
    /// Singing detection: plan the vocals of a stem mix (m_smart.vocalPlan).
    void planVocals(DeckAttributes* pFromDeck, DeckAttributes* pToDeck);
    void afterToDeckStarted();
    void updateSmartTransition(double progress);
    void followBeats();
    /// A loaded track's analysis finished: plan the mix again.
    void trackAnalyzed();
    void endSmartTransition(bool completed);
    void updateGlide(DeckAttributes* pDeck);
    struct SmartTransition {
        bool active = false;
        DeckAttributes* pFrom = nullptr;
        DeckAttributes* pTo = nullptr;
        bool beatmatched = false;
        double toRatio = 1.0;
        // Values to put back afterwards (NaN = control missing).
        double fromLowKill = 0.0;
        double toLowKill = 0.0;
        // The DJ's mid and high EQ settings (full EQ transition).
        double fromMid = 1.0;
        double fromHigh = 1.0;
        double toMid = 1.0;
        double toHigh = 1.0;
        double toQuantize = 0.0;
        double toKeylock = 0.0;
        int toKeyShift = 0; // key morph, semitones (0 = none)
        // Beat lock: when a beat map bends the tempo, the incoming speed
        // follows the outgoing beats all through the mix.
        bool beatLock = false;
        phrasealign::Grid fromGrid; // seconds at each track's own speed
        phrasealign::Grid toGrid;
        int lockUpdates = 0;
        double worstSlipBeats = 0.0;
        double progress = 0.0;       // how far the mix is (0..1)
        int resyncs = 0;             // jumps back in line (early in the mix)
        int lastResyncUpdate = -1000;
        int loggedBar = -1000;       // the slip is logged once per bar
        // Stem mix: both songs have their parts, so the parts cross over
        // instead of the EQ (drums + bass swap, vocals never together). The
        // DJ's own part levels, put back afterwards.
        bool stems = false;
        struct StemLevels {
            double vocals = 1.0;
            double instrumental = 1.0;
            double drums = 1.0;
            double bass = 1.0;
            double vocalsKill = 0.0;
            double instrumentalKill = 0.0;
            double drumsKill = 0.0;
        };
        StemLevels fromStems;
        StemLevels toStems;
        // Singing detection: who sings during the mix (planVocals).
        bool vocalPlanned = false;
        beatmatch::VocalPlan vocalPlan;
    };
    SmartTransition m_smart;
    // The queue ran empty while a song was playing (logged once).
    bool m_queueEmptyLogged = false;
    // The last phrase plan's decision: beatmatched or a switch. The mix
    // itself follows it (see beginSmartTransition).
    struct PlannedMix {
        bool valid = false;
        TrackId fromId;
        TrackId toId;
        bool matched = false;
    };
    PlannedMix m_plannedMix;
    struct Glide {
        DeckAttributes* pDeck = nullptr;
        double startRatio = 1.0;
        double lastWritten = 1.0; // to notice the DJ moving the tempo
        TrackId trackId;          // to notice a new track on the deck
        QElapsedTimer timer;
    };
    Glide m_glide;
    // Drives the glide on its own, so it also finishes after Auto DJ has
    // switched itself off (the last track of the queue).
    QTimer m_glideTicker;
    void startGlide(DeckAttributes* pDeck, double startRatio);
    // Decks whose tempo/key lock we changed, with the key lock to restore.
    // A deck is reset to its own tempo once its track has faded out, so the
    // next track loaded there does not inherit a leftover tempo.
    QHash<QString, double> m_keylockBefore;
    // trackDone = the track has faded out. Before that, a deck with a key
    // morph keeps key lock on (turning it off would undo the morph).
    void resetDeckTempo(DeckAttributes* pDeck, bool trackDone);
    // The key morph now on a deck (semitones), 0 if none or another track.
    int currentKeyShift(DeckAttributes* pDeck) const;
    struct KeyShift {
        TrackId trackId;
        int semitones = 0;
    };
    QHash<QString, KeyShift> m_keyShift;
    QSet<TrackId> m_playedLive;
    // Moves a planned beatmatched fade onto phrase boundaries (seconds, as
    // used inside calculateTransition before they become fractions).
    void alignTransitionToPhrases(DeckAttributes* pFromDeck,
            DeckAttributes* pToDeck,
            double fromDeckPositionSec);
    TrackCollectionManager* m_pTrackCollectionManager = nullptr;

    DISALLOW_COPY_AND_ASSIGN(AutoDJProcessor);
};
