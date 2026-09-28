#include "stems/stemcontrols.h"

#include <QtDebug>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "effects/effectsmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "moc_stemcontrols.cpp"

namespace {

constexpr int kTickMs = 40;       // fades are updated 25 times a second
constexpr double kSwapBeats = 16; // 4 bars
constexpr double kEchoBeats = 16; // the echo rings out for 4 bars
constexpr double kDefaultBeatSeconds = 0.5;
constexpr double kHalfPi = 1.5707963267948966;

ControlObject* control(const QString& group, const QString& item) {
    return ControlObject::getControl(ConfigKey(group, item), ControlFlag::AllowMissingOrInvalid);
}

double value(const QString& group, const QString& item, double fallback = 0.0) {
    ControlObject* pControl = control(group, item);
    return pControl ? pControl->get() : fallback;
}

void setValue(const QString& group, const QString& item, double newValue) {
    if (ControlObject* pControl = control(group, item)) {
        pControl->set(newValue);
    }
}

QString stemGroup(int deck, int part) {
    return QStringLiteral("[Channel%1_Stem%2]").arg(deck + 1).arg(part + 1);
}

QString vocalsChainGroup(int deck) {
    return QStringLiteral("[QuickEffectRack1_%1]").arg(stemGroup(deck, StemControls::kVocals));
}

} // namespace

StemControls::StemControls(UserSettingsPointer pConfig,
        EffectsManager* pEffectsManager,
        PlayerManager* pPlayerManager,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pEffectsManager(pEffectsManager),
          m_pPlayerManager(pPlayerManager) {
    for (int deck = 0; deck < kDecks; ++deck) {
        Deck& d = m_decks[deck];
        const QString g = group(deck);
        auto makeKnob = [&](const QString& item) {
            auto pKnob = std::make_unique<ControlPotmeter>(
                    ConfigKey(g, item), 0.0, 1.0, false, true, false, false, 1.0);
            pKnob->set(1.0);
            connect(pKnob.get(), &ControlObject::valueChanged, this, [this, deck](double) {
                apply(deck);
            });
            return pKnob;
        };
        auto makeKill = [&](const QString& item) {
            auto pKill = std::make_unique<ControlPushButton>(ConfigKey(g, item));
            pKill->setButtonMode(mixxx::control::ButtonMode::PowerWindow);
            connect(pKill.get(), &ControlObject::valueChanged, this, [this, deck](double) {
                apply(deck);
            });
            return pKill;
        };
        d.pVocals = makeKnob(QStringLiteral("stem_vocals"));
        d.pInstrumental = makeKnob(QStringLiteral("stem_instrumental"));
        d.pDrums = makeKnob(QStringLiteral("stem_drums"));
        d.pVocalsKill = makeKill(QStringLiteral("stem_vocals_kill"));
        d.pInstrumentalKill = makeKill(QStringLiteral("stem_instrumental_kill"));
        d.pDrumsKill = makeKill(QStringLiteral("stem_drums_kill"));
        d.pEchoOut = std::make_unique<ControlPushButton>(ConfigKey(g, QStringLiteral("stem_echo_out")));
        connect(d.pEchoOut.get(), &ControlObject::valueChanged, this, [this, deck](double v) {
            if (v > 0.0) {
                startEchoOut(deck);
            }
        });
        d.pEchoActive = std::make_unique<ControlObject>(
                ConfigKey(g, QStringLiteral("stem_echo_out_active")));
        d.pEchoActive->setReadOnly();
        d.pReady = std::make_unique<ControlObject>(ConfigKey(g, QStringLiteral("stem_ready")));
        d.pReady->setReadOnly();
    }
    m_pVocalSwap = std::make_unique<ControlPushButton>(
            ConfigKey(QStringLiteral("[Stems]"), QStringLiteral("vocal_swap")));
    connect(m_pVocalSwap.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0.0) {
            startVocalSwap();
        }
    });
    m_pVocalSwapActive = std::make_unique<ControlObject>(
            ConfigKey(QStringLiteral("[Stems]"), QStringLiteral("vocal_swap_active")));
    m_pVocalSwapActive->setReadOnly();

    connectDecks();
    if (m_pPlayerManager) {
        connect(m_pPlayerManager,
                &PlayerManager::numberOfDecksChanged,
                this,
                [this](int) {
                    connectDecks();
                });
    }
    m_clock.start();
    connect(&m_timer, &QTimer::timeout, this, &StemControls::tick);
    m_timer.start(kTickMs);
}

