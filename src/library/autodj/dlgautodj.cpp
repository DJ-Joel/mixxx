#include "library/autodj/dlgautodj.h"

#include <QActionGroup>
#include <QComboBox>
#include <algorithm>
#include <QDialog>
#include <QGridLayout>
#include <QScrollArea>
#include <QToolTip>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include <QGuiApplication>
#include <QHeaderView>
#include <QScreen>
#include <cmath>
#include <QTableWidget>
#include <QTimer>

#include "control/controlobject.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#ifdef __FFMPEG__
#include "video/videomanager.h"
#endif
#include "library/library.h"
#include "library/playlisttablemodel.h"
#include "moc_dlgautodj.cpp"
#include "track/track.h"
#include "util/assert.h"
#include "util/duration.h"
#include "widget/wlibrary.h"
#include "widget/wtracktableview.h"

namespace {
const char* kPreferenceGroupName = "[Auto DJ]";
const char* kRepeatPlaylistPreference = "Requeue";
// Auto DJ 2.0 Smart Fill options: 0 = build up, 1 = keep level, 2 = up and down.
const char* kSmartFillEnergyPreference = "SmartFillEnergy";
const char* kSmartFillAvoidArtistPreference = "SmartFillAvoidSameArtist";
// "" = whole library, "crate:<id>" or "playlist:<id>".
const char* kSmartFillSourcePreference = "SmartFillSource";
} // anonymous namespace

