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
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QGuiApplication>

#include "analyzer/analyzerbeatcheck.h"
#include "analyzer/analyzerenergy.h"
#include "analyzer/energycalculator.h"
#include "analyzer/analyzerprogress.h"
#include "library/autodj/smart/beatcheck.h"
#include "library/autodj/smart/energystore.h"
#include "track/beats.h"
#include "track/cue.h"
#include "analyzer/analyzerscheduledtrack.h"
#include "control/controlobject.h"
#include "library/analysis/ui_dlganalysis.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "stems/stemcache.h"
#include "stems/stemsplitter.h"
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
    pushButtonStemSplit->setToolTip(tr(
            "Split the selected songs (or all songs in the list if none are selected)\n"
            "into drums, bass, other and vocals now, in the background, so they load\n"
            "with their parts ready. Songs you load into a deck meanwhile go first.\n"
            "Click again to stop. Where the parts are saved: Auto DJ > Stems."));
    connect(pushButtonStemSplit, &QPushButton::clicked, this, &DlgAnalysis::slotStemSplit);
    pushButtonBeatCheck->setToolTip(tr(
            "Songs whose tempo, beat lines or beat 1 look wrong to a second beat finder\n"
            "(\"Beat This!\"), which listens to every song during Analyze.\n"
            "You choose Fix or Ignore for each one; nothing changes without that,\n"
            "and every Fix can be undone."));
    connect(pushButtonBeatCheck, &QPushButton::clicked, this, &DlgAnalysis::slotBeatCheck);
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