StemControls::~StemControls() {
    m_timer.stop();
    for (int deck = 0; deck < kDecks; ++deck) {
        if (m_decks[deck].echoStage != 0) {
            finishEchoOut(deck, true);
        }
    }
}

QString StemControls::group(int deck) const {
    return QStringLiteral("[Channel%1]").arg(deck + 1);
}

bool StemControls::hasParts(int deck) const {
    return value(group(deck), QStringLiteral("stem_count")) >= 4.0;
}

void StemControls::connectDecks() {
    if (!m_pPlayerManager) {
        return;
    }
    const int count = std::min(kDecks, m_pPlayerManager->numberOfDecks());
    for (int deck = 0; deck < count; ++deck) {
        if (m_connectedDecks.contains(deck)) {
            continue;
        }
        BaseTrackPlayer* pPlayer = m_pPlayerManager->getDeck(deck);
        if (!pPlayer) {
            continue;
        }
        m_connectedDecks.insert(deck);
        // A new song: every control back to normal (the engine sets the part
        // volumes back to full too).
        connect(pPlayer, &BaseTrackPlayer::newTrackLoaded, this, [this, deck](TrackPointer) {
            resetDeck(deck);
        });
        connect(pPlayer, &BaseTrackPlayer::playerEmpty, this, [this, deck]() {
            resetDeck(deck);
        });
    }
}

void StemControls::resetDeck(int deck) {
    Deck& d = m_decks[deck];
    if (d.echoStage != 0) {
        finishEchoOut(deck, true);
    }
    if (deck == m_swapOut || deck == m_swapIn) {
        m_swapOut = -1;
        m_swapIn = -1;
        m_pVocalSwapActive->forceSet(0.0);
    }
    d.pVocals->set(1.0);
    d.pInstrumental->set(1.0);
    d.pDrums->set(1.0);
    d.pVocalsKill->set(0.0);
    d.pInstrumentalKill->set(0.0);
    d.pDrumsKill->set(0.0);
    // Nothing written: the engine already starts the new song at full.
    d.written.fill(1.0);
}

void StemControls::apply(int deck) {
    if (!hasParts(deck)) {
        return; // no parts (yet): the controls do nothing
    }
    Deck& d = m_decks[deck];
    const double vocals = d.pVocalsKill->toBool() ? 0.0 : d.pVocals->get();
    const double instrumental = d.pInstrumentalKill->toBool() ? 0.0 : d.pInstrumental->get();
    const double drums = d.pDrumsKill->toBool() ? 0.0 : d.pDrums->get();
    const std::array<double, 4> parts{{drums, instrumental, instrumental, vocals}};
    for (int part = 0; part < 4; ++part) {
        // Only what changed, so the four knobs of the stem panel keep
        // working as well.
        if (std::abs(parts[part] - d.written[part]) > 1e-6) {
            setValue(stemGroup(deck, part), QStringLiteral("volume"), parts[part]);
            d.written[part] = parts[part];
        }
    }
}

double StemControls::beatSeconds(int deck) const {
    const double bpm = value(group(deck), QStringLiteral("bpm"));
    return bpm > 30.0 ? 60.0 / bpm : kDefaultBeatSeconds;
}