DlgAutoDJ::DlgAutoDJ(WLibrary* parent,
        UserSettingsPointer pConfig,
        Library* pLibrary,
        AutoDJProcessor* pProcessor,
        KeyboardEventFilter* pKeyboard)
        : QWidget(parent),
          Ui::DlgAutoDJ(),
          m_pConfig(pConfig),
          m_pAutoDJProcessor(pProcessor),
          m_pTrackTableView(new WTrackTableView(this,
                  m_pConfig,
                  pLibrary,
                  parent->getTrackTableBackgroundColorOpacity())),
          m_bShowButtonText(parent->getShowButtonText()),
          m_pAutoDJTableModel(nullptr) {
    setupUi(this);

    m_pTrackTableView->installEventFilter(pKeyboard);

    connect(m_pTrackTableView,
            &WTrackTableView::loadTrack,
            this,
            &DlgAutoDJ::loadTrack);
    connect(m_pTrackTableView,
            &WTrackTableView::loadTrackToPlayer,
            this,
            &DlgAutoDJ::loadTrackToPlayer);
    connect(m_pTrackTableView,
            &WTrackTableView::trackSelected,
            this,
            &DlgAutoDJ::trackSelected);
    connect(m_pTrackTableView,
            &WTrackTableView::trackSelected,
            this,
            &DlgAutoDJ::updateSelectionInfo);

    connect(pLibrary,
            &Library::setTrackTableFont,
            m_pTrackTableView,
            &WTrackTableView::setTrackTableFont);
    connect(pLibrary,
            &Library::setTrackTableRowHeight,
            m_pTrackTableView,
            &WTrackTableView::setTrackTableRowHeight);
    connect(pLibrary,
            &Library::setSelectedClick,
            m_pTrackTableView,
            &WTrackTableView::setSelectedClick);

    QBoxLayout* box = qobject_cast<QBoxLayout*>(layout());
    VERIFY_OR_DEBUG_ASSERT(box) { //Assumes the form layout is a QVBox/QHBoxLayout!
    } else {
        box->removeWidget(m_pTrackTablePlaceholder);
        m_pTrackTablePlaceholder->hide();
        box->insertWidget(1, m_pTrackTableView);
    }

    // We do _NOT_ take ownership of this from AutoDJProcessor.
    m_pAutoDJTableModel = m_pAutoDJProcessor->getTableModel();
    m_pTrackTableView->loadTrackModel(m_pAutoDJTableModel);

    // Do not set this because it disables auto-scrolling
    //m_pTrackTableView->setDragDropMode(QAbstractItemView::InternalMove);

    connect(pushButtonAutoDJ,
            &QPushButton::clicked,
            this,
            &DlgAutoDJ::toggleAutoDJButton);

    setupActionButton(pushButtonFadeNow, &DlgAutoDJ::fadeNowButton, tr("Fade"));
    setupActionButton(pushButtonSkipNext, &DlgAutoDJ::skipNextButton, tr("Skip"));
    setupActionButton(pushButtonShuffle, &DlgAutoDJ::shufflePlaylistButton, tr("Shuffle"));
    setupActionButton(pushButtonAddRandomTrack, &DlgAutoDJ::addRandomTrackButton, tr("Random"));

    // Auto DJ 2.0 Smart Sort. Always shows text: skins have no icon for it.
    pushButtonSmartSort->setText(tr("Smart Sort"));
    pushButtonSmartSort->setToolTip(tr(
            "Sort the Auto DJ queue for smooth mixing (key, BPM and energy).\n"
            "\n"
            "While Auto DJ is running, the next track stays first."));
    connect(pushButtonSmartSort,
            &QPushButton::clicked,
            this,
            &DlgAutoDJ::smartSortButton);
    connect(m_pAutoDJProcessor,
            &AutoDJProcessor::smartSortFinished,
            this,
            &DlgAutoDJ::slotSmartSortFinished);
    connect(m_pAutoDJProcessor,
            &AutoDJProcessor::smartSortFailed,
            this,
            &DlgAutoDJ::slotSmartSortFailed);

    // Auto DJ 2.0 Phase 2: beatmatched mixes with a bass swap.
    // The skins do not style this new button, so on/off is shown in its
    // text instead of its colour.
    const auto showBeatmatchState = [this](bool on) {
        pushButtonBeatmatch->setText(on ? tr("Beatmatch: ON") : tr("Beatmatch: OFF"));
    };
    pushButtonBeatmatch->setCheckable(true);
    pushButtonBeatmatch->setChecked(m_pAutoDJProcessor->isBeatmatchEnabled());
    showBeatmatchState(pushButtonBeatmatch->isChecked());
    pushButtonBeatmatch->setToolTip(tr(
            "Beatmatched transitions: the next track is played at the same\n"
            "tempo with its beats lined up (only if within 5%), the bass is\n"
            "swapped halfway, and the tempo then glides back over 30 seconds.\n"
            "If the keys clash, the next track is pitched up or down a little\n"
            "so they fit (key morph).\n"
            "Off: plain crossfade.\n"
            "Right-click: key morph setting."));
    connect(pushButtonBeatmatch,
            &QPushButton::toggled,
            this,
            [this, showBeatmatchState](bool checked) {
                m_pAutoDJProcessor->setBeatmatchEnabled(checked);
                showBeatmatchState(checked);
            });
    // Key morph setting: right-click the Beatmatch button.
    pushButtonBeatmatch->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pushButtonBeatmatch,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint& pos) {
                QMenu menu(this);
                menu.addSection(tr("Key morph, when the keys clash"));
                auto* pGroup = new QActionGroup(&menu);
                const int current = m_pAutoDJProcessor->keyMorphLimit();
                const QList<QPair<int, QString>> choices = {
                        {0, tr("Off")},
                        {1, tr("Up to 1 semitone (recommended)")},
                        {2, tr("Up to 2 semitones (fixes more, voices may sound odd)")},
                };
                for (const auto& choice : choices) {
                    QAction* pAction = menu.addAction(choice.second);
                    pAction->setCheckable(true);
                    pAction->setChecked(choice.first == current);
                    pGroup->addAction(pAction);
                    const int value = choice.first;
                    connect(pAction, &QAction::triggered, this, [this, value]() {
                        m_pAutoDJProcessor->setKeyMorphLimit(value);
                    });
                }
                menu.exec(pushButtonBeatmatch->mapToGlobal(pos));
            });

    // Auto DJ 2.0: skip most of the playing track to hear the next mix now.
    pushButtonSkipToMix->setText(tr("Skip to Mix"));
    pushButtonSkipToMix->setToolTip(tr(
            "Jump the playing track to 10 seconds before the next mix,\n"
            "so you can hear the mix without waiting. The mix itself is\n"
            "not changed (unlike seeking past its start or Fade Now)."));
    connect(pushButtonSkipToMix,
            &QPushButton::clicked,
            this,
            [this]() {
                m_pAutoDJProcessor->skipToMix();
            });

    // Auto DJ 2.0 Smart Fill: add tracks that mix well after the last one.
    pushButtonSmartFill->setText(tr("Smart Fill"));
    pushButtonSmartFill->setToolTip(tr(
            "Add tracks from your library to the end of the queue, each one\n"
            "chosen to mix smoothly (key, tempo within 5%, energy) after the\n"
            "one before. Tracks already queued are never added twice."));
    auto* pFillMenu = new QMenu(pushButtonSmartFill);
    for (int count : {3, 5, 10, 20}) {
        pFillMenu->addAction(tr("Add %1 tracks").arg(count), this, [this, count]() {
            slotSmartFill(count);
        });
    }
    // Options, remembered between sessions.
    pFillMenu->addSeparator();
    auto* pEnergyGroup = new QActionGroup(pFillMenu);
    const int energyChoice = m_pConfig->getValue(
            ConfigKey(kPreferenceGroupName, kSmartFillEnergyPreference), 0);
    const std::pair<QString, int> energyOptions[] = {
            {tr("Energy: build up"), 0},
            {tr("Energy: keep level"), 1},
            {tr("Energy: up and down"), 2},
    };
    for (const auto& [text, value] : energyOptions) {
        QAction* pAction = pFillMenu->addAction(text);
        pAction->setCheckable(true);
        pAction->setChecked(value == energyChoice);
        pEnergyGroup->addAction(pAction);
        connect(pAction, &QAction::triggered, this, [this, value = value]() {
            m_pConfig->setValue(ConfigKey(kPreferenceGroupName, kSmartFillEnergyPreference), value);
        });
    }
    pFillMenu->addSeparator();
    QAction* pAvoidArtist = pFillMenu->addAction(tr("Never the same artist twice in a row"));
    pAvoidArtist->setCheckable(true);
    pAvoidArtist->setChecked(m_pConfig->getValue(
            ConfigKey(kPreferenceGroupName, kSmartFillAvoidArtistPreference), true));
    connect(pAvoidArtist, &QAction::toggled, this, [this](bool on) {
        m_pConfig->setValue(ConfigKey(kPreferenceGroupName, kSmartFillAvoidArtistPreference), on);
    });
    // Automatic top-up: Mixxx's own "add tracks when the queue runs low"
    // (Preferences > Auto DJ) now uses Smart Fill with these options.
    pFillMenu->addSeparator();
    QAction* pAutoFill = pFillMenu->addAction(tr("Keep the queue filled automatically"));
    pAutoFill->setCheckable(true);
    const auto autoFillOn = [this]() {
        return m_pConfig->getValue(ConfigKey(kPreferenceGroupName, "EnableRandomQueue"), false) &&
                m_pConfig->getValue(ConfigKey(kPreferenceGroupName, "SmartFillAuto"), true);
    };
    pAutoFill->setChecked(autoFillOn());
    connect(pFillMenu, &QMenu::aboutToShow, this, [this, pAutoFill, autoFillOn]() {
        pAutoFill->setChecked(autoFillOn()); // may have changed in Preferences
        const int minimum = m_pConfig->getValue(
                ConfigKey(kPreferenceGroupName, "RandomQueueMinimumAllowed"), 5);
        pAutoFill->setToolTip(tr("When fewer than %1 songs are left in the queue, add more "
                                 "with Smart Fill (the minimum is set in Preferences > Auto DJ).")
                                      .arg(minimum));
    });
    connect(pAutoFill, &QAction::toggled, this, [this](bool on) {
        m_pConfig->setValue(ConfigKey(kPreferenceGroupName, "EnableRandomQueue"), on);
        m_pConfig->setValue(ConfigKey(kPreferenceGroupName, "SmartFillAuto"), on);
        if (on) {
            // Top up straight away rather than at the next track change.
            const int minimum = m_pConfig->getValue(
                    ConfigKey(kPreferenceGroupName, "RandomQueueMinimumAllowed"), 5);
            const int missing = minimum - m_pAutoDJTableModel->rowCount();
            if (missing > 0) {
                m_pAutoDJProcessor->fillQueue(missing);
            }
        }
    });
    pFillMenu->setToolTipsVisible(true);

    // Where the songs come from: the whole library, or one crate/playlist.
    // Rebuilt each time the menu opens, so new crates show up.
    pFillMenu->addSeparator();
    QMenu* pSourceMenu = pFillMenu->addMenu(tr("Take songs from"));
    connect(pSourceMenu, &QMenu::aboutToShow, this, [this, pSourceMenu]() {
        pSourceMenu->clear();
        const QString current = m_pConfig->getValue(
                ConfigKey(kPreferenceGroupName, kSmartFillSourcePreference), QString());
        auto* pGroup = new QActionGroup(pSourceMenu);
        QList<std::pair<QString, QString>> sources = {{QString(), tr("Whole library")}};
        sources += m_pAutoDJProcessor->smartFillSources();
        for (const auto& [key, name] : std::as_const(sources)) {
            QAction* pAction = pSourceMenu->addAction(name);
            pAction->setCheckable(true);
            pAction->setChecked(key == current);
            pGroup->addAction(pAction);
            connect(pAction, &QAction::triggered, this, [this, key = key]() {
                m_pConfig->setValue(ConfigKey(kPreferenceGroupName, kSmartFillSourcePreference), key);
            });
        }
        if (sources.size() == 1) {
            QAction* pNone = pSourceMenu->addAction(tr("(no crates or playlists yet)"));
            pNone->setEnabled(false);
        }
    });
    pushButtonSmartFill->setMenu(pFillMenu);

    // Auto DJ 2.0 Live Assistant: a small window that follows the deck
    // playing live and lists the best next songs from the library.
    pushButtonLiveAssistant->setText(tr("Live Assistant"));
    pushButtonLiveAssistant->setToolTip(tr(
            "Opens a small window that follows the song playing live and\n"
            "lists the 10 best songs to play next (key, tempo and energy).\n"
            "Double-click one to load it on the free deck."));
    connect(pushButtonLiveAssistant, &QPushButton::clicked, this, [this]() {
        showLiveAssistant();
    });
    // Auto DJ 2.0 video mixing: the music videos of the decks on a screen
    // or projector, mixed like the sound.
