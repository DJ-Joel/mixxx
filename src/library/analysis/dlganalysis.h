#pragma once

#include <QButtonGroup>

#include "analyzer/analyzerprogress.h"
#include "analyzer/analyzerscheduledtrack.h"
#include "library/analysis/analysislibrarytablemodel.h"
#include "library/analysis/ui_dlganalysis.h"
#include "library/autodj/genrescanner.h"
#include "library/libraryview.h"
#include "preferences/usersettings.h"

class Library;
class WAnalysisLibraryTableView;
class WLibrary;
class QItemSelection;
class QProgressDialog;

class DlgAnalysis : public QWidget, public Ui::DlgAnalysis, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgAnalysis(WLibrary* parent,
            UserSettingsPointer pConfig,
            Library* pLibrary);
    ~DlgAnalysis() override;

    void onSearch(const QString& text) override;
    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;
    inline const QString currentSearch() {
        return m_pAnalysisLibraryTableModel->currentSearch();
    }
    void saveCurrentViewState() override;
    bool restoreCurrentViewState() override;

  public slots:
    void tableSelectionChanged(const QItemSelection& selected,
            const QItemSelection& deselected);
    void selectAll();
    void analyze();
    void slotAnalysisActive(bool bActive);
    void onTrackAnalysisSchedulerProgress(AnalyzerProgress analyzerProgress, int finishedCount, int totalCount);
    void onTrackAnalysisSchedulerFinished();
    void slotShowRecentSongs();
    void slotRecentDaysChanged(int days);
    void slotShowAllSongs();
    void installEventFilter(QObject* pFilter);
    /// Auto DJ 2.0 plus Video Mixing Genre Scan: look up genres on MusicBrainz, then review.
    void slotGenreScan();
    /// Auto DJ 2.0 plus Video Mixing: split songs into parts ahead of time.
    void slotStemSplit();
    void slotGenreScanFinished(const QList<GenreScanner::Result>& results, bool cancelled);

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& player);
    void analyzeTracks(const QList<AnalyzerScheduledTrack>& tracks);
    void stopAnalysis();
    void trackSelected(TrackPointer pTrack);

  private:
    void keyPressEvent(QKeyEvent* pEvent) override;
    // Note m_pTrackTablePlaceholder is defined in the .ui file
    UserSettingsPointer m_pConfig;
    bool m_bAnalysisActive;
    QButtonGroup m_songsButtonGroup;
    WAnalysisLibraryTableView* m_pAnalysisLibraryTableView;
    AnalysisLibraryTableModel* m_pAnalysisLibraryTableModel;
    Library* m_pLibrary;
    GenreScanner* m_pGenreScanner;
    QProgressDialog* m_pGenreProgress = nullptr;
    bool m_stemSplitConnected = false;
};
