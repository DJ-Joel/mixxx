#include "library/analysis/dlganalysis.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

#include "analyzer/analyzerprogress.h"
#include "analyzer/analyzerscheduledtrack.h"
#include "control/controlobject.h"
#include "library/analysis/ui_dlganalysis.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "track/track.h"
#include "moc_dlganalysis.cpp"
#include "util/assert.h"
#include "widget/wanalysislibrarytableview.h"
#include "widget/wlibrary.h"

namespace {
const char* kPreferenceGroup = "[Analysis]";
const char* kRecentDaysConfigKey = "RecentDays";
} // anonymous namespace

DlgAnalysis::DlgAnalysis(WLibrary* parent,
        UserSettingsPointer pConfig,
        Library* pLibrary)
        : QWidget(parent),
          m_pConfig(pConfig),
          m_bAnalysisActive(false),
          m_pLibrary(pLibrary),
          m_pGenreScanner(new GenreScanner(this)) {
    setupUi(this);
    m_songsButtonGroup.addButton(radioButtonRecentlyAdded);
    m_songsButtonGroup.addButton(radioButtonAllSongs);

    // Load the recent days value from config
    const unsigned int recentDays = m_pConfig->getValue<unsigned int>(
            ConfigKey(kPreferenceGroup, kRecentDaysConfigKey),
            AnalysisLibraryTableModel::kDefaultRecentDays);
    spinBoxRecentDays->setValue(static_cast<int>(recentDays));
    spinBoxRecentDays->setFocusPolicy(Qt::ClickFocus);

    // work around QLineEdit being protected
    QLineEdit* lineEditRecentDays(spinBoxRecentDays->findChild<QLineEdit*>());
    if (lineEditRecentDays) {
        lineEditRecentDays->setFocusPolicy(Qt::ClickFocus);
    }

    m_pAnalysisLibraryTableView = new WAnalysisLibraryTableView(
            this,
            pConfig,
            pLibrary,
            parent->getTrackTableBackgroundColorOpacity());
    connect(m_pAnalysisLibraryTableView,
            &WAnalysisLibraryTableView::loadTrack,
            this,
            &DlgAnalysis::loadTrack);
    connect(m_pAnalysisLibraryTableView,
            &WAnalysisLibraryTableView::loadTrackToPlayer,
            this,
            &DlgAnalysis::loadTrackToPlayer);

    connect(m_pAnalysisLibraryTableView,
            &WAnalysisLibraryTableView::trackSelected,
            this,
            &DlgAnalysis::trackSelected);

    QBoxLayout* box = qobject_cast<QBoxLayout*>(layout());
    VERIFY_OR_DEBUG_ASSERT(box) { // Assumes the form layout is a QVBox/QHBoxLayout!
    }
    else {
        box->removeWidget(m_pTrackTablePlaceholder);
        m_pTrackTablePlaceholder->hide();
        box->insertWidget(1, m_pAnalysisLibraryTableView);
    }

    m_pAnalysisLibraryTableModel = new AnalysisLibraryTableModel(
            this, pLibrary->trackCollectionManager());
    // Initialize model with the configured days value
    m_pAnalysisLibraryTableModel->setRecentDays(recentDays);
    m_pAnalysisLibraryTableView->loadTrackModel(m_pAnalysisLibraryTableModel);

    connect(radioButtonRecentlyAdded,
            &QRadioButton::clicked,
            this,
            &DlgAnalysis::slotShowRecentSongs);
    connect(radioButtonAllSongs,
            &QRadioButton::clicked,
            this,
            &DlgAnalysis::slotShowAllSongs);

    connect(spinBoxRecentDays,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &DlgAnalysis::slotRecentDaysChanged);
    // Don't click those radio buttons now reduce skin loading time.
    // 'RecentlyAdded' is clicked in onShow()

    connect(pushButtonAnalyze,
            &QPushButton::clicked,
            this,
            &DlgAnalysis::analyze);
    pushButtonAnalyze->setEnabled(false);

    connect(pushButtonSelectAll,
            &QPushButton::clicked,
            this,
            &DlgAnalysis::selectAll);

    // Auto DJ 2.0 plus Video Mixing Genre Scan.
    pushButtonGenreScan->setToolTip(tr(
            "Look up genres on MusicBrainz (an online music database) for the\n"
            "selected songs, or for all songs in the list if none are selected.\n"
            "You review every suggestion before anything is changed."));
    connect(pushButtonGenreScan, &QPushButton::clicked, this, &DlgAnalysis::slotGenreScan);
    connect(m_pGenreScanner,
            &GenreScanner::finished,
            this,
            &DlgAnalysis::slotGenreScanFinished);
    connect(m_pGenreScanner,
            &GenreScanner::progress,
            this,
            [this](int done, int total, const QString& current) {
                if (!m_pGenreProgress) {
                    return;
                }
                m_pGenreProgress->setMaximum(total);
                m_pGenreProgress->setValue(done);
                m_pGenreProgress->setLabelText(
                        tr("Looking up genres on MusicBrainz: %1 of %2\n%3")
                                .arg(done)
                                .arg(total)
                                .arg(current));
            });

    connect(m_pAnalysisLibraryTableView->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            &DlgAnalysis::tableSelectionChanged);

    connect(pLibrary,
            &Library::setTrackTableFont,
            m_pAnalysisLibraryTableView,
            &WAnalysisLibraryTableView::setTrackTableFont);
    connect(pLibrary,
            &Library::setTrackTableRowHeight,
            m_pAnalysisLibraryTableView,
            &WAnalysisLibraryTableView::setTrackTableRowHeight);
    connect(pLibrary,
            &Library::setSelectedClick,
            m_pAnalysisLibraryTableView,
            &WAnalysisLibraryTableView::setSelectedClick);

    slotAnalysisActive(m_bAnalysisActive);
}

