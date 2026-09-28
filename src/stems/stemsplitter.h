#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "preferences/usersettings.h"
#include "stems/stemcache.h"
#include "track/track_decl.h"
#include "track/trackid.h"

namespace stems {
class Engine;
}

/// Auto DJ 2.0 plus Video Mixing: splits songs into drums, bass, other and
/// vocals in the background and keeps the parts as stem files
/// (<settings folder>/stems/*.stem.mp4), so each song is split only once.
///
/// Songs loaded into a deck go first, then the next songs of the Auto DJ
/// queue. One song at a time, on its own thread. If the stems engine is not
/// installed nothing happens (one line in the log).
class StemSplitter : public QObject {
    Q_OBJECT
  public:
    explicit StemSplitter(UserSettingsPointer pConfig, QObject* pParent = nullptr);
    ~StemSplitter() override;

    /// Split this song (if not done yet). `urgent`: loaded in a deck, goes
    /// before the queued songs.
    void request(const TrackPointer& pTrack, bool urgent);
    /// The finished stem file of this song, or empty.
    QString stemFile(const TrackPointer& pTrack) const;
    bool isEnabled() const;
    void setEnabled(bool enabled);
    /// The folder the DJ chose for the parts (a "Mixxx Stems" folder is
    /// made inside it). Default: Mixxx's settings folder.
    QString folder() const;
    void setFolder(const QString& folder);
    /// In that folder, or beside each song.
    stems::StemCache::Location location() const;
    void setLocation(stems::StemCache::Location location);

    static constexpr int kMaxWaiting = 8;

    /// The one splitter (made by the Auto DJ page), or nullptr.
    static StemSplitter* instance();
    /// The folder with the AI engine, and whether it is there.
    QString engineFolder() const;
    bool engineInstalled() const;
    /// Not a stem file, the file exists, and no parts yet.
    static bool needsSplit(const TrackPointer& pTrack);
    /// 44.1 or 48 kHz (or not known yet).
    static bool canSplit(const TrackPointer& pTrack);
    /// Songs of the STEM SPLIT list that could not be split ("title: why"),
    /// cleared by reading.
    QStringList takeBatchFailures();
    /// How much faster than playing this computer splits (0 = not known yet).
    double speed() const;

    /// STEM SPLIT: split these songs in the background, after the songs in
    /// the decks and the Auto DJ queue. Returns how many were added (songs
    /// with parts already are skipped).
    int splitBatch(const QList<TrackPointer>& tracks);
    void stopBatch();
    bool batchRunning() const;

  signals:
    /// A song's stem file is ready (from the splitter thread, queued).
    void stemsReady(TrackId trackId, const QString& path);
    /// STEM SPLIT progress (from the splitter thread, queued). total 0 =
    /// stopped or finished.
    void batchProgress(int done, int total, double secondsLeft);

  private:
    struct Job {
        TrackPointer track;
        QString target;
    };
    void run();
    bool split(const Job& job);
    /// Moves the finished file from this computer to its place (beside
    /// the song, else the stems folder). Updates *pTarget.
    bool moveFinished(const QString& partial, QString* pTarget, const Job& job, QString* pError);
    bool copyFile(const QString& from, const QString& to, QString* pError);
    QString targetFor(const TrackPointer& pTrack) const;

    UserSettingsPointer m_pConfig;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::deque<Job> m_batch;    ///< STEM SPLIT list (lowest priority)
    int m_batchTotal = 0;
    int m_batchDone = 0;
    QStringList m_batchFailures;
    QString m_lastError; ///< why the last split failed (splitter thread)
    double m_batchSeconds = 0.0; ///< music still to split in the list
    double m_speedShared = 0.0;  ///< m_speed for other threads
    QString m_current; ///< target being split now
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_cancel{false}; ///< STOP pressed: drop the song being split
    bool m_currentFromBatch = false;   ///< the song being split is from the list
    std::atomic<bool> m_enabled{true};
    bool m_engineFailed = false; ///< engine missing / broken: stop trying
    /// How much faster than playing this computer splits (learnt from each
    /// song, so it fits fast and slow computers alike). 0 = not known yet.
    double m_speed = 0.0; ///< splitter thread only
    std::unique_ptr<stems::Engine> m_pEngine; ///< splitter thread only
};
