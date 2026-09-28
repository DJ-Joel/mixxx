#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include "library/autodj/autodjprocessor.h"
#include "library/autodj/ui_dlgautodj.h"
#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class PlaylistTableModel;
class WLibrary;
class WTrackTableView;
class Library;
class KeyboardEventFilter;
class QDialog;
class QLabel;
class QTableWidget;
class QTimer;
class VideoManager;
class StemSplitter;

class DlgAutoDJ : public QWidget, public Ui::DlgAutoDJ, public LibraryView {
    Q_OBJECT
  public:
    DlgAutoDJ(WLibrary* parent,
            UserSettingsPointer pConfig,
            Library* pLibrary,
            AutoDJProcessor* pProcessor,
            KeyboardEventFilter* pKeyboard);
    ~DlgAutoDJ() override;

    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;
    void pasteFromSidebar() override;
    void onSearch(const QString& text) override;
    void saveCurrentViewState() override;
    bool restoreCurrentViewState() override;

  public slots:
    void shufflePlaylistButton(bool buttonChecked);
    void smartSortButton(bool buttonChecked);
    void slotSmartSortFinished(int trackCount,
            int clashCount,
            const QStringList& warnings,
            const QStringList& orderLines);
    void slotSmartSortFailed(const QString& message);
    void slotSetEnergyRating(int rating);
    void slotSmartFill(int count);
    void skipNextButton(bool buttonChecked);
    void fadeNowButton(bool buttonChecked);
    void toggleAutoDJButton(bool enable);
    void autoDJError(AutoDJProcessor::AutoDJError error);
    void transitionTimeChanged(int time);
    void transitionSliderChanged(int value);
    void autoDJStateChanged(AutoDJProcessor::AutoDJState state);
    void updateSelectionInfo();
    void slotTransitionModeChanged(int comboboxIndex);
    void slotRepeatPlaylistChanged(bool checked);

  signals:
    void addRandomTrackButton(bool buttonChecked);
    void loadTrack(TrackPointer tio);
#ifdef __STEM__
    void loadTrackToPlayer(TrackPointer tio,
            const QString& group,
            mixxx::StemChannelSelection stemMask,
            bool);
#else
    void loadTrackToPlayer(TrackPointer tio, const QString& group, bool);
#endif
    void trackSelected(TrackPointer pTrack);

  private:
    void setupActionButton(QPushButton* pButton,
            void (DlgAutoDJ::*pSlot)(bool),
            const QString& fallbackText);
    void keyPressEvent(QKeyEvent* pEvent) override;

    const UserSettingsPointer m_pConfig;

    AutoDJProcessor* const m_pAutoDJProcessor;
    WTrackTableView* const m_pTrackTableView;
    const bool m_bShowButtonText;

    PlaylistTableModel* m_pAutoDJTableModel;

    QList<TrackId> selectedTrackIds() const;

    QString m_enableBtnTooltip;
    QString m_disableBtnTooltip;

    // Auto DJ 2.0 plus Video Mixing Live Assistant window.
    void showLiveAssistant();
    // force = recompute even if the live deck, track, tempo and key are
    // unchanged since the last time.
    void refreshLiveAssistant(bool force);
    QPointer<QDialog> m_pLiveAssistant;
    QLabel* m_pLiveNow = nullptr;
    QTableWidget* m_pLiveTable = nullptr;
    QLabel* m_pLiveStatus = nullptr;
    QTimer* m_pLiveTimer = nullptr;
    QString m_liveState;

    // Auto DJ 2.0 plus Video Mixing (created when first used).
    VideoManager* videoManager();
    QPointer<VideoManager> m_pVideo;
    /// Video: move the picture earlier or later than the sound.
    void showPictureTiming();
    /// Video: the DJ's name and/or logo on screen.
    void showVideoBrand();
    /// Video: record the mixed picture and sound as an MP4 file.
    void startVideoRecording();
    /// The Video button shows "REC" while recording.
    void updateVideoButton();
    /// Mixxx's REC button: while the video is showing, REC also records
    /// the video (an MP4 next to the sound file, same name).
    void recordingStatusChanged(double status);
    bool m_recHandled = false;      ///< this REC press was looked at
    bool m_recStartedVideo = false; ///< the video recording came from REC
    QPointer<QDialog> m_pPictureTiming;

    // Auto DJ 2.0 plus Video Mixing: stems split in the background.
    StemSplitter* m_pStems = nullptr;
    /// Splits the next songs of the Auto DJ queue ahead of time.
    void requestQueueStems();
    /// A song's parts are ready: a stopped deck holding it reloads it, so
    /// it plays from its parts (at the same position).
    void useReadyStems(TrackId trackId);
};