DlgAnalysis::~DlgAnalysis() {
    qDebug() << "~DlgAnalysis()";

    // Delete m_pAnalysisLibraryTableView before the table models.
    // This is because the table view saves the header state using the model
    // in its destructor.
    delete m_pAnalysisLibraryTableView;
}

void DlgAnalysis::onShow() {
    if (!radioButtonRecentlyAdded->isChecked() &&
            !radioButtonAllSongs->isChecked()) {
        radioButtonRecentlyAdded->click();
    }
    // Refresh table
    // There might be new tracks dropped to other views
    m_pAnalysisLibraryTableModel->select();
}

bool DlgAnalysis::hasFocus() const {
    return m_pAnalysisLibraryTableView->hasFocus();
}

void DlgAnalysis::setFocus() {
    m_pAnalysisLibraryTableView->setFocus();
}

void DlgAnalysis::onSearch(const QString& text) {
    m_pAnalysisLibraryTableModel->searchCurrentTrackSet(
            text, radioButtonRecentlyAdded->isChecked());
}

void DlgAnalysis::tableSelectionChanged(const QItemSelection&,
        const QItemSelection&) {
    bool tracksSelected = m_pAnalysisLibraryTableView->selectionModel()->hasSelection();
    pushButtonAnalyze->setEnabled(tracksSelected || m_bAnalysisActive);
}

void DlgAnalysis::selectAll() {
    m_pAnalysisLibraryTableView->selectAll();
}

void DlgAnalysis::analyze() {
    // qDebug() << this << "analyze()";
    if (m_bAnalysisActive) {
        emit stopAnalysis();
    } else {
        QList<AnalyzerScheduledTrack> tracks;

        QModelIndexList selectedIndexes = m_pAnalysisLibraryTableView->selectionModel()->selectedRows();
        for (const auto& selectedIndex : std::as_const(selectedIndexes)) {
            TrackId trackId(m_pAnalysisLibraryTableModel->getFieldVariant(
                    selectedIndex, ColumnCache::COLUMN_LIBRARYTABLE_ID));
            if (trackId.isValid()) {
                tracks.append(trackId);
            }
        }
        emit analyzeTracks(tracks);
    }
}