namespace {

QString hoursAndMinutes(double seconds) {
    const int minutes = static_cast<int>(std::ceil(seconds / 60.0));
    if (minutes < 60) {
        return QObject::tr("%1 min").arg(std::max(1, minutes));
    }
    return QObject::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

} // namespace

void DlgAnalysis::slotStemSplit() {
    StemSplitter* pSplitter = StemSplitter::instance();
    if (!pSplitter) {
        return;
    }
    if (!m_stemSplitConnected) {
        m_stemSplitConnected = true;
        connect(pSplitter,
                &StemSplitter::batchProgress,
                this,
                [this](int done, int total, double secondsLeft) {
                    if (total <= 0 || done >= total) {
                        pushButtonStemSplit->setText(tr("Stem Split"));
                        StemSplitter* pSplitter = StemSplitter::instance();
                        const QStringList failures =
                                pSplitter ? pSplitter->takeBatchFailures() : QStringList();
                        if (!failures.isEmpty()) {
                            QMessageBox::information(this,
                                    tr("Stem Split"),
                                    tr("%1 of the songs could not be split:\n\n%2")
                                            .arg(failures.size())
                                            .arg(failures.mid(0, 15).join(QStringLiteral("\n"))));
                        }
                        return;
                    }
                    pushButtonStemSplit->setText(tr("Stem Split %1/%2 - %3 left")
                                    .arg(done)
                                    .arg(total)
                                    .arg(hoursAndMinutes(secondsLeft)));
                });
    }
    if (pSplitter->batchRunning()) {
        if (QMessageBox::question(this,
                    tr("Stem Split"),
                    tr("Stop splitting the songs of the list? Parts already made are kept.")) ==
                QMessageBox::Yes) {
            pSplitter->stopBatch();
            pushButtonStemSplit->setText(tr("Stem Split"));
        }
        return;
    }
    if (!pSplitter->engineInstalled()) {
        QMessageBox::information(this,
                tr("Stem Split"),
                tr("The stems engine is not installed, so songs cannot be split into "
                   "parts.\n\nIt belongs in:\n%1")
                        .arg(QDir::toNativeSeparators(pSplitter->engineFolder())));
        return;
    }
    if (!pSplitter->isEnabled()) {
        QMessageBox::information(this,
                tr("Stem Split"),
                tr("Splitting songs into parts is switched off (Auto DJ > Stems)."));
        return;
    }
    // The selected songs, or every song in the list.
    QModelIndexList rows = m_pAnalysisLibraryTableView->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        for (int row = 0; row < m_pAnalysisLibraryTableModel->rowCount(); ++row) {
            rows.append(m_pAnalysisLibraryTableModel->index(row, 0));
        }
    }
    QList<TrackPointer> tracks;
    double seconds = 0.0;
    int longSkipped = 0;
    for (const QModelIndex& index : std::as_const(rows)) {
        TrackPointer pTrack = m_pAnalysisLibraryTableModel->getTrack(index);
        if (StemSplitter::needsSplit(pTrack)) {
            // A whole DJ mix (over 20 minutes) only when chosen on its own.
            if (rows.size() > 1 && StemSplitter::isLong(pTrack)) {
                ++longSkipped;
                continue;
            }
            seconds += std::max(0.0, pTrack->getDuration());
            tracks.append(pTrack);
        }
    }
    if (longSkipped > 0) {
        qInfo().noquote() << "Stems: Stem Split skips" << longSkipped
                          << "songs longer than 20 minutes";
    }
    if (tracks.isEmpty()) {
        QMessageBox::information(this,
                tr("Stem Split"),
                tr("Nothing to split: these %1 songs already have their parts, or cannot "
                   "be split (only 44.1 and 48 kHz songs can).")
                        .arg(rows.size()));
        return;
    }
    // Time and space, from this computer's own speed when known.
    double speed = pSplitter->speed();
    if (speed <= 0.0) {
        speed = QDir(QDir(pSplitter->engineFolder()).filePath(QStringLiteral("cuda"))).exists()
                ? 15.0
                : 3.0;
    }
    constexpr double kBytesPerSecond = 5 * 24000.0; // five AAC tracks of 192 kbit/s
    const double bytes = seconds * kBytesPerSecond;
    const QString target = stems::StemCache::fileFor(tracks.first());
    QString drive = QFileInfo(target).absolutePath();
    while (!drive.isEmpty() && !QFileInfo::exists(drive)) {
        drive = QFileInfo(drive).absolutePath() == drive ? QString() : QFileInfo(drive).absolutePath();
    }
    const QStorageInfo storage(drive);
    const double freeBytes = storage.isValid() ? static_cast<double>(storage.bytesAvailable()) : -1.0;
    const QString where = stems::StemCache::location() == stems::StemCache::Location::NextToSong
            ? tr("next to each song")
            : QDir::toNativeSeparators(QFileInfo(target).absolutePath());
    if (freeBytes >= 0.0 && bytes > freeBytes * 0.95) {
        QMessageBox::warning(this,
                tr("Stem Split"),
                tr("The parts of these %1 songs need about %2 GB, but only %3 GB is free "
                   "there (%4).\n\nChoose another folder in Auto DJ > Stems, or select "
                   "fewer songs.")
                        .arg(tracks.size())
                        .arg(bytes / 1e9, 0, 'f', 1)
                        .arg(freeBytes / 1e9, 0, 'f', 1)
                        .arg(where));
        return;
    }
    const QString question =
            tr("Stem Split will split %1 songs (%2 of music) into drums, bass, other and "
               "vocals.\n\nIt takes about %3 and needs about %4 GB%5.\nSaved: %6\n\n"
               "You can keep using Mixxx meanwhile; songs you load into a deck go first. "
               "Click Stem Split again to stop.%7")
                    .arg(tracks.size())
                    .arg(hoursAndMinutes(seconds))
                    .arg(hoursAndMinutes(seconds / speed))
                    .arg(bytes / 1e9, 0, 'f', 1)
                    .arg(freeBytes >= 0.0
                                    ? tr(" (%1 GB free)").arg(freeBytes / 1e9, 0, 'f', 1)
                                    : QString())
                    .arg(where)
                    .arg(longSkipped > 0
                                    ? tr("\n\n%1 songs longer than 20 minutes (DJ mixes?) are "
                                         "left out; select one on its own to split it.")
                                              .arg(longSkipped)
                                    : QString());
    if (QMessageBox::question(this, tr("Stem Split"), question) != QMessageBox::Yes) {
        return;
    }
    const int added = pSplitter->splitBatch(tracks);
    qInfo().noquote() << "Stems: Stem Split started for" << added << "songs";
    if (added <= 0) {
        // Already being split (loaded in a deck, from the track menu, ...).
        QMessageBox::information(this,
                tr("Stem Split"),
                tr("These songs are already being split. Their parts will be ready soon."));
        return;
    }
    pushButtonStemSplit->setText(tr("Stem Split 0/%1 - %2 left")
                    .arg(added)
                    .arg(hoursAndMinutes(seconds / speed)));
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

namespace {
QString minutesSeconds(double sec) {
    const double s = std::max(0.0, sec);
    const int minutes = static_cast<int>(s / 60.0);
    return QStringLiteral("%1:%2").arg(minutes).arg(s - minutes * 60.0, 5, 'f', 2, QLatin1Char('0'));
}

// What the DJ ignored: the same answer again stays ignored, a different
// one (the grid or the marker changed since) is shown again.
QString beatCheckSignature(const beatcheck::Review& r) {
    return QStringLiteral("1|%1%2%3%4%5%8|%6|%7")
            .arg(r.tempoWrong ? 1 : 0)
            .arg(r.introMisread ? 1 : 0)
            .arg(r.linesOff ? 1 : 0)
            .arg(r.halfBeat ? 1 : 0)
            .arg(r.beatOneOff ? 1 : 0)
            .arg(qRound(r.newGrid ? r.newBpm * 10.0 : 0.0))
            .arg(qRound(r.newIntroEndSec * 20.0))
            .arg(r.firstVersionMap ? 1 : 0);
}

QByteArray beatsHash(const mixxx::BeatsPointer& pBeats) {
    return pBeats ? QCryptographicHash::hash(pBeats->toByteArray(), QCryptographicHash::Md5)
                  : QByteArray();
}

QString songName(const TrackPointer& pTrack) {
    return pTrack->getArtist().isEmpty()
            ? pTrack->getTitle()
            : pTrack->getArtist() + QStringLiteral(" - ") + pTrack->getTitle();
}

// The song is still as a beat check Fix left it (nothing changed since):
// it is not shown again.
bool stillAsFixed(const EnergyStore::BeatCheckUndo& undo, const TrackPointer& pTrack) {
    if (undo.gridSaved && beatsHash(pTrack->getBeats()) != undo.newBeatsHash) {
        return false;
    }
    if (undo.introSaved) {
        const CuePointer pIntro = pTrack->findCueByType(mixxx::CueType::Intro);
        const mixxx::audio::FramePos end =
                pIntro ? pIntro->getEndPosition() : mixxx::audio::kInvalidFramePos;
        if (!end.isValid() || std::fabs(end.value() - undo.newIntroEndFrame) > 2.0) {
            return false;
        }
    }
    return undo.gridSaved || undo.introSaved;
}

// The proposed grid as the beat grid check sees it.
phrasealign::Grid proposedGrid(const beatcheck::Review& r) {
    if (r.steady) {
        phrasealign::Grid grid;
        grid.firstBeatSec = r.newFirstBeatSec;
        grid.beatSec = r.newBpm > 0.0 ? 60.0 / r.newBpm : 0.0;
        return grid;
    }
    return phrasealign::Grid::fromBeats(r.newBeats);
}
} // namespace

void DlgAnalysis::slotBeatCheck() {
    TrackCollectionManager* pManager = m_pLibrary->trackCollectionManager();
    if (!pManager || !pManager->internalCollection()) {
        return;
    }
    const QSqlDatabase db = pManager->internalCollection()->database();
    const QList<TrackId> ids = EnergyStore::beatCheckedTracks(db);
    const bool anyUndo = !EnergyStore::beatCheckUndoTracks(db).isEmpty();
    if (ids.isEmpty() && !anyUndo) {
        const bool ready = QFileInfo::exists(AnalyzerBeatCheck::runtimeFile(m_pConfig));
        QMessageBox::information(this,
                tr("Beat Check"),
                ready ? tr("No song has been checked yet. The beat check listens to each song "
                           "during Analyze: select the songs (or all of them) and press "
                           "Analyze, then come back here.")
                      : tr("The beat check needs ONNX Runtime, which comes with the stems "
                           "engine (Auto DJ > Stems), and it was not found:\n%1")
                                .arg(QDir::toNativeSeparators(
                                        AnalyzerBeatCheck::runtimeFile(m_pConfig))));
        return;
    }

    struct Row {
        TrackPointer pTrack;
        beatcheck::Review review;
        QString signature;
        QString problem;
        QString fix;
        double driftBeats = -1.0; ///< the proposed grid in the beat grid check
        int order = 0;
    };
    auto rows = std::make_shared<std::vector<Row>>();
    int checked = 0;
    int ignored = 0;
    int fine = 0;
    int fixedBefore = 0;
    int needLevels = 0;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    for (const TrackId& id : ids) {
        const TrackPointer pTrack = pManager->getTrackById(id);
        const auto stored = EnergyStore::loadBeatCheck(db, id);
        if (!pTrack || !stored) {
            continue;
        }
        // Fixed by the beat check and not changed since: done, unless the
        // beat check now has a better grid for it (a first-version map).
        const auto undo = EnergyStore::loadBeatCheckUndo(db, id);
        const bool asFixed = undo && stillAsFixed(*undo, pTrack);
        const double sampleRate = pTrack->getSampleRate().value();
        beatcheck::Song song;
        song.grid = AnalyzerEnergy::beatGrid(pTrack);
        song.durationSec = pTrack->getDuration();
        song.bpmLocked = pTrack->isBpmLocked();
        const CuePointer pIntro = pTrack->findCueByType(mixxx::CueType::Intro);
        const mixxx::audio::FramePos introEnd =
                pIntro ? pIntro->getEndPosition() : mixxx::audio::kInvalidFramePos;
        if (introEnd.isValid() && sampleRate > 0.0) {
            song.introEndSec = introEnd.value() / sampleRate;
            const auto autoMarkers = EnergyStore::loadAutoMarkers(db, id);
            song.introEndByDj = !(autoMarkers.introEndSec >= 0.0 &&
                    std::fabs(autoMarkers.introEndSec - song.introEndSec) < 0.02);
        }
        beatcheck::Found found;
        found.beats = stored->beats;
        found.downbeats = stored->downbeats;
        beatcheck::Review r = beatcheck::review(found, song);
        if (!r.checked) {
            continue;
        }
        if (asFixed && !r.newGrid) {
            ++fixedBefore;
            continue;
        }
        ++checked;
        if (!r.anyProblem()) {
            ++fine;
            continue;
        }
        // A new grid is only offered once it has passed the beat grid check
        // (the same one Auto DJ uses): it must stay on the music.
        QString gridNote;
        double drift = -1.0;
        if (r.newGrid) {
            if (!stored->hasLevels()) {
                ++needLevels;
                gridNote = tr("A new grid is offered once it has been tried against the "
                              "music: press Analyze once more (quick for songs already "
                              "checked).");
            } else {
                const auto tryGrid = [&]() {
                    return EnergyCalculator::gridDriftBeats(stored->levels,
                            proposedGrid(r),
                            stored->bodyStartSec,
                            stored->bodyEndSec);
                };
                drift = tryGrid();
                if (drift > EnergyCalculator::kGridMaxDriftBeats && r.introOnly) {
                    // Mixxx's own main part drifts too (Cabaret Voltaire):
                    // the grid from the beats heard instead, if that fits.
                    song.allowIntroOnly = false;
                    r = beatcheck::review(found, song);
                    drift = r.newGrid ? tryGrid() : -1.0;
                }
                if (drift > EnergyCalculator::kGridMaxDriftBeats) {
                    gridNote = tr("The beat check's own grid did not stay on the music "
                                  "either (%1 beats off), so it is not offered.")
                                       .arg(drift, 0, 'f', 2);
                }
            }
            if (!gridNote.isEmpty()) {
                song.allowNewGrid = false;
                r = beatcheck::review(found, song);
            }
        }
        if (asFixed && !r.newGrid) {
            ++fixedBefore; // its new grid did not pass: as the Fix left it
            continue;
        }
        Row row;
        row.pTrack = pTrack;
        row.review = r;
        row.driftBeats = drift;
        row.signature = beatCheckSignature(r);
        if (stored->ignored == row.signature) {
            ++ignored;
            continue;
        }
        QStringList problems;
        if (r.tempoWrong) {
            problems << tr("Tempo: Mixxx says %1 BPM, the beat check hears %2 BPM.")
                                .arg(r.mixxxBpm, 0, 'f', 1)
                                .arg(r.songBpm, 0, 'f', 1);
        }
        if (r.introMisread) {
            problems << tr("Intro read at %1 BPM; the beat check hears the song's %2 BPM there "
                           "too.")
                                .arg(r.introBpm, 0, 'f', 1)
                                .arg(r.songBpm, 0, 'f', 1);
        }
        if (r.linesOff && r.halfBeat) {
            problems << tr("Beat lines half a beat from the beat check's in %1% of the song. "
                           "Listen first: one of the two is on the off-beat.")
                                .arg(qRound((1.0 - r.linesOnBeat) * 100.0));
        } else if (r.linesOff) {
            problems << tr("Beat lines: only %1% are on the beat.")
                                .arg(qRound(r.linesOnBeat * 100.0));
        }
        if (r.firstVersionMap) {
            problems << tr("Beat map from the beat check's first version: it went through stray "
                           "beats, so its start can be at a made-up tempo.");
        }
        if (r.beatOneOff) {
            const double beats = std::fabs(r.beatOneOffBeats);
            problems << (r.beatOneOffBeats > 0.0
                                        ? tr("Beat 1: the Intro End marker (%1) is %2 beat(s) "
                                             "after beat 1.")
                                        : tr("Beat 1: the Intro End marker (%1) is %2 beat(s) "
                                             "before beat 1."))
                                .arg(minutesSeconds(song.introEndSec))
                                .arg(beats, 0, 'f', 1);
        }
        row.problem = problems.join(QChar('\n'));
        QStringList fixes;
        if (r.newGrid) {
            const QString what = r.introOnly
                    ? (r.steady ? tr("New beat grid at %1 BPM: Mixxx's own beats kept, the "
                                     "intro redone (BPM locked).")
                                          .arg(r.newBpm, 0, 'f', 2)
                                : tr("Beat map: Mixxx's own beats kept, the intro redone at "
                                     "about %1 BPM (BPM locked).")
                                          .arg(r.newBpm, 0, 'f', 1))
                    : (r.steady ? tr("New beat grid at %1 BPM from the beat check (BPM locked).")
                                          .arg(r.newBpm, 0, 'f', 2)
                                : tr("New beat map from the beat check, about %1 BPM (BPM "
                                     "locked).")
                                          .arg(r.newBpm, 0, 'f', 1));
            fixes << what;
            if (r.replacesLock) {
                fixes << tr("You locked this BPM: Fix replaces your grid anyway (\"Fix all\" "
                            "leaves it; Undo puts it back).");
            }
        }
        if (r.newIntroEndSec >= 0.0) {
            fixes << tr("Intro End marker %1 -> %2.")
                             .arg(minutesSeconds(song.introEndSec))
                             .arg(minutesSeconds(r.newIntroEndSec));
        }
        if (!r.anyFix()) {
            fixes << (!gridNote.isEmpty()
                            ? tr("Nothing.")
                            : (song.introEndByDj && r.beatOneOff
                                            ? tr("Nothing: you set the Intro End marker "
                                                 "yourself.")
                                            : tr("Nothing.")));
        }
        if (!gridNote.isEmpty()) {
            fixes << gridNote;
        }
        row.fix = fixes.join(QChar('\n'));
        row.order = r.anyFix() ? ((r.halfBeat || r.replacesLock) ? 1 : 0) : 2;
        rows->push_back(row);
    }
    QGuiApplication::restoreOverrideCursor();
    std::stable_sort(rows->begin(), rows->end(), [](const Row& a, const Row& b) {
        return a.order < b.order;
    });

    auto* pDialog = new QDialog(this);
    pDialog->setAttribute(Qt::WA_DeleteOnClose);
    pDialog->setWindowTitle(tr("Beat Check"));
    auto* pLayout = new QVBoxLayout(pDialog);
    QString summary =
            tr("%1 songs checked: %2 look right, %3 already fixed by the beat check, %4 to "
               "look at (%5 ignored before). %6 songs in the library have not been checked "
               "yet: they are checked during Analyze.")
                    .arg(checked + fixedBefore)
                    .arg(fine)
                    .arg(fixedBefore)
                    .arg(rows->size())
                    .arg(ignored)
                    .arg(std::max(0,
                            m_pAnalysisLibraryTableModel->rowCount() - static_cast<int>(ids.size())));
    if (needLevels > 0) {
        summary += QChar('\n') +
                tr("%1 new grids wait to be tried against the music: press Analyze once more "
                   "(quick: the songs are not listened to again).")
                        .arg(needLevels);
    }
    summary += QChar('\n') +
            tr("Choose Fix or Ignore for each song, then press Do it. Double-click a song to "
               "load it on a deck and listen first. Every Fix can be undone (Undo fixes).");
    auto* pSummary = new QLabel(summary, pDialog);
    pSummary->setWordWrap(true);
    pLayout->addWidget(pSummary);

    auto* pTable = new QTableWidget(static_cast<int>(rows->size()), 4, pDialog);
    pTable->setHorizontalHeaderLabels({tr("Choice"), tr("Song"), tr("What looks wrong"), tr("Fix does")});
    pTable->verticalHeader()->hide();
    pTable->setWordWrap(true);
    pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int i = 0; i < static_cast<int>(rows->size()); ++i) {
        const Row& row = (*rows)[i];
        auto* pChoice = new QComboBox(pTable);
        pChoice->addItem(tr("Decide later"), 0);
        if (row.review.anyFix()) {
            pChoice->addItem(tr("Fix"), 1);
        }
        pChoice->addItem(tr("Ignore"), 2);
        pTable->setCellWidget(i, 0, pChoice);
        pTable->setItem(i, 1, new QTableWidgetItem(songName(row.pTrack)));
        pTable->setItem(i, 2, new QTableWidgetItem(row.problem));
        pTable->setItem(i, 3, new QTableWidgetItem(row.fix));
    }
    pTable->resizeColumnsToContents();
    pTable->setColumnWidth(1, std::min(pTable->columnWidth(1), 280));
    pTable->setColumnWidth(2, std::min(std::max(pTable->columnWidth(2), 200), 380));
    pTable->horizontalHeader()->setStretchLastSection(true);
    pTable->resizeRowsToContents();
    connect(pTable, &QTableWidget::cellDoubleClicked, this, [this, rows](int r, int) {
        if (r >= 0 && r < static_cast<int>(rows->size())) {
            emit loadTrack((*rows)[r].pTrack);
        }
    });
    pLayout->addWidget(pTable);

