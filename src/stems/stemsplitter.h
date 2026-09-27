#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "preferences/usersettings.h"
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

    static constexpr int kMaxWaiting = 8;

  signals:
    /// A song's stem file is ready (from the splitter thread, queued).
    void stemsReady(TrackId trackId, const QString& path);

  private:
    struct Job {
        TrackPointer track;
        QString target;
    };
    void run();
    bool split(const Job& job);
    QString targetFor(const TrackPointer& pTrack) const;

    UserSettingsPointer m_pConfig;
    QString m_folder;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    QString m_current; ///< target being split now
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_enabled{true};
    bool m_engineFailed = false; ///< engine missing / broken: stop trying
    std::unique_ptr<stems::Engine> m_pEngine; ///< splitter thread only
};