void DlgAnalysis::slotGenreScan() {
    if (m_pGenreScanner->isRunning()) {
        return;
    }
    // The selected songs, or every song in the list.
    QModelIndexList rows = m_pAnalysisLibraryTableView->selectionModel()->selectedRows();
    const bool selection = !rows.isEmpty();
    if (!selection) {
        for (int row = 0; row < m_pAnalysisLibraryTableModel->rowCount(); ++row) {
            rows.append(m_pAnalysisLibraryTableModel->index(row, 0));
        }
    }
    QList<GenreScanner::Item> all;
    int withoutGenre = 0;
    for (const QModelIndex& index : std::as_const(rows)) {
        GenreScanner::Item item;
        item.id = TrackId(m_pAnalysisLibraryTableModel->getFieldVariant(
                index, ColumnCache::COLUMN_LIBRARYTABLE_ID));
        if (!item.id.isValid()) {
            continue;
        }
        item.artist = m_pAnalysisLibraryTableModel
                              ->getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_ARTIST)
                              .toString();
        item.title = m_pAnalysisLibraryTableModel
                             ->getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_TITLE)
                             .toString();
        item.currentGenre = m_pAnalysisLibraryTableModel
                                    ->getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_GENRE)
                                    .toString();
        if (genrescan::isEmptyGenre(item.currentGenre)) {
            ++withoutGenre;
        }
        all.append(item);
    }
    if (all.isEmpty()) {
        QMessageBox::information(this, tr("Genre Scan"), tr("There are no songs in the list."));
        return;
    }

    QMessageBox ask(QMessageBox::Question,
            tr("Genre Scan"),
            tr("Genre Scan looks up %1 on MusicBrainz, an online music database, "
               "one request per second.\n\n"
               "Nothing is changed until you have reviewed the results.")
                    .arg(selection ? tr("the %1 selected songs").arg(all.size())
                                   : tr("all %1 songs in the list").arg(all.size())),
            QMessageBox::NoButton,
            this);
    auto* pOnlyEmpty = new QCheckBox(
            tr("Only songs without a genre (%1 of them): empty, Other or Unknown")
                    .arg(withoutGenre),
            &ask);
    pOnlyEmpty->setChecked(true);
    ask.setCheckBox(pOnlyEmpty);
    QPushButton* pStart = ask.addButton(tr("Start"), QMessageBox::AcceptRole);
    ask.addButton(QMessageBox::Cancel);
    ask.exec();
    if (ask.clickedButton() != pStart) {
        return;
    }
    QList<GenreScanner::Item> items;
    for (const GenreScanner::Item& item : std::as_const(all)) {
        if (!pOnlyEmpty->isChecked() || genrescan::isEmptyGenre(item.currentGenre)) {
            items.append(item);
        }
    }
    if (items.isEmpty()) {
        QMessageBox::information(this, tr("Genre Scan"), tr("All these songs already have a genre."));
        return;
    }

    // Up to 3 requests per song (search, song, artist), 1 per second.
    const int minutes = std::max(1, static_cast<int>(std::ceil(items.size() * 2.5 / 60.0)));
    m_pGenreProgress = new QProgressDialog(
            tr("Starting... (about %1 minutes)").arg(minutes), tr("Stop"), 0, static_cast<int>(items.size()), this);
    m_pGenreProgress->setWindowTitle(tr("Genre Scan"));
    m_pGenreProgress->setWindowModality(Qt::NonModal);
    m_pGenreProgress->setMinimumDuration(0);
    m_pGenreProgress->setAutoClose(false);
    m_pGenreProgress->setAutoReset(false);
    connect(m_pGenreProgress, &QProgressDialog::canceled, m_pGenreScanner, &GenreScanner::cancel);
    m_pGenreProgress->show();
    pushButtonGenreScan->setEnabled(false);
    m_pGenreScanner->start(items);
}