double StemControls::secondsToNextBeat(int deck) const {
    if (value(group(deck), QStringLiteral("play")) <= 0.0) {
        return 0.0;
    }
    const double distance = std::clamp(value(group(deck), QStringLiteral("beat_distance")), 0.0, 1.0);
    const double rate = std::max(0.25, std::abs(value(group(deck), QStringLiteral("rate_ratio"), 1.0)));
    return (1.0 - distance) * beatSeconds(deck) / rate;
}

double StemControls::loudness(int deck) const {
    const QString g = group(deck);
    if (value(g, QStringLiteral("play")) <= 0.0) {
        return 0.0;
    }
    // Channel fader times crossfader side (0 left, 1 middle, 2 right).
    const double fader = value(g, QStringLiteral("volume"), 1.0);
    const double position = value(QStringLiteral("[Master]"), QStringLiteral("crossfader"));
    const int side = static_cast<int>(value(g, QStringLiteral("orientation"), 1.0));
    double crossfader = 1.0;
    if (side == 0) {
        crossfader = position <= 0.0 ? 1.0 : 1.0 - position;
    } else if (side == 2) {
        crossfader = position >= 0.0 ? 1.0 : 1.0 + position;
    }
    return fader * crossfader;
}

void StemControls::startVocalSwap() {
    if (m_swapOut >= 0) {
        return; // one at a time
    }
    // The two loudest playing decks with parts.
    int out = -1;
    int in = -1;
    for (int deck = 0; deck < kDecks; ++deck) {
        if (!hasParts(deck) || value(group(deck), QStringLiteral("play")) <= 0.0) {
            continue;
        }
        if (out < 0 || loudness(deck) > loudness(out)) {
            in = out;
            out = deck;
        } else if (in < 0 || loudness(deck) > loudness(in)) {
            in = deck;
        }
    }
    if (out < 0 || in < 0) {
        qInfo() << "Stems: vocal swap needs two playing decks whose songs have their parts";
        return;
    }
    // A killed vocal counts as 0 (the knob takes over).
    for (const int deck : {out, in}) {
        Deck& d = m_decks[deck];
        if (d.pVocalsKill->toBool()) {
            d.pVocals->set(0.0);
            d.pVocalsKill->set(0.0);
        }
    }
    m_swapOut = out;
    m_swapIn = in;
    m_swapOutFrom = m_decks[out].pVocals->get();
    m_swapInFrom = m_decks[in].pVocals->get();
    const double wait = secondsToNextBeat(out);
    const double length = kSwapBeats * beatSeconds(out) /
            std::max(0.25, std::abs(value(group(out), QStringLiteral("rate_ratio"), 1.0)));
    m_swapStartMs = m_clock.elapsed() + static_cast<qint64>(wait * 1000.0);
    m_swapEndMs = m_swapStartMs + static_cast<qint64>(length * 1000.0);
    m_pVocalSwapActive->forceSet(1.0);
    qInfo().noquote() << "Stems: vocal swap deck" << out + 1 << "-> deck" << in + 1 << "over"
                      << QString::number(length, 'f', 1) << "s";
}

