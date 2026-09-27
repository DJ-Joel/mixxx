#pragma once

#include <QImage>
#include <QMutex>
#include <QString>
#include <QWaitCondition>
#include <thread>

/// Auto DJ 2.0 video: decodes the picture of one deck's music video.
///
/// The deck's audio is played by Mixxx as always; this only shows the frame
/// that belongs to the deck's current position. The owner tells it where the
/// deck is (setTarget) many times a second, and it decodes forward (or seeks)
/// on its own thread to the frame for that moment. So tempo changes, loops,
/// jumps and beatmatching carry the picture along, and a slow decoder can
/// never disturb the audio: it only shows an older frame.
///
/// On Windows it uses the Windows decoder (Media Foundation: H.264, H.265,
/// ...), which Mixxx's FFmpeg build does not include; FFmpeg is the
/// fallback for other kinds of video, and the only decoder elsewhere.
class VideoDecoder {
  public:
    /// Pictures are scaled to fit inside this size (the output canvas).
    VideoDecoder(QString name, int maxWidth, int maxHeight);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Opens a file (empty = close). Returns at once; opening happens on
    /// the decoder thread.
    void open(const QString& path);

    /// Where the deck is now: seconds from the start of the track's audio.
    void setTarget(double seconds);

    /// The latest picture for the open file (null if none yet or no video).
    /// `pSerial` gets a number that changes whenever the picture changes.
    QImage frame(quint64* pSerial) const;

    enum class State {
        Closed,  ///< no file
        Opening, ///< being opened
        Video,   ///< has a video stream
        NoVideo, ///< opened, but it has no video stream (plain audio file)
        Failed,  ///< could not be opened or decoded
    };
    State state() const;

    /// Statistics since the last call (then reset): frames shown, seeks,
    /// and milliseconds spent decoding and converting.
    struct Stats {
        int framesShown = 0;
        int seeks = 0;
        double busyMs = 0.0;
    };
    Stats takeStats();

  private:
    void run();

    const QString m_name;
    const int m_maxWidth;
    const int m_maxHeight;

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    // Guarded by m_mutex:
    QString m_requestedPath;
    bool m_pathChanged = false;
    double m_target = 0.0;
    bool m_targetChanged = false;
    bool m_stop = false;
    QImage m_frame;
    quint64 m_serial = 0;
    State m_state = State::Closed;
    Stats m_stats;

    std::thread m_thread;
};