#ifdef __FFMPEG__
    pushButtonVideo->setText(tr("Video"));
    pushButtonVideo->setToolTip(tr(
            "Shows the music videos of the decks on a screen or projector.\n"
            "The picture follows each deck (tempo, loops, jumps) and is mixed\n"
            "like the sound (volume faders and crossfader). Songs without\n"
            "video show their cover art and title. Esc closes the video."));
    auto* pVideoMenu = new QMenu(pushButtonVideo);
    connect(pVideoMenu, &QMenu::aboutToShow, this, [this, pVideoMenu]() {
        pVideoMenu->clear();
        pVideoMenu->addSection(tr("Show the videos full screen on"));
        const QScreen* pMixxxScreen = window() ? window()->screen() : nullptr;
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (QScreen* pScreen : screens) {
            const QRect area = pScreen->geometry();
            QString label = tr("%1 (%2 x %3)")
                                    .arg(pScreen->name())
                                    .arg(area.width())
                                    .arg(area.height());
            if (pScreen == pMixxxScreen) {
                label += tr("  - covers Mixxx, press Esc to close");
            }
            QAction* pAction = pVideoMenu->addAction(label);
            const QPointer<QScreen> pTarget(pScreen);
            connect(pAction, &QAction::triggered, this, [this, pTarget]() {
                if (pTarget) {
                    videoManager()->showOnScreen(pTarget);
                }
            });
        }
        QAction* pWindowed = pVideoMenu->addAction(tr("In a window (for testing)"));
        connect(pWindowed, &QAction::triggered, this, [this]() {
            videoManager()->showInWindow();
        });
        pVideoMenu->addSeparator();
        QAction* pPreview = pVideoMenu->addAction(tr("Preview window"));
        pPreview->setCheckable(true);
        pPreview->setChecked(m_pVideo && m_pVideo->isPreviewVisible());
        connect(pPreview, &QAction::toggled, this, [this](bool visible) {
            videoManager()->setPreviewVisible(visible);
        });
        QAction* pStop = pVideoMenu->addAction(tr("Stop video"));
        connect(pStop, &QAction::triggered, this, [this]() {
            if (m_pVideo) {
                m_pVideo->stop();
            }
        });
    });
    pushButtonVideo->setMenu(pVideoMenu);
#else
    pushButtonVideo->hide(); // needs FFmpeg