    auto* pButtons = new QDialogButtonBox(pDialog);
    QPushButton* pDo = pButtons->addButton(tr("Do it"), QDialogButtonBox::AcceptRole);
    QPushButton* pAllFix = pButtons->addButton(tr("Fix all"), QDialogButtonBox::ActionRole);
    QPushButton* pUndo = pButtons->addButton(tr("Undo fixes..."), QDialogButtonBox::ActionRole);
    pButtons->addButton(QDialogButtonBox::Close);
    pAllFix->setToolTip(tr("Every song that can be fixed, except the ones marked \"Listen "
                           "first\" and the ones whose BPM you locked."));
    pUndo->setToolTip(tr("Put songs back the way they were before the beat check fixed them."));
    pUndo->setEnabled(anyUndo);
    connect(pAllFix, &QPushButton::clicked, pDialog, [pTable, rows]() {
        for (int i = 0; i < pTable->rowCount(); ++i) {
            const beatcheck::Review& r = (*rows)[i].review;
            if (r.halfBeat || r.replacesLock) {
                continue; // needs listening, or the DJ's own lock
            }
            auto* pChoice = qobject_cast<QComboBox*>(pTable->cellWidget(i, 0));
            const int fix = pChoice ? pChoice->findData(1) : -1;
            if (fix >= 0) {
                pChoice->setCurrentIndex(fix);
            }
        }
    });
    connect(pUndo, &QPushButton::clicked, pDialog, [this, pDialog]() {
        pDialog->reject();
        slotBeatCheckUndo();
    });
    connect(pButtons, &QDialogButtonBox::rejected, pDialog, &QDialog::reject);
    connect(pDo, &QPushButton::clicked, pDialog, [this, pDialog, pTable, rows, db]() {
        int fixed = 0;
        int ignoredNow = 0;
        for (int i = 0; i < pTable->rowCount(); ++i) {
            auto* pChoice = qobject_cast<QComboBox*>(pTable->cellWidget(i, 0));
            const int choice = pChoice ? pChoice->currentData().toInt() : 0;
            const Row& row = (*rows)[i];
            if (choice == 2) {
                EnergyStore::setBeatCheckIgnored(db, row.pTrack->getId(), row.signature);
                ++ignoredNow;
                continue;
            }
            if (choice != 1) {
                continue;
            }
            const beatcheck::Review& r = row.review;
            const auto sampleRate = row.pTrack->getSampleRate();
            if (!sampleRate.isValid()) {
                continue;
            }
            const auto frame = [sampleRate](double sec) {
                return mixxx::audio::FramePos(sec * sampleRate.value()).toNearestFrameBoundary();
            };
            // What it was before, kept for Undo (from before the first Fix).
            EnergyStore::BeatCheckUndo undo =
                    EnergyStore::loadBeatCheckUndo(db, row.pTrack->getId())
                            .value_or(EnergyStore::BeatCheckUndo());
            bool changed = false;
            if (r.newGrid) {
                mixxx::BeatsPointer pBeats;
                if (r.steady) {
                    pBeats = mixxx::Beats::fromConstTempo(
                            sampleRate, frame(r.newFirstBeatSec), mixxx::Bpm(r.newBpm));
                } else {
                    QVector<mixxx::audio::FramePos> positions;
                    positions.reserve(static_cast<int>(r.newBeats.size()));
                    for (double t : r.newBeats) {
                        positions.append(frame(t));
                    }
                    pBeats = mixxx::Beats::fromBeatPositions(sampleRate, positions);
                }
                if (pBeats) {
                    const mixxx::BeatsPointer pOld = row.pTrack->getBeats();
                    const bool wasLocked = row.pTrack->isBpmLocked();
                    if (wasLocked) {
                        row.pTrack->setBpmLocked(false); // the DJ chose Fix for it
                    }
                    if (row.pTrack->trySetBeats(pBeats)) {
                        if (!undo.gridSaved) {
                            undo.gridSaved = true;
                            undo.oldBeats = pOld ? pOld->toByteArray() : QByteArray();
                            undo.oldBeatsVersion = pOld ? pOld->getVersion() : QString();
                            undo.oldBeatsSubVersion = pOld ? pOld->getSubVersion() : QString();
                            undo.oldBpmLocked = wasLocked;
                        }
                        undo.newBeatsHash = beatsHash(row.pTrack->getBeats());
                        changed = true;
                        row.pTrack->setBpmLocked(true); // a later analysis keeps it
                    } else {
                        row.pTrack->setBpmLocked(wasLocked);
                    }
                }
            }
            if (r.newIntroEndSec >= 0.0) {
                // Counts as the DJ's marker from now on (the analysis no
                // longer moves it): the DJ chose it.
                if (const CuePointer pIntro = row.pTrack->findCueByType(mixxx::CueType::Intro)) {
                    const mixxx::audio::FramePos oldEnd = pIntro->getEndPosition();
                    const mixxx::audio::FramePos newEnd = frame(r.newIntroEndSec);
                    pIntro->setEndPosition(newEnd);
                    if (!undo.introSaved) {
                        undo.introSaved = true;
                        undo.oldIntroEndFrame = oldEnd.isValid() ? oldEnd.value() : -1.0;
                    }
                    undo.newIntroEndFrame = newEnd.value();
                    changed = true;
                }
            }
            if (changed) {
                ++fixed;
                const QString what = QString(row.fix).replace(QChar('\n'), QChar(' '));
                undo.fixedAt = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
                undo.what = undo.what.isEmpty() ? what : undo.what + QStringLiteral(" | ") + what;
                EnergyStore::saveBeatCheckUndo(db, row.pTrack->getId(), undo);
                qInfo().noquote() << "Beat check: fixed" << row.pTrack->getInfo() << "-" << what
                                  << (r.newGrid ? QStringLiteral("(grid check %1 beats)")
                                                          .arg(row.driftBeats, 0, 'f', 3)
                                                : QString());
            }
        }
        m_pAnalysisLibraryTableModel->select();
        QMessageBox::information(pDialog,
                tr("Beat Check"),
                tr("Fixed %1 song(s), ignored %2. Undo fixes... puts any of them back.")
                        .arg(fixed)
                        .arg(ignoredNow));
        pDialog->accept();
    });
    pLayout->addWidget(pButtons);
    pDialog->resize(1100, 680);
    pDialog->show();
}