void StemControls::startEchoOut(int deck) {
    Deck& d = m_decks[deck];
    if (d.echoStage != 0 || !hasParts(deck)) {
        return;
    }
    if (d.pVocalsKill->toBool() || d.pVocals->get() <= 0.0) {
        return; // no vocals to take away
    }
    const bool playing = value(group(deck), QStringLiteral("play")) > 0.0;
    EffectSlotPointer pSlot;
    EffectManifestPointer pEcho;
    if (playing && m_pEffectsManager) {
        const EffectChainPointer pChain = m_pEffectsManager->getEffectChain(vocalsChainGroup(deck));
        pSlot = pChain ? pChain->getEffectSlot(0) : EffectSlotPointer();
        pEcho = m_pEffectsManager->getBackendManager()->getManifest(
                QStringLiteral("org.mixxx.effects.echo"), EffectBackendType::BuiltIn);
    }
    if (!pSlot || !pEcho) {
        // Stopped (or no echo available): just take the vocals away.
        d.pVocalsKill->set(1.0);
        apply(deck);
        return;
    }
    // The echo goes on the vocals' own effect slot for a moment; the DJ's
    // effect comes back afterwards.
    const QString chain = vocalsChainGroup(deck);
    d.echoPrevious = pSlot->getManifest();
    d.echoPreviousSuper = value(chain, QStringLiteral("super1"), 0.5);
    d.echoPreviousEnabled = value(chain, QStringLiteral("enabled"));
    pSlot->loadEffectWithDefaults(pEcho);
    setValue(chain, QStringLiteral("super1"), 1.0); // echo send fully open
    setValue(chain, QStringLiteral("enabled"), 1.0);
    setValue(chain.chopped(1) + QStringLiteral("_Effect1]"), QStringLiteral("enabled"), 1.0);
    const double wait = secondsToNextBeat(deck);
    const double tail = kEchoBeats * beatSeconds(deck);
    d.echoCutMs = m_clock.elapsed() + static_cast<qint64>(wait * 1000.0);
    d.echoEndMs = d.echoCutMs + static_cast<qint64>(tail * 1000.0);
    d.echoStage = 1;
    d.pEchoActive->forceSet(1.0);
    qInfo().noquote() << "Stems: echo out deck" << deck + 1;
}

void StemControls::finishEchoOut(int deck, bool restoreOnly) {
    Q_UNUSED(restoreOnly);
    Deck& d = m_decks[deck];
    if (d.echoStage == 0) {
        return;
    }
    d.echoStage = 0;
    d.pEchoActive->forceSet(0.0);
    if (!m_pEffectsManager) {
        return;
    }
    const QString chain = vocalsChainGroup(deck);
    const EffectChainPointer pChain = m_pEffectsManager->getEffectChain(chain);
    const EffectSlotPointer pSlot = pChain ? pChain->getEffectSlot(0) : EffectSlotPointer();
    if (pSlot) {
        pSlot->loadEffectWithDefaults(d.echoPrevious);
    }
    setValue(chain, QStringLiteral("super1"), d.echoPreviousSuper);
    setValue(chain, QStringLiteral("enabled"), d.echoPreviousEnabled);
    d.echoPrevious.clear();
}

void StemControls::tick() {
    const qint64 now = m_clock.elapsed();
    for (int deck = 0; deck < kDecks; ++deck) {
        Deck& d = m_decks[deck];
        const bool ready = hasParts(deck);
        if (d.pReady->get() != (ready ? 1.0 : 0.0)) {
            d.pReady->forceSet(ready ? 1.0 : 0.0);
            if (ready) {
                apply(deck); // parts arrived: use the knobs as they are
            }
        }
        if (d.echoStage == 1 && now >= d.echoCutMs) {
            d.pVocalsKill->set(1.0); // the vocals go, the echo rings on
            apply(deck);
            d.echoStage = 2;
        } else if (d.echoStage == 2 && now >= d.echoEndMs) {
            finishEchoOut(deck, false);
        }
    }
    if (m_swapOut >= 0 && now >= m_swapStartMs) {
        const double length = std::max<double>(1.0, static_cast<double>(m_swapEndMs - m_swapStartMs));
        const double progress = std::clamp((now - m_swapStartMs) / length, 0.0, 1.0);
        // Out goes to silence, in comes up to full, both on an equal-power
        // curve so the vocal level stays even during the swap.
        const double out = m_swapOutFrom * std::cos(progress * kHalfPi);
        const double in = m_swapInFrom + (1.0 - m_swapInFrom) * std::sin(progress * kHalfPi);
        m_decks[m_swapOut].pVocals->set(out);
        m_decks[m_swapIn].pVocals->set(in);
        apply(m_swapOut);
        apply(m_swapIn);
        if (progress >= 1.0) {
            m_swapOut = -1;
            m_swapIn = -1;
            m_pVocalSwapActive->forceSet(0.0);
        }
    }
}