#endif

    // Remember every song heard live this session, window open or not.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            [this](TrackPointer pTrack) {
                m_pAutoDJProcessor->notePlayedLive(pTrack);
            });
    m_pAutoDJProcessor->notePlayedLive(PlayerInfo::instance().getCurrentPlayingTrack());

    // Auto DJ 2.0 energy rating: the DJ's own 1..10 score for the selected
    // tracks. It always wins over the measured energy.
    pushButtonEnergy->setText(tr("Energy"));
    pushButtonEnergy->setToolTip(tr(
            "Rate the energy of the selected tracks, 1 (calm) to 10 (peak).\n"
            "Your rating is used by Smart Sort instead of the measured energy."));
    auto* pEnergyMenu = new QMenu(pushButtonEnergy);
    for (int rating = 10; rating >= 1; --rating) {
        QString text = QString::number(rating);
        if (rating == 10) {
            text += tr("  (peak)");
        } else if (rating == 1) {
            text += tr("  (calm)");
        }
        pEnergyMenu->addAction(text, this, [this, rating]() {
            slotSetEnergyRating(rating);
        });
    }
    pEnergyMenu->addSeparator();
    pEnergyMenu->addAction(tr("Clear rating (use measured)"), this, [this]() {
        slotSetEnergyRating(0);
    });
    pushButtonEnergy->setMenu(pEnergyMenu);

    m_enableBtnTooltip = tr(
            "Enable Auto DJ\n"
            "\n"
            "Shortcut: Shift+F12");
    m_disableBtnTooltip = tr(
            "Disable Auto DJ\n"
            "\n"
            "Shortcut: Shift+F12");
    QString fadeBtnTooltip = tr(
            "Trigger the transition to the next track\n"
            "\n"
            "Shortcut: Shift+F11");
    QString skipBtnTooltip = tr(
            "Skip the next track in the Auto DJ queue\n"
            "\n"
            "Shortcut: Shift+F10");
    QString shuffleBtnTooltip = tr(
            "Shuffle the content of the Auto DJ queue\n"
            "\n"
            "Shortcut: Shift+F9");
    QString addRandomTrackBtnTooltip = tr(
            "Adds a random track from track sources (crates) to the Auto DJ queue.\n"
            "If no track sources are configured, the track is added from the library instead.");
    QString repeatBtnTooltip = tr(
            "Repeat the playlist");
    QString spinBoxTransitionTooltip = tr(
            "Determines the duration of the transition");
    QString labelTransitionTooltip = tr(
            // "sec" as in seconds
            "Seconds");
    QString fadeModeTooltip = tr(
            "Auto DJ Fade Modes\n"
            "\n"
            "Full Intro + Outro:\n"
            "Play the full intro and outro. Use the intro or outro length as the\n"
            "crossfade time, whichever is shorter. If no intro or outro are marked,\n"
            "use the selected crossfade time.\n"
            "\n"
            "Fade At Outro Start:\n"
            "Start crossfading at the outro start. If the outro is longer than the\n"
            "intro, cut off the end of the outro. Use the intro or outro length as\n"
            "the crossfade time, whichever is shorter. If no intro or outro are\n"
            "marked, use the selected crossfade time.\n"
            "\n"
            "Full Track:\n"
            "Play the whole track. Begin crossfading from the selected number of\n"
            "seconds before the end of the track. A negative crossfade time adds\n"
            "silence between tracks.\n"
            "\n"
            "Skip Silence:\n"
            "Play the whole track except for silence at the beginning and end.\n"
            "Begin crossfading from the selected number of seconds before the\n"
            "last sound.\n"
            "\n"
            "Skip Silence Start Full Volume:\n"
            "The same as Skip Silence, but starting transitions with a centered\n"
            "crossfader, so that the intro starts at full volume.\n");

    pushButtonFadeNow->setToolTip(fadeBtnTooltip);
    pushButtonSkipNext->setToolTip(skipBtnTooltip);
    pushButtonShuffle->setToolTip(shuffleBtnTooltip);
    pushButtonAddRandomTrack->setToolTip(addRandomTrackBtnTooltip);
    pushButtonRepeatPlaylist->setToolTip(repeatBtnTooltip);
    spinBoxTransition->setToolTip(spinBoxTransitionTooltip);
    labelTransitionAppendix->setToolTip(labelTransitionTooltip);
    fadeModeCombobox->setToolTip(fadeModeTooltip);

    // Prevent the interactive widgets from being focused with Tab or Shift+Tab
    fadeModeCombobox->setFocusPolicy(Qt::ClickFocus);
    spinBoxTransition->setFocusPolicy(Qt::ClickFocus);
    // work around QLineEdit being protected
    QLineEdit* lineEditTransition(spinBoxTransition->findChild<QLineEdit*>());
    lineEditTransition->setFocusPolicy(Qt::ClickFocus);
    // Needed to catch Enter, Return and Escape keypresses
    lineEditTransition->installEventFilter(this);

    connect(spinBoxTransition,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &DlgAutoDJ::transitionSliderChanged);

    fadeModeCombobox->addItem(tr("Full Intro + Outro"),
            static_cast<int>(AutoDJProcessor::TransitionMode::FullIntroOutro));
    fadeModeCombobox->addItem(tr("Fade At Outro Start"),
            static_cast<int>(AutoDJProcessor::TransitionMode::FadeAtOutroStart));
    fadeModeCombobox->addItem(tr("Full Track"),
            static_cast<int>(AutoDJProcessor::TransitionMode::FixedFullTrack));
    fadeModeCombobox->addItem(tr("Skip Silence"),
            static_cast<int>(AutoDJProcessor::TransitionMode::FixedSkipSilence));
    fadeModeCombobox->addItem(tr("Skip Silence Start Full Volume"),
            static_cast<int>(AutoDJProcessor::TransitionMode::FixedStartCenterSkipSilence));
    fadeModeCombobox->setCurrentIndex(
            fadeModeCombobox->findData(static_cast<int>(m_pAutoDJProcessor->getTransitionMode())));
    connect(fadeModeCombobox,
            QOverload<int>::of(&QComboBox::activated),
            this,
            &DlgAutoDJ::slotTransitionModeChanged);

    connect(pushButtonRepeatPlaylist,
            &QPushButton::clicked,
            this,
            &DlgAutoDJ::slotRepeatPlaylistChanged);
    if (m_bShowButtonText) {
        pushButtonRepeatPlaylist->setText(tr("Repeat"));
    }
    bool repeatPlaylist = m_pConfig->getValue<bool>(
            ConfigKey(kPreferenceGroupName, kRepeatPlaylistPreference));
    pushButtonRepeatPlaylist->setChecked(repeatPlaylist);
    slotRepeatPlaylistChanged(repeatPlaylist);

    // Setup DlgAutoDJ UI based on the current AutoDJProcessor state. Keep in
    // mind that AutoDJ may already be active when DlgAutoDJ is created (due to
    // skin changes, etc.).
    spinBoxTransition->setValue(static_cast<int>(m_pAutoDJProcessor->getTransitionTime()));
    connect(m_pAutoDJProcessor,
            &AutoDJProcessor::transitionTimeChanged,
            this,
            &DlgAutoDJ::transitionTimeChanged);

    connect(m_pAutoDJProcessor,
            &AutoDJProcessor::autoDJError,
            this,
            &DlgAutoDJ::autoDJError);

    connect(m_pAutoDJProcessor,
            &AutoDJProcessor::autoDJStateChanged,
            this,
            &DlgAutoDJ::autoDJStateChanged);
    autoDJStateChanged(m_pAutoDJProcessor->getState());

    updateSelectionInfo();
}

DlgAutoDJ::~DlgAutoDJ() {
    qDebug() << "~DlgAutoDJ()";

    // Delete m_pTrackTableView before the table model. This is because the
    // table view saves the header state using the model.
    delete m_pTrackTableView;
}

void DlgAutoDJ::setupActionButton(QPushButton* pButton,
        void (DlgAutoDJ::*pSlot)(bool),
        const QString& fallbackText) {
    connect(pButton, &QPushButton::clicked, this, pSlot);
    if (m_bShowButtonText) {
        pButton->setText(fallbackText);
    }
}

void DlgAutoDJ::onShow() {
    m_pAutoDJTableModel->select();
}

void DlgAutoDJ::onSearch(const QString& text) {
    // Do not allow filtering the Auto DJ playlist, because
    // Auto DJ will work from the filtered table
    Q_UNUSED(text);
}