void DlgAnalysis::slotBeatCheckUndo() {
    TrackCollectionManager* pManager = m_pLibrary->trackCollectionManager();
    if (!pManager || !pManager->internalCollection()) {
        return;
    }
    const QSqlDatabase db = pManager->internalCollection()->database();
    struct UndoRow {
        TrackPointer pTrack;
        EnergyStore::BeatCheckUndo undo;
    };
    auto rows = std::make_shared<std::vector<UndoRow>>();
    for (const TrackId& id : EnergyStore::beatCheckUndoTracks(db)) {
        const TrackPointer pTrack = pManager->getTrackById(id);
        const auto undo = EnergyStore::loadBeatCheckUndo(db, id);
        if (pTrack && undo) {
            rows->push_back({pTrack, *undo});
        }
    }
    if (rows->empty()) {
        QMessageBox::information(this, tr("Undo Beat Check"), tr("No beat check fix to undo."));
        return;
    }
    std::sort(rows->begin(), rows->end(), [](const UndoRow& a, const UndoRow& b) {
        return a.undo.fixedAt > b.undo.fixedAt; // the latest first
    });
    auto* pDialog = new QDialog(this);
    pDialog->setAttribute(Qt::WA_DeleteOnClose);
    pDialog->setWindowTitle(tr("Undo Beat Check"));
    auto* pLayout = new QVBoxLayout(pDialog);
    auto* pSummary = new QLabel(
            tr("Tick the songs to put back the way they were before the beat check fixed "
               "them (their beat grid, BPM lock and Intro End marker), then press Undo "
               "ticked. Double-click a song to load it on a deck."),
            pDialog);
    pSummary->setWordWrap(true);
    pLayout->addWidget(pSummary);
    auto* pTable = new QTableWidget(static_cast<int>(rows->size()), 4, pDialog);
    pTable->setHorizontalHeaderLabels({tr("Undo"), tr("Song"), tr("What the fix did"), tr("When")});
    pTable->verticalHeader()->hide();
    pTable->setWordWrap(true);
    pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int i = 0; i < static_cast<int>(rows->size()); ++i) {
        const UndoRow& row = (*rows)[i];
        auto* pTick = new QTableWidgetItem();
        pTick->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        pTick->setCheckState(Qt::Unchecked);
        pTable->setItem(i, 0, pTick);
        pTable->setItem(i, 1, new QTableWidgetItem(songName(row.pTrack)));
        pTable->setItem(i, 2, new QTableWidgetItem(row.undo.what));
        pTable->setItem(i, 3, new QTableWidgetItem(row.undo.fixedAt));
    }
    pTable->resizeColumnsToContents();
    pTable->setColumnWidth(1, std::min(pTable->columnWidth(1), 280));
    pTable->setColumnWidth(2, std::min(std::max(pTable->columnWidth(2), 200), 480));
    pTable->horizontalHeader()->setStretchLastSection(true);
    pTable->resizeRowsToContents();
    connect(pTable, &QTableWidget::cellDoubleClicked, this, [this, rows](int r, int) {
        if (r >= 0 && r < static_cast<int>(rows->size())) {
            emit loadTrack((*rows)[r].pTrack);
        }
    });
    pLayout->addWidget(pTable);
    auto* pButtons = new QDialogButtonBox(pDialog);
    QPushButton* pDo = pButtons->addButton(tr("Undo ticked"), QDialogButtonBox::AcceptRole);
    QPushButton* pTickAll = pButtons->addButton(tr("Tick all"), QDialogButtonBox::ActionRole);
    pButtons->addButton(QDialogButtonBox::Close);
    connect(pTickAll, &QPushButton::clicked, pDialog, [pTable]() {
        for (int i = 0; i < pTable->rowCount(); ++i) {
            pTable->item(i, 0)->setCheckState(Qt::Checked);
        }
    });
    connect(pButtons, &QDialogButtonBox::rejected, pDialog, &QDialog::reject);
    connect(pDo, &QPushButton::clicked, pDialog, [this, pDialog, pTable, rows, db]() {
        int undone = 0;
        for (int i = 0; i < pTable->rowCount(); ++i) {
            if (pTable->item(i, 0)->checkState() != Qt::Checked) {
                continue;
            }
            const UndoRow& row = (*rows)[i];
            const EnergyStore::BeatCheckUndo& undo = row.undo;
            const auto sampleRate = row.pTrack->getSampleRate();
            bool ok = true;
            if (undo.gridSaved && sampleRate.isValid()) {
                const mixxx::BeatsPointer pOld = undo.oldBeats.isEmpty()
                        ? mixxx::BeatsPointer()
                        : mixxx::Beats::fromByteArray(sampleRate,
                                  undo.oldBeatsVersion,
                                  undo.oldBeatsSubVersion,
                                  undo.oldBeats);
                row.pTrack->setBpmLocked(false);
                ok = row.pTrack->trySetBeats(pOld);
                row.pTrack->setBpmLocked(undo.oldBpmLocked);
            }
            if (undo.introSaved && undo.oldIntroEndFrame >= 0.0) {
                if (const CuePointer pIntro = row.pTrack->findCueByType(mixxx::CueType::Intro)) {
                    pIntro->setEndPosition(mixxx::audio::FramePos(undo.oldIntroEndFrame));
                }
            }
            if (!ok) {
                qWarning().noquote() << "Beat check: could not undo" << row.pTrack->getInfo();
                continue;
            }
            EnergyStore::deleteBeatCheckUndo(db, row.pTrack->getId());
            ++undone;
            qInfo().noquote() << "Beat check: undid" << row.pTrack->getInfo() << "-" << undo.what;
        }
        m_pAnalysisLibraryTableModel->select();
        QMessageBox::information(pDialog,
                tr("Undo Beat Check"),
                tr("Put %1 song(s) back the way they were.").arg(undone));
        pDialog->accept();
    });
    pLayout->addWidget(pButtons);
    pDialog->resize(1000, 600);
    pDialog->show();
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