void DlgAnalysis::slotGenreScanFinished(
        const QList<GenreScanner::Result>& results, bool cancelled) {
    pushButtonGenreScan->setEnabled(true);
    if (m_pGenreProgress) {
        m_pGenreProgress->hide();
        m_pGenreProgress->deleteLater();
        m_pGenreProgress = nullptr;
    }
    if (results.isEmpty()) {
        return;
    }
    int found = 0;
    for (const auto& result : results) {
        if (!result.suggested.isEmpty()) {
            ++found;
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Genre Scan results"));
    auto* pLayout = new QVBoxLayout(&dialog);
    auto* pSummary = new QLabel(
            tr("Found a genre for %1 of %2 songs%3.\n"
               "MusicBrainz genres are votes by its users, so please check them. "
               "Tick the ones to apply. To change a suggestion, double-click it and type.")
                    .arg(found)
                    .arg(results.size())
                    .arg(cancelled ? tr(" (scan stopped early)") : QString()),
            &dialog);
    pSummary->setWordWrap(true);
    pLayout->addWidget(pSummary);

    auto* pTable = new QTableWidget(static_cast<int>(results.size()), 5, &dialog);
    pTable->setHorizontalHeaderLabels(
            {tr("Apply"), tr("Song"), tr("Genre now"), tr("Suggested"), tr("From")});
    pTable->verticalHeader()->hide();
    // Songs with a suggestion first, then the ones without.
    QList<GenreScanner::Result> ordered;
    for (const auto& result : results) {
        if (!result.suggested.isEmpty()) {
            ordered.append(result);
        }
    }
    for (const auto& result : results) {
        if (result.suggested.isEmpty()) {
            ordered.append(result);
        }
    }
    for (int row = 0; row < static_cast<int>(ordered.size()); ++row) {
        const GenreScanner::Result& result = ordered[row];
        const bool hasSuggestion = !result.suggested.isEmpty();
        auto* pApply = new QTableWidgetItem();
        pApply->setData(Qt::UserRole, result.item.id.toVariant());
        if (hasSuggestion) {
            pApply->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            // Ticked when the song has no genre yet; a genre the DJ set is
            // only replaced when they tick it.
            pApply->setCheckState(genrescan::isEmptyGenre(result.item.currentGenre)
                            ? Qt::Checked
                            : Qt::Unchecked);
        } else {
            pApply->setFlags(Qt::ItemIsEnabled);
        }
        pTable->setItem(row, 0, pApply);
        const QString song = result.item.artist.isEmpty()
                ? result.item.title
                : result.item.artist + QStringLiteral(" - ") + result.item.title;
        const auto readOnly = [](const QString& text) {
            auto* pItem = new QTableWidgetItem(text);
            pItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            return pItem;
        };
        pTable->setItem(row, 1, readOnly(song));
        pTable->setItem(row, 2, readOnly(result.item.currentGenre));
        auto* pSuggested = new QTableWidgetItem(hasSuggestion ? result.suggested : QString());
        pSuggested->setFlags(hasSuggestion
                        ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                        : Qt::ItemIsEnabled);
        pTable->setItem(row, 3, pSuggested);
        pTable->setItem(row,
                4,
                readOnly(hasSuggestion
                                ? tr("%1 (%n vote(s))", "", result.votes)
                                          .arg(result.source == QStringLiteral("song")
                                                          ? tr("this song")
                                                          : tr("the artist"))
                                : result.note));
    }
    pTable->resizeColumnsToContents();
    pTable->horizontalHeader()->setStretchLastSection(true);
    pLayout->addWidget(pTable);

    auto* pButtons = new QDialogButtonBox(&dialog);
    QPushButton* pApplyButton =
            pButtons->addButton(tr("Apply ticked"), QDialogButtonBox::AcceptRole);
    QPushButton* pTickAll = pButtons->addButton(tr("Tick all"), QDialogButtonBox::ActionRole);
    QPushButton* pUntickAll = pButtons->addButton(tr("Untick all"), QDialogButtonBox::ActionRole);
    pButtons->addButton(QDialogButtonBox::Cancel);
    const auto setAll = [pTable](Qt::CheckState state) {
        for (int row = 0; row < pTable->rowCount(); ++row) {
            QTableWidgetItem* pItem = pTable->item(row, 0);
            if (pItem && (pItem->flags() & Qt::ItemIsUserCheckable)) {
                pItem->setCheckState(state);
            }
        }
    };
    connect(pTickAll, &QPushButton::clicked, &dialog, [setAll]() {
        setAll(Qt::Checked);
    });
    connect(pUntickAll, &QPushButton::clicked, &dialog, [setAll]() {
        setAll(Qt::Unchecked);
    });
    connect(pApplyButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(pButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    pLayout->addWidget(pButtons);
    dialog.resize(1000, 640);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    int applied = 0;
    for (int row = 0; row < pTable->rowCount(); ++row) {
        QTableWidgetItem* pApply = pTable->item(row, 0);
        if (!pApply || !(pApply->flags() & Qt::ItemIsUserCheckable) ||
                pApply->checkState() != Qt::Checked) {
            continue;
        }
        const QString genre = pTable->item(row, 3)->text().trimmed();
        if (genre.isEmpty()) {
            continue;
        }
        const TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(
                TrackId(pApply->data(Qt::UserRole)));
        if (pTrack) {
            pTrack->updateGenre(genre); // saved when the track is released
            ++applied;
        }
    }
    m_pAnalysisLibraryTableModel->select();
    QMessageBox::information(this,
            tr("Genre Scan"),
            tr("Updated the genre of %n song(s).", "", applied));
}

void DlgAnalysis::slotAnalysisActive(bool bActive) {
    // qDebug() << this << "slotAnalysisActive" << bActive;
    m_bAnalysisActive = bActive;
    if (bActive) {
        pushButtonAnalyze->setChecked(true);
        pushButtonAnalyze->setText(tr("Stop Analysis"));
        pushButtonAnalyze->setEnabled(true);
        labelProgress->setEnabled(true);
    } else {
        pushButtonAnalyze->setChecked(false);
        pushButtonAnalyze->setText(tr("Analyze"));
        labelProgress->setText("");
        labelProgress->setEnabled(false);
    }
}

void DlgAnalysis::onTrackAnalysisSchedulerProgress(
        AnalyzerProgress, int finishedCount, int totalCount) {
    // qDebug() << this << "onTrackAnalysisSchedulerProgress" <<
    // analyzerProgress << finishedCount << totalCount;
    if (labelProgress->isEnabled()) {
        int totalProgressPercent = 0;
        if (totalCount > 0) {
            totalProgressPercent = (finishedCount * 100) / totalCount;
            if (totalProgressPercent > 100) {
                totalProgressPercent = 100;
            }
        }

        labelProgress->setText(tr("Analyzing %1/%2")
                                       .arg(QString::number(finishedCount),
                                               QString::number(totalCount)) +
                QStringLiteral(" (%3%)").arg(
                        QString::number(totalProgressPercent)));
    }
}

void DlgAnalysis::onTrackAnalysisSchedulerFinished() {
    slotAnalysisActive(false);
}

void DlgAnalysis::slotShowRecentSongs() {
    spinBoxRecentDays->setEnabled(true);
    labelRecentDaysAppendix->setEnabled(true);
    m_pAnalysisLibraryTableModel->showRecentSongs();
}

void DlgAnalysis::slotRecentDaysChanged(int days) {
    // Update the model's days value
    m_pAnalysisLibraryTableModel->setRecentDays(static_cast<unsigned int>(days));

    // Save to config
    m_pConfig->setValue(
            ConfigKey(kPreferenceGroup, kRecentDaysConfigKey),
            static_cast<unsigned int>(days));

    // If "Recently Added" is selected, refresh the view
    if (radioButtonRecentlyAdded->isChecked()) {
        m_pAnalysisLibraryTableModel->showRecentSongs();
    }
}

void DlgAnalysis::slotShowAllSongs() {
    spinBoxRecentDays->setEnabled(false);
    labelRecentDaysAppendix->setEnabled(false);
    m_pAnalysisLibraryTableModel->showAllSongs();
}

void DlgAnalysis::keyPressEvent(QKeyEvent* pEvent) {
    // If we receive key events the spinbox is focused.
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

void DlgAnalysis::installEventFilter(QObject* pFilter) {
    QWidget::installEventFilter(pFilter);
    m_pAnalysisLibraryTableView->installEventFilter(pFilter);
}

void DlgAnalysis::saveCurrentViewState() {
    m_pAnalysisLibraryTableView->saveCurrentViewState();
}

bool DlgAnalysis::restoreCurrentViewState() {
    return m_pAnalysisLibraryTableView->restoreCurrentViewState();
}