void DlgAutoDJ::shufflePlaylistButton(bool) {
    QModelIndexList indexList = m_pTrackTableView->selectionModel()->selectedRows();

    // Activate regardless of button being checked
    m_pAutoDJProcessor->shufflePlaylist(indexList);
}

void DlgAutoDJ::smartSortButton(bool) {
    pushButtonSmartSort->setEnabled(false); // re-enabled when the sort ends
    m_pAutoDJProcessor->smartSortPlaylist();
}

void DlgAutoDJ::slotSmartSortFinished(int trackCount,
        int clashCount,
        const QStringList& warnings,
        const QStringList& orderLines) {
    Q_UNUSED(warnings); // the clashes are also marked inside orderLines
    pushButtonSmartSort->setEnabled(true);
    if (trackCount < 2) {
        return;
    }
    // A resizable dialog with a scrollable list, so long track names and
    // long lists are never cut off (a QMessageBox is too narrow for this).
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Smart Sort"));
    auto* pLayout = new QVBoxLayout(&dialog);
    const QString summary = clashCount == 0
            ? tr("Sorted %1 tracks with no key or tempo clashes.").arg(trackCount)
            : tr("Sorted %1 tracks. %2 transitions could not be made smooth "
                 "(marked !! below).\nA bridge track between them would help.")
                      .arg(trackCount)
                      .arg(clashCount);
    auto* pSummary = new QLabel(summary, &dialog);
    pSummary->setWordWrap(true);
    pLayout->addWidget(pSummary);
    auto* pDetails = new QPlainTextEdit(&dialog);
    pDetails->setReadOnly(true);
    pDetails->setLineWrapMode(QPlainTextEdit::NoWrap);
    // Fixed-width font so the key and BPM columns line up.
    pDetails->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    pDetails->setPlainText(orderLines.join(QChar('\n')));
    pLayout->addWidget(pDetails);
    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    connect(pButtons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    // Auto DJ 2.0 bridge tracks, chosen per gap: a gap with only one track
    // that bridges it gets that track; with several, the DJ picks one (the
    // best match is preselected); "no bridge" leaves the gap as it is.
    QList<std::pair<int, QComboBox*>> choices; // gap position, its choice box
    QList<std::pair<int, TrackId>> onlyChoices; // gaps with one option
    const QList<BridgeGap>& gaps = m_pAutoDJProcessor->bridgeGaps();
    int bridgeableGaps = 0;
    for (const BridgeGap& gap : gaps) {
        if (!gap.options.isEmpty()) {
            ++bridgeableGaps;
        }
    }
    if (bridgeableGaps > 0) {
        auto* pBridgeTitle = new QLabel(
                tr("<b>Bridge tracks</b> — choose which track goes into each gap:"), &dialog);
        pLayout->addWidget(pBridgeTitle);
        auto* pGapWidget = new QWidget(&dialog);
        auto* pGrid = new QGridLayout(pGapWidget);
        int row = 0;
        for (const BridgeGap& gap : gaps) {
            auto* pGapLabel = new QLabel(tr("Between %1 and %2:\n  %3\n  -> %4")
                                                 .arg(gap.k)
                                                 .arg(gap.k + 1)
                                                 .arg(gap.fromText, gap.toText),
                    pGapWidget);
            pGapLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            pGrid->addWidget(pGapLabel, row, 0);
            if (gap.options.isEmpty()) {
                pGrid->addWidget(new QLabel(tr("No track in your library bridges this gap."),
                                         pGapWidget),
                        row,
                        1);
            } else if (gap.options.size() == 1) {
                onlyChoices.append(std::make_pair(gap.k, gap.options.first()));
                pGrid->addWidget(new QLabel(tr("Only one fits, selected:\n%1")
                                                    .arg(gap.optionTexts.first()),
                                         pGapWidget),
                        row,
                        1);
            } else {
                auto* pCombo = new QComboBox(pGapWidget);
                for (int i = 0; i < gap.options.size(); ++i) {
                    pCombo->addItem(i == 0 ? tr("%1   (best match)").arg(gap.optionTexts[i])
                                           : gap.optionTexts[i],
                            gap.options[i].toVariant());
                }
                pCombo->addItem(tr("(no bridge here)"), QVariant());
                choices.append(std::make_pair(gap.k, pCombo));
                pGrid->addWidget(pCombo, row, 1);
            }
            ++row;
        }
        auto* pScroll = new QScrollArea(&dialog);
        pScroll->setWidget(pGapWidget);
        pScroll->setWidgetResizable(true);
        pLayout->addWidget(pScroll);

        QPushButton* pAddBridges = pButtons->addButton(
                tr("Add chosen bridge tracks"), QDialogButtonBox::ActionRole);
        pAddBridges->setToolTip(tr(
                "Adds the chosen track into each gap.\n"
                "You can remove any of them from the queue afterwards."));
        connect(pAddBridges, &QPushButton::clicked, &dialog, [this, &dialog, choices, onlyChoices]() {
            // One track can only go into one gap. The DJ's own choices must
            // not repeat; a track that is the ONLY option for two gaps goes
            // into the first of them.
            QList<std::pair<int, TrackId>> picks;
            QList<TrackId> chosen;
            for (const auto& [k, pCombo] : choices) {
                const QVariant value = pCombo->currentData();
                if (!value.isValid()) {
                    continue;
                }
                const TrackId id(value);
                if (chosen.contains(id)) {
                    QMessageBox::warning(&dialog,
                            tr("Smart Sort"),
                            tr("The same track is chosen for two gaps. "
                               "Please choose a different track for one of them."));
                    return;
                }
                chosen.append(id);
                picks.append(std::make_pair(k, id));
            }
            int skipped = 0;
            for (const auto& pick : onlyChoices) {
                if (chosen.contains(pick.second)) {
                    ++skipped; // already chosen for another gap
                    continue;
                }
                chosen.append(pick.second);
                picks.append(pick);
            }
            std::sort(picks.begin(), picks.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            if (skipped > 0) {
                QMessageBox::information(&dialog,
                        tr("Smart Sort"),
                        tr("%n gap(s) had only a track that is already used in another "
                           "gap, so they stay without a bridge.",
                                "",
                                skipped));
            }
            m_pAutoDJProcessor->choosePendingBridges(picks);
            const int added = m_pAutoDJProcessor->insertPendingBridges();
            dialog.accept();
            if (added > 0) {
                QMessageBox::information(this,
                        tr("Smart Sort"),
                        tr("Added %1 bridge tracks to the Auto DJ queue.").arg(added));
            } else {
                QMessageBox::warning(this,
                        tr("Smart Sort"),
                        tr("No bridge tracks were added. The queue may have "
                           "changed since sorting; please sort again."));
            }
        });
    }
    pLayout->addWidget(pButtons);
    dialog.resize(900, bridgeableGaps > 0 ? 720 : 560);
    dialog.exec();
}

void DlgAutoDJ::slotSmartSortFailed(const QString& message) {
    pushButtonSmartSort->setEnabled(true);
    QMessageBox::warning(this, tr("Smart Sort"), message);
}

QList<TrackId> DlgAutoDJ::selectedTrackIds() const {
    QList<TrackId> ids;
    const QModelIndexList rows = m_pTrackTableView->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) {
        const TrackId id = m_pAutoDJTableModel->getTrackId(index);
        if (id.isValid() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    return ids;
}

void DlgAutoDJ::slotSetEnergyRating(int rating) {
    const QList<TrackId> ids = selectedTrackIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this,
                tr("Energy"),
                tr("Select one or more tracks in the Auto DJ queue first."));
        return;
    }
    if (rating == 0 &&
            QMessageBox::question(this,
                    tr("Energy"),
                    tr("Clear your energy rating for %n track(s)?\n"
                       "Smart Sort will use the measured energy instead.",
                            "",
                            static_cast<int>(ids.size()))) != QMessageBox::Yes) {
        return;
    }
    if (!m_pAutoDJProcessor->setEnergyRating(ids, rating)) {
        QMessageBox::warning(this, tr("Energy"), tr("Could not save the energy rating."));
        return;
    }
    updateSelectionInfo();
    // Short "saved" note next to the button (no dialog to click away).
    const QString note = rating == 0
            ? tr("Rating cleared for %n track(s)", "", static_cast<int>(ids.size()))
            : tr("Saved: energy %1 for %n track(s)", "", static_cast<int>(ids.size()))
                      .arg(rating);
    QToolTip::showText(pushButtonEnergy->mapToGlobal(QPoint(0, pushButtonEnergy->height())),
            note,
            pushButtonEnergy,
            QRect(),
            3000);
}

void DlgAutoDJ::slotSmartFill(int count) {
    const int energyChoice = m_pConfig->getValue(
            ConfigKey(kPreferenceGroupName, kSmartFillEnergyPreference), 0);
    const bool avoidArtist = m_pConfig->getValue(
            ConfigKey(kPreferenceGroupName, kSmartFillAvoidArtistPreference), true);
    const auto energy = energyChoice == 1
            ? MixScoreWeights::EnergyDirection::Hold
            : (energyChoice == 2 ? MixScoreWeights::EnergyDirection::Wave
                                 : MixScoreWeights::EnergyDirection::Build);
    // The chosen crate/playlist, if it still exists.
    QString source = m_pConfig->getValue(
            ConfigKey(kPreferenceGroupName, kSmartFillSourcePreference), QString());
    QString sourceName = tr("the whole library");
    if (!source.isEmpty()) {
        bool found = false;
        for (const auto& [key, name] : m_pAutoDJProcessor->smartFillSources()) {
            if (key == source) {
                found = true;
                sourceName = name;
            }
        }
        if (!found) {
            source.clear(); // deleted meanwhile: use the whole library
            m_pConfig->setValue(ConfigKey(kPreferenceGroupName, kSmartFillSourcePreference), source);
        }
    }
    const QStringList added = m_pAutoDJProcessor->smartFill(count, energy, avoidArtist, source);
    if (added.isEmpty()) {
        QMessageBox::information(this,
                tr("Smart Fill"),
                tr("Smart Fill found nothing to add.\n\n"
                   "Either the last track in the queue has no key or BPM yet "
                   "(analyze it first), or nothing in your library mixes smoothly "
                   "after it. Turning off \"Never the same artist twice in a row\" "
                   "or choosing \"Energy: up and down\" gives it more choice."));
        return;
    }
    QMessageBox::information(this,
            tr("Smart Fill"),
            tr("Added %1 of %2 tracks from %5 to the end of the queue:\n\n%3%4")
                    .arg(added.size())
                    .arg(count)
                    .arg(added.join(QChar('\n')),
                            added.size() < count
                                    ? tr("\n\nNothing else mixes smoothly after the last one.")
                                    : QString())
                    .arg(sourceName));
}

void DlgAutoDJ::skipNextButton(bool) {
    // Activate regardless of button being checked
    m_pAutoDJProcessor->skipNext();
}

void DlgAutoDJ::fadeNowButton(bool) {
    // Activate regardless of button being checked
    m_pAutoDJProcessor->fadeNow();
}

void DlgAutoDJ::toggleAutoDJButton(bool enable) {
    m_pAutoDJProcessor->toggleAutoDJ(enable);
}

// TODO If there's a way to migrate the translations move this
// to AutoDJProcessor in order to keep this class minimal
void DlgAutoDJ::autoDJError(AutoDJProcessor::AutoDJError error) {
    switch (error) {
    case AutoDJProcessor::ADJ_NOT_TWO_DECKS:
        QMessageBox::warning(nullptr,
                tr("Auto DJ"),
                tr("Auto DJ requires two decks assigned to opposite sides of the crossfader."),
                QMessageBox::Ok);
        break;
    case AutoDJProcessor::ADJ_BOTH_DECKS_PLAYING:
        QMessageBox::warning(nullptr,
                tr("Auto DJ"),
                tr("One deck must be stopped to enable Auto DJ mode."),
                QMessageBox::Ok);
        break;
    case AutoDJProcessor::ADJ_UNUSED_DECK_PLAYING:
        QMessageBox::warning(nullptr,
                tr("Auto DJ"),
                tr("Decks not used for Auto DJ must be stopped to enable Auto DJ mode."),
                QMessageBox::Ok);
        break;
    case AutoDJProcessor::ADJ_OK:
    default:
        break;
    }
}

void DlgAutoDJ::transitionTimeChanged(int time) {
    spinBoxTransition->setValue(time);
}

void DlgAutoDJ::transitionSliderChanged(int value) {
    m_pAutoDJProcessor->setTransitionTime(value);
}

void DlgAutoDJ::autoDJStateChanged(AutoDJProcessor::AutoDJState state) {
    if (state == AutoDJProcessor::ADJ_DISABLED) {
        pushButtonAutoDJ->setChecked(false);
        pushButtonAutoDJ->setToolTip(m_enableBtnTooltip);
        if (m_bShowButtonText) {
            pushButtonAutoDJ->setText(tr("Enable"));
        }
        pushButtonFadeNow->setEnabled(false);
        pushButtonSkipNext->setEnabled(false);
    } else {
        // No matter the mode, you can always disable once it is enabled.
        pushButtonAutoDJ->setChecked(true);
        pushButtonAutoDJ->setToolTip(m_disableBtnTooltip);
        if (m_bShowButtonText) {
            pushButtonAutoDJ->setText(tr("Disable"));
        }

        // If fading, you can't hit fade now.
        if (state == AutoDJProcessor::ADJ_LEFT_FADING ||
                state == AutoDJProcessor::ADJ_RIGHT_FADING ||
                state == AutoDJProcessor::ADJ_ENABLE_P1LOADED) {
            pushButtonFadeNow->setEnabled(false);
        } else {
            pushButtonFadeNow->setEnabled(true);
        }

        pushButtonSkipNext->setEnabled(true);
    }
}

void DlgAutoDJ::slotTransitionModeChanged(int newIndex) {
    m_pAutoDJProcessor->setTransitionMode(
            static_cast<AutoDJProcessor::TransitionMode>(
                    fadeModeCombobox->itemData(newIndex).toInt()));
    // Clicking on a transition mode item moves keyboard focus to the list widget.
    // Move focus back to the previously focused library widget.
    ControlObject::set(ConfigKey("[Library]", "refocus_prev_widget"), 1);
}

void DlgAutoDJ::slotRepeatPlaylistChanged(bool checked) {
    m_pConfig->setValue(ConfigKey(kPreferenceGroupName, kRepeatPlaylistPreference),
            checked);
}

void DlgAutoDJ::updateSelectionInfo() {
    QModelIndexList indices = m_pTrackTableView->selectionModel()->selectedRows();

    // Derive total duration from the table model. This is much faster than
    // getting the duration from individual track objects.
    mixxx::Duration duration = m_pAutoDJTableModel->getTotalDuration(indices);

    QString label;

    if (!indices.isEmpty()) {
        label.append(mixxx::DurationBase::formatTime(duration.toDoubleSeconds()));
        label.append(QString(" (%1)").arg(indices.size()));
        // Auto DJ 2.0: show the energy when exactly one track is selected.
        if (indices.size() == 1) {
            const auto [energy, manual] = m_pAutoDJProcessor->energyOf(
                    m_pAutoDJTableModel->getTrackId(indices.first()));
            if (energy > 0.0) {
                label.append(manual
                                ? tr("  |  Energy %1 (your rating)").arg(energy, 0, 'f', 0)
                                : tr("  |  Energy %1 (measured)").arg(energy, 0, 'f', 1));
            } else {
                label.append(tr("  |  Energy ?"));
            }
        }
        labelSelectionInfo->setToolTip(tr("Displays the duration and number of selected tracks."));
        labelSelectionInfo->setText(label);
        labelSelectionInfo->setEnabled(true);
    } else {
        labelSelectionInfo->setText("");
        labelSelectionInfo->setEnabled(false);
    }
}

bool DlgAutoDJ::hasFocus() const {
    return m_pTrackTableView->hasFocus();
}

void DlgAutoDJ::setFocus() {
    m_pTrackTableView->setFocus();
}

void DlgAutoDJ::pasteFromSidebar() {
    m_pTrackTableView->pasteFromSidebar();
}

void DlgAutoDJ::keyPressEvent(QKeyEvent* pEvent) {
    // If we receive key events either the mode selector or the spinbox are focused.
    // Return, Enter and Escape move focus back to the previously focused
    // library widget in order to immediately allow keyboard shortcuts again.
    if (pEvent->key() == Qt::Key_Return ||
            pEvent->key() == Qt::Key_Enter ||
            pEvent->key() == Qt::Key_Escape) {
        ControlObject::set(ConfigKey("[Library]", "refocus_prev_widget"), 1);
        return;
    }
    QWidget::keyPressEvent(pEvent);
}

void DlgAutoDJ::saveCurrentViewState() {
    m_pTrackTableView->saveCurrentViewState();
}

bool DlgAutoDJ::restoreCurrentViewState() {
    return m_pTrackTableView->restoreCurrentViewState();
}

void DlgAutoDJ::showLiveAssistant() {
    if (!m_pLiveAssistant) {
        // Qt::Tool: a small window that stays above Mixxx while the DJ works
        // in the library.
        auto* pDialog = new QDialog(this, Qt::Tool);
        pDialog->setWindowTitle(tr("Live Assistant"));
        auto* pLayout = new QVBoxLayout(pDialog);
        m_pLiveNow = new QLabel(pDialog);
        m_pLiveNow->setWordWrap(true);
        m_pLiveNow->setTextFormat(Qt::PlainText);
        m_pLiveTable = new QTableWidget(0, 5, pDialog);
        m_pLiveTable->setHorizontalHeaderLabels(
                {tr("Song"), tr("Key"), tr("BPM"), tr("Energy"), tr("Why it fits")});
        m_pLiveTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_pLiveTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_pLiveTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_pLiveTable->verticalHeader()->setVisible(false);
        m_pLiveTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 5; ++column) {
            m_pLiveTable->horizontalHeader()->setSectionResizeMode(
                    column, QHeaderView::ResizeToContents);
        }
        m_pLiveStatus = new QLabel(pDialog);
        m_pLiveStatus->setWordWrap(true);
        m_pLiveStatus->setTextFormat(Qt::PlainText);
        pLayout->addWidget(m_pLiveNow);
        pLayout->addWidget(m_pLiveTable, 1);
        pLayout->addWidget(m_pLiveStatus);
        pDialog->resize(720, 400);

        connect(m_pLiveTable, &QTableWidget::cellDoubleClicked, pDialog, [this](int row, int) {
            const QTableWidgetItem* pItem = m_pLiveTable->item(row, 0);
            if (!pItem) {
                return;
            }
            QString message;
            m_pAutoDJProcessor->loadOnFreeDeck(TrackId(pItem->data(Qt::UserRole)), &message);
            m_pLiveStatus->setText(message);
            refreshLiveAssistant(true); // the loaded song drops off the list
        });
        // The live deck can change without a new track (the DJ moves the
        // tempo, or the crossfader), so check once a second. It only
        // recomputes when something changed.
        m_pLiveTimer = new QTimer(pDialog);
        m_pLiveTimer->setInterval(1000);
        connect(m_pLiveTimer, &QTimer::timeout, pDialog, [this]() {
            refreshLiveAssistant(false);
        });
        connect(&PlayerInfo::instance(),
                &PlayerInfo::currentPlayingTrackChanged,
                pDialog,
                [this](TrackPointer) {
                    refreshLiveAssistant(true);
                });
        connect(pDialog, &QDialog::finished, pDialog, [this]() {
            m_pLiveTimer->stop();
        });
        m_pLiveAssistant = pDialog;
    }
    m_pLiveStatus->setText(tr("Double-click a song to load it on the free deck."));
    m_pLiveTimer->start();
    m_pLiveAssistant->show();
    m_pLiveAssistant->raise();
    m_pLiveAssistant->activateWindow();
    refreshLiveAssistant(true);
}

