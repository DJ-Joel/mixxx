#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <array>
#include <memory>

#include "effects/defs.h"
#include "preferences/usersettings.h"

class ControlObject;
class ControlPotmeter;
class ControlPushButton;
class EffectsManager;
class PlayerManager;

/// Auto DJ 2.0 plus Video Mixing: the DJ's simple stem controls.
///
/// Per deck ([ChannelN]): three knobs with kill buttons - stem_vocals,
/// stem_instrumental (bass + other) and stem_drums - that drive the four
/// part volumes of a song playing from its stem file ([ChannelN_StemM],
/// volume), plus stem_echo_out: the vocals leave with an echo that rings
/// out. stem_bass (not on screen, for Auto DJ) scales the bass inside the
/// instrumental. [Stems],vocal_swap fades the vocals from the louder playing deck to
/// the other one over 4 bars. All are controls, so a MIDI controller can use
/// them too. Songs without parts: the controls do nothing (stem_ready 0).
class StemControls : public QObject {
    Q_OBJECT
  public:
    StemControls(UserSettingsPointer pConfig,
            EffectsManager* pEffectsManager,
            PlayerManager* pPlayerManager,
            QObject* pParent = nullptr);
    ~StemControls() override;

    static constexpr int kDecks = 4;
    /// Part order in the stem file: drums, bass, other, vocals.
    static constexpr int kDrums = 0;
    static constexpr int kBass = 1;
    static constexpr int kOther = 2;
    static constexpr int kVocals = 3;

  private:
    struct Deck {
        std::unique_ptr<ControlPotmeter> pVocals;
        std::unique_ptr<ControlPotmeter> pInstrumental;
        std::unique_ptr<ControlPotmeter> pDrums;
        /// The bass inside the instrumental (1 = with it). Not on screen:
        /// Auto DJ uses it to swap drums + bass apart from the synths.
        std::unique_ptr<ControlPotmeter> pBass;
        std::unique_ptr<ControlPushButton> pVocalsKill;
        std::unique_ptr<ControlPushButton> pInstrumentalKill;
        std::unique_ptr<ControlPushButton> pDrumsKill;
        std::unique_ptr<ControlPushButton> pEchoOut;
        std::unique_ptr<ControlObject> pEchoActive;
        std::unique_ptr<ControlObject> pReady;
        std::array<double, 4> written{{-1.0, -1.0, -1.0, -1.0}}; ///< last part volumes set
        // Echo out
        int echoStage = 0;    ///< 0 off, 1 echo on and waiting for the beat, 2 ringing out
        qint64 echoCutMs = 0; ///< when the vocals go (timer ms)
        qint64 echoEndMs = 0; ///< when the echo is taken away again
        EffectManifestPointer echoPrevious;
        double echoPreviousSuper = 0.0;
        double echoPreviousEnabled = 0.0;
    };

    void apply(int deck);
    void resetDeck(int deck);
    void connectDecks();
    void tick();
    void startEchoOut(int deck);
    void finishEchoOut(int deck, bool restoreOnly);
    void startVocalSwap();
    QString group(int deck) const;
    bool hasParts(int deck) const;
    double beatSeconds(int deck) const;
    double secondsToNextBeat(int deck) const;
    double loudness(int deck) const;

    UserSettingsPointer m_pConfig;
    EffectsManager* m_pEffectsManager;
    PlayerManager* m_pPlayerManager;
    std::array<Deck, kDecks> m_decks;
    std::unique_ptr<ControlPushButton> m_pVocalSwap;
    std::unique_ptr<ControlObject> m_pVocalSwapActive;
    QSet<int> m_connectedDecks;
    QTimer m_timer;
    QElapsedTimer m_clock;
    // Vocal swap: from deck m_swapOut to deck m_swapIn.
    int m_swapOut = -1;
    int m_swapIn = -1;
    qint64 m_swapStartMs = 0;
    qint64 m_swapEndMs = 0;
    double m_swapOutFrom = 1.0;
    double m_swapInFrom = 0.0;
};
