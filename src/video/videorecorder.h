#pragma once

#include <QImage>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/// Auto DJ 2.0 plus Video Mixing: records the mixed picture (with titles,
/// logo and cuts) and the mixed sound into one MP4 file (H.264 video, AAC
/// sound), using the video encoder built into Windows.
///
/// Picture and sound share one clock: the number of sound frames the audio
/// engine has made. The sound arrives from the recording side channel in
/// batches (up to about half a second late), each tagged with its frame
/// number; each picture is stamped with the engine's frame count at the
/// moment it was drawn. So they stay in step however late either arrives.
///
/// The file is written on a thread of its own, so recording never holds
/// up the video or the audio engine. If that thread falls behind, pictures
/// are dropped (the previous one is kept longer), never the sound.
class VideoRecorder {
  public:
    struct Settings {
        QString path;
        int width = 1920;
        int height = 1080;
        int fps = 30;
        int videoBitsPerSecond = 12000000;
        int sampleRate = 44100; ///< only 44100 or 48000 (AAC)
        qint64 startFrame = 0;  ///< engine frame where the recording starts
    };

    /// The file writer (Windows Media Foundation), or nullptr where there
    /// is none.
    class Encoder {
      public:
        virtual ~Encoder() = default;
        virtual bool open(const Settings& settings, QString* pError) = 0;
        /// One NV12 picture, number `index` (it starts at index / fps).
        virtual bool writeVideo(const std::uint8_t* pNv12, std::size_t bytes, long long index) = 0;
        /// Stereo 16-bit sound; `firstFrame` counts from the recording start.
        virtual bool writeAudio(const std::int16_t* pPcm, std::size_t frames, qint64 firstFrame) = 0;
        /// Completes the file.
        virtual bool finish(QString* pError) = 0;
    };
    static std::unique_ptr<Encoder> makeEncoder();
    static bool isSupported();

    VideoRecorder();
    ~VideoRecorder(); ///< finishes the file (waits for it)

    /// Opens the file and starts the writer thread. False (with a reason)
    /// if the file cannot be made.
    bool start(const Settings& settings, QString* pError);
    /// A new picture drawn when the engine had made `engineFrame` frames
    /// (after the picture timing). A null image = nothing new: the last
    /// picture is still on screen (keeps the video moving).
    void addPicture(qint64 engineFrame, const QImage& picture);
    /// Sound from the side channel (any thread).
    void addAudio(const float* pSamples, std::size_t sampleCount, qint64 firstFrame);
    /// Ends the recording at this engine frame. Returns at once; the
    /// writer thread waits for the last sound, completes the file and ends.
    void stop(qint64 endFrame);

    bool isFinished() const {
        return m_finished.load();
    }
    /// Empty, or why the recording failed.
    QString error() const;
    const Settings& settings() const {
        return m_settings;
    }
    double secondsRecorded(qint64 engineFrame) const;

    /// At most this many new pictures wait for the writer thread.
    static constexpr int kMaxWaitingPictures = 8;
    /// After stop, wait this long at most for the last sound.
    static constexpr int kLastSoundWaitMs = 3000;

  private:
    struct Picture {
        qint64 frame = 0;
        QImage image; ///< null = "time moved on, same picture"
    };
    void run();
    bool writePicturesUntil(qint64 frame);

    Settings m_settings;
    std::unique_ptr<Encoder> m_pEncoder;
    std::thread m_thread;
    std::atomic<bool> m_finished{false};

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Picture> m_pictures;
    int m_waitingImages = 0;
    std::vector<std::int16_t> m_audio; ///< waiting sound (stereo)
    qint64 m_audioStart = -1;          ///< engine frame of m_audio[0]
    qint64 m_audioEnd = 0;             ///< engine frame after the last sound received
    bool m_stopping = false;
    qint64 m_endFrame = 0;
    QString m_error;
    long long m_droppedPictures = 0;

    // Writer thread only.
    QImage m_lastImage;
    bool m_lastConverted = false;
    std::vector<std::uint8_t> m_nv12;
    long long m_nextVideoIndex = 0;
    qint64 m_audioWritten = 0; ///< frames written, from the recording start
    long long m_duplicates = 0;
};