void DlgAutoDJ::refreshLiveAssistant(bool force) {
    if (!m_pLiveAssistant || !m_pLiveAssistant->isVisible()) {
        return;
    }
    // Cheap check first: which deck is live, what it plays, its tempo (in
    // half-BPM steps) and key.
    const int deckIndex = PlayerInfo::instance().getCurrentPlayingDeck();
    const TrackPointer pTrack = PlayerInfo::instance().getCurrentPlayingTrack();
    QString state = QStringLiteral("none");
    if (deckIndex >= 0 && pTrack) {
        const QString group = PlayerManager::groupForDeck(deckIndex);
        const ConfigKey bpmKey(group, QStringLiteral("bpm"));
        const ConfigKey keyKey(group, QStringLiteral("key"));
        const double bpm = ControlObject::exists(bpmKey) ? ControlObject::get(bpmKey) : 0.0;
        const double key = ControlObject::exists(keyKey) ? ControlObject::get(keyKey) : 0.0;
        state = QStringLiteral("%1|%2|%3|%4")
                        .arg(group, pTrack->getId().toString())
                        .arg(qRound(bpm * 2))
                        .arg(key);
    }
    if (!force && state == m_liveState) {
        return;
    }
    m_liveState = state;

    const AutoDJProcessor::LiveSuggestions live = m_pAutoDJProcessor->liveSuggestions(10);
    m_pLiveTable->setRowCount(0);
    if (live.deckGroup.isEmpty()) {
        m_pLiveNow->setText(tr("Nothing is playing. Start a deck and the best next songs "
                               "appear here."));
        return;
    }
    const TrackFeatures& now = live.now;
    const auto energyText = [](const TrackFeatures& t) {
        return t.hasEnergy() ? QString::number(t.energy, 'f', 0) : QStringLiteral("?");
    };
    m_pLiveNow->setText(tr("Playing live on %1: %2  |  %3  |  %4 BPM  |  energy %5")
                                .arg(live.deckGroup,
                                        now.displayName,
                                        now.camelotText(),
                                        now.hasBpm() ? QString::number(now.bpm, 'f', 1)
                                                     : QStringLiteral("?"),
                                        energyText(now)));
    if (live.next.isEmpty()) {
        m_pLiveStatus->setText(tr("No song in your library mixes smoothly with this one "
                                  "(key and tempo)."));
        return;
    }
    // Why it fits, in words: key, tempo, energy.
    const auto whyText = [&now](const TrackFeatures& t) {
        QStringList parts;
        const int distance = MixScorer::camelotDistance(now.camelotNumber, t.camelotNumber);
        const bool sameLetter = now.camelotMinor == t.camelotMinor;
        if (distance == 0) {
            parts << (sameLetter ? tr("same key") : tr("relative key"));
        } else if (distance == 1) {
            parts << (sameLetter ? tr("next key") : tr("diagonal key"));
        } else {
            parts << tr("key jump (energy boost)");
        }
        double ratio = t.bpm / now.bpm;
        QString timeNote;
        if (ratio > 1.5) {
            ratio /= 2.0;
            timeNote = tr(" (half time)");
        } else if (ratio < 0.75) {
            ratio *= 2.0;
            timeNote = tr(" (double time)");
        }
        const double pct = (ratio - 1.0) * 100.0;
        parts << (std::fabs(pct) < 0.05
                         ? tr("same tempo") + timeNote
                         : tr("tempo %1%2%").arg(pct > 0 ? QStringLiteral("+") : QString())
                                           .arg(pct, 0, 'f', 1) +
                                 timeNote);
        if (now.hasEnergy() && t.hasEnergy()) {
            const double delta = t.energy - now.energy;
            parts << (std::fabs(delta) < 0.5
                             ? tr("same energy")
                             : tr("energy %1%2")
                                       .arg(delta > 0 ? QStringLiteral("+") : QString())
                                       .arg(delta, 0, 'f', 0));
        }
        return parts.join(QStringLiteral(", "));
    };
    m_pLiveTable->setRowCount(static_cast<int>(live.next.size()));
    for (int row = 0; row < live.next.size(); ++row) {
        const TrackFeatures& t = live.next[row].track;
        auto* pSong = new QTableWidgetItem(t.displayName);
        pSong->setData(Qt::UserRole, t.id.toVariant());
        m_pLiveTable->setItem(row, 0, pSong);
        m_pLiveTable->setItem(row, 1, new QTableWidgetItem(t.camelotText()));
        m_pLiveTable->setItem(row, 2, new QTableWidgetItem(QString::number(t.bpm, 'f', 1)));
        m_pLiveTable->setItem(row, 3, new QTableWidgetItem(energyText(t)));
        m_pLiveTable->setItem(row, 4, new QTableWidgetItem(whyText(t)));
    }
}

VideoManager* DlgAutoDJ::videoManager() {
#ifdef __FFMPEG__
    if (!m_pVideo) {
        m_pVideo = new VideoManager(window());
    }
    return m_pVideo;
#else
    return nullptr;
#endif
}
