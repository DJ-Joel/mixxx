// Auto DJ 2.0 plus Video Mixing: video recording (see videorecorder.h).
//
// Built without the precompiled header (see CMakeLists.txt), so the Windows
// headers below get NOMINMAX before anything else includes them.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // keep std::min / std::max usable
#endif
#endif

#include "video/videorecorder.h"

#include <QtDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

#include "video/videomix.h"

// ---------------------------------------------------------------------------
// The recorder (any system).
// ---------------------------------------------------------------------------

VideoRecorder::VideoRecorder()
        : m_pEncoder(makeEncoder()) {
}

VideoRecorder::~VideoRecorder() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping) {
            m_stopping = true;
            m_endFrame = std::max(m_audioEnd, m_settings.startFrame);
        }
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool VideoRecorder::isSupported() {
    return static_cast<bool>(makeEncoder());
}

bool VideoRecorder::start(const Settings& settings, QString* pError) {
    if (!m_pEncoder) {
        if (pError) {
            *pError = QStringLiteral("Video recording needs Windows.");
        }
        m_finished = true;
        return false;
    }
    if (settings.sampleRate != 44100 && settings.sampleRate != 48000) {
        if (pError) {
            *pError = QStringLiteral(
                    "Video recording needs the sound at 44100 or 48000 Hz "
                    "(it is %1 Hz). Change it in Preferences > Sound Hardware.")
                              .arg(settings.sampleRate);
        }
        m_finished = true;
        return false;
    }
    m_settings = settings;
    m_settings.width &= ~1; // NV12 needs even sizes
    m_settings.height &= ~1;
    m_nv12.assign(static_cast<std::size_t>(m_settings.width) * m_settings.height * 3 / 2, 0);

    // The file is opened on the writer thread (Windows wants the encoder
    // used on the thread that made it); wait here for the answer.
    std::promise<QString> opened;
    std::future<QString> openResult = opened.get_future();
    m_thread = std::thread([this, &opened]() {
        QString error;
        if (!m_pEncoder->open(m_settings, &error)) {
            if (error.isEmpty()) {
                error = QStringLiteral("The video file could not be made.");
            }
            opened.set_value(error);
            m_finished = true;
            return;
        }
        opened.set_value(QString());
        run();
    });
    const QString error = openResult.get();
    if (!error.isEmpty()) {
        m_thread.join();
        if (pError) {
            *pError = error;
        }
        qWarning() << "Video recording: could not start:" << error;
        return false;
    }
    qInfo().noquote() << "Video recording: started" << m_settings.path << "("
                      << m_settings.width << "x" << m_settings.height << "at"
                      << m_settings.fps << "pictures a second, sound at"
                      << m_settings.sampleRate << "Hz)";
    return true;
}

void VideoRecorder::addPicture(qint64 engineFrame, const QImage& picture) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || m_finished) {
            return;
        }
        if (picture.isNull()) {
            // Only the time moved on: merge with a waiting "same picture".
            if (!m_pictures.empty() && m_pictures.back().image.isNull()) {
                m_pictures.back().frame = std::max(m_pictures.back().frame, engineFrame);
                return;
            }
        } else if (m_waitingImages >= kMaxWaitingPictures) {
            // The writer is behind: drop the oldest waiting picture (the
            // one before it stays on a little longer), keep the newest.
            for (Picture& waiting : m_pictures) {
                if (!waiting.image.isNull()) {
                    waiting.image = QImage();
                    --m_waitingImages;
                    ++m_droppedPictures;
                    break;
                }
            }
        }
        Picture next;
        next.frame = engineFrame;
        next.image = picture;
        if (!picture.isNull()) {
            ++m_waitingImages;
        }
        m_pictures.push_back(std::move(next));
    }
    m_wake.notify_one();
}

void VideoRecorder::addAudio(const float* pSamples, std::size_t sampleCount, qint64 firstFrame) {
    const qint64 frames = static_cast<qint64>(sampleCount / 2);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_finished || frames <= 0) {
        return;
    }
    qint64 begin = std::max(firstFrame, m_settings.startFrame);
    qint64 end = firstFrame + frames;
    m_audioEnd = std::max(m_audioEnd, end);
    if (m_stopping) {
        end = std::min(end, m_endFrame);
    }
    if (end <= begin) {
        return;
    }
    if (m_audioStart < 0) {
        m_audioStart = begin;
    } else {
        // Frames arrive in order, so this only skips a repeat.
        const qint64 expected = m_audioStart + static_cast<qint64>(m_audio.size() / 2);
        begin = std::max(begin, expected);
        if (end <= begin) {
            return;
        }
    }
    const std::size_t oldSize = m_audio.size();
    const std::size_t count = static_cast<std::size_t>(end - begin) * 2;
    m_audio.resize(oldSize + count);
    videomix::floatToPcm16(pSamples + (begin - firstFrame) * 2, count, m_audio.data() + oldSize);
    m_wake.notify_one();
}

void VideoRecorder::stop(qint64 endFrame) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_stopping = true;
        m_endFrame = std::max(endFrame, m_settings.startFrame);
    }
    m_wake.notify_all();
    qInfo() << "Video recording: stopping, waiting for the last sound";
}

QString VideoRecorder::error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

double VideoRecorder::secondsRecorded(qint64 engineFrame) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const qint64 last = m_stopping ? m_endFrame : engineFrame;
    return std::max<qint64>(0, last - m_settings.startFrame) /
            static_cast<double>(m_settings.sampleRate);
}

bool VideoRecorder::writePicturesUntil(qint64 frame) {
    const double seconds = (frame - m_settings.startFrame) /
            static_cast<double>(m_settings.sampleRate);
    const long long target = videomix::videoFramesBefore(seconds, m_settings.fps);
    while (m_nextVideoIndex < target) {
        if (!m_lastConverted) {
            if (m_lastImage.isNull()) {
                // Nothing drawn yet: black.
                const std::size_t lumaBytes =
                        static_cast<std::size_t>(m_settings.width) * m_settings.height;
                std::fill(m_nv12.begin(), m_nv12.begin() + lumaBytes, std::uint8_t{16});
                std::fill(m_nv12.begin() + lumaBytes, m_nv12.end(), std::uint8_t{128});
            } else {
                QImage image = m_lastImage;
                if (image.width() != m_settings.width || image.height() != m_settings.height) {
                    image = image.scaled(m_settings.width,
                            m_settings.height,
                            Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
                }
                if (image.format() != QImage::Format_RGB32 &&
                        image.format() != QImage::Format_ARGB32) {
                    image = image.convertToFormat(QImage::Format_RGB32);
                }
                videomix::bgraToNv12(image.constBits(),
                        m_settings.width,
                        m_settings.height,
                        static_cast<int>(image.bytesPerLine()),
                        m_nv12.data());
            }
            m_lastConverted = true;
        } else {
            ++m_duplicates;
        }
        if (!m_pEncoder->writeVideo(m_nv12.data(), m_nv12.size(), m_nextVideoIndex)) {
            return false;
        }
        ++m_nextVideoIndex;
    }
    return true;
}

void VideoRecorder::run() {
    bool failed = false;
    QString failure;
    bool stopping = false;
    std::chrono::steady_clock::time_point stopSeen;
    while (true) {
        std::deque<Picture> pictures;
        std::vector<std::int16_t> audio;
        qint64 audioStart = -1;
        qint64 audioEnd = 0;
        qint64 endFrame = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !m_pictures.empty() || !m_audio.empty() || m_stopping;
            });
            pictures.swap(m_pictures);
            m_waitingImages = 0;
            audio.swap(m_audio);
            audioStart = m_audioStart;
            m_audioStart = -1;
            audioEnd = m_audioEnd;
            if (m_stopping && !stopping) {
                stopping = true;
                stopSeen = std::chrono::steady_clock::now();
            }
            endFrame = m_endFrame;
        }
        if (!failed && !audio.empty()) {
            const qint64 first = audioStart - m_settings.startFrame;
            const std::size_t frames = audio.size() / 2;
            if (m_pEncoder->writeAudio(audio.data(), frames, first)) {
                m_audioWritten = first + static_cast<qint64>(frames);
            } else {
                failed = true;
                failure = QStringLiteral("The sound could not be written.");
            }
        }
        for (Picture& picture : pictures) {
            if (failed) {
                break;
            }
            qint64 frame = picture.frame;
            if (stopping) {
                frame = std::min(frame, endFrame);
            }
            if (!writePicturesUntil(frame)) {
                failed = true;
                failure = QStringLiteral("The picture could not be written.");
                break;
            }
            if (!picture.image.isNull()) {
                m_lastImage = std::move(picture.image);
                m_lastConverted = false;
            }
        }
        if (stopping) {
            const bool allSound = audioEnd >= endFrame;
            const bool waitedEnough = std::chrono::steady_clock::now() - stopSeen >
                    std::chrono::milliseconds(kLastSoundWaitMs);
            if (failed || allSound || waitedEnough) {
                break;
            }
        }
    }

    // The last sound, and pictures up to the end.
    qint64 endFrame = 0;
    std::vector<std::int16_t> audio;
    qint64 audioStart = -1;
    long long dropped = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        audio.swap(m_audio);
        audioStart = m_audioStart;
        m_audioStart = -1;
        endFrame = m_endFrame;
        dropped = m_droppedPictures;
    }
    if (!failed && !audio.empty()) {
        const qint64 first = audioStart - m_settings.startFrame;
        if (m_pEncoder->writeAudio(audio.data(), audio.size() / 2, first)) {
            m_audioWritten = first + static_cast<qint64>(audio.size() / 2);
        } else {
            failed = true;
            failure = QStringLiteral("The sound could not be written.");
        }
    }
    if (!failed && !writePicturesUntil(endFrame)) {
        failed = true;
        failure = QStringLiteral("The picture could not be written.");
    }
    QString finishError;
    const bool finished = m_pEncoder->finish(&finishError);
    if (!failed && !finished) {
        failed = true;
        failure = finishError.isEmpty()
                ? QStringLiteral("The video file could not be completed.")
                : finishError;
    }
    const double soundSeconds = m_audioWritten / static_cast<double>(m_settings.sampleRate);
    if (failed) {
        qWarning().noquote() << "Video recording: FAILED:" << failure << "("
                             << m_settings.path << ")";
    } else {
        qInfo().noquote() << "Video recording: saved" << m_settings.path << ":"
                          << QString::number(soundSeconds, 'f', 1) << "seconds,"
                          << m_nextVideoIndex << "pictures (" << m_duplicates
                          << "repeated while the screen did not change," << dropped
                          << "dropped because the writer was behind)";
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = failed ? failure : QString();
    }
    m_finished = true;
}

// ---------------------------------------------------------------------------
// The Windows encoder (Media Foundation Sink Writer).
// ---------------------------------------------------------------------------

#if defined(_WIN32) && defined(__MEDIAFOUNDATION__)

#include <windows.h>
// windows.h first
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstring>
#include <string>

namespace {

// Spelled out, so nothing depends on which GUIDs the import libraries have.
// MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS (use the graphics card's encoder)
const GUID kHardwareTransforms = {
        0xa634a91c, 0x822b, 0x41b9, {0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0}};

constexpr UINT32 kH264High = 100;  // eAVEncH264VProfile_High
constexpr UINT32 kAacBytesPerSecond = 24000; // 192 kbit/s
constexpr LONGLONG kHundredNanoseconds = 10000000;

template<class T>
void release(T*& pObject) {
    if (pObject) {
        pObject->Release();
        pObject = nullptr;
    }
}

QString hresultText(const char* what, HRESULT hr) {
    return QStringLiteral("%1 (error 0x%2)")
            .arg(QString::fromLatin1(what))
            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

class MediaFoundationEncoder : public VideoRecorder::Encoder {
  public:
    ~MediaFoundationEncoder() override {
        release(m_pWriter);
        shutdown();
    }

    bool open(const VideoRecorder::Settings& settings, QString* pError) override {
        m_settings = settings;
        m_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        m_hrStartup = MFStartup(MF_VERSION);
        if (FAILED(m_hrStartup)) {
            *pError = hresultText("Windows Media Foundation did not start", m_hrStartup);
            return false;
        }
        // High profile first (sharper), then the encoder's own choice.
        HRESULT hr = makeWriter(true);
        if (FAILED(hr)) {
            qInfo() << "Video recording: the encoder refused H.264 High, trying its default";
            release(m_pWriter);
            ::DeleteFileW(reinterpret_cast<const wchar_t*>(m_settings.path.utf16()));
            hr = makeWriter(false);
        }
        if (FAILED(hr)) {
            release(m_pWriter);
            *pError = hresultText(m_step, hr);
            return false;
        }
        return true;
    }

    bool writeVideo(const std::uint8_t* pNv12, std::size_t bytes, long long index) override {
        const LONGLONG start = index * kHundredNanoseconds / m_settings.fps;
        const LONGLONG next = (index + 1) * kHundredNanoseconds / m_settings.fps;
        return writeSample(m_videoStream, pNv12, bytes, start, next - start, "picture");
    }

    bool writeAudio(const std::int16_t* pPcm, std::size_t frames, qint64 firstFrame) override {
        const LONGLONG start = firstFrame * kHundredNanoseconds / m_settings.sampleRate;
        const LONGLONG end = (firstFrame + static_cast<qint64>(frames)) *
                kHundredNanoseconds / m_settings.sampleRate;
        return writeSample(m_audioStream,
                reinterpret_cast<const std::uint8_t*>(pPcm),
                frames * 4,
                start,
                end - start,
                "sound");
    }

    bool finish(QString* pError) override {
        bool ok = true;
        if (m_pWriter) {
            const HRESULT hr = m_pWriter->Finalize();
            if (FAILED(hr)) {
                *pError = hresultText("The video file could not be completed", hr);
                ok = false;
            }
            release(m_pWriter);
        }
        shutdown();
        return ok;
    }

  private:
    HRESULT makeWriter(bool highProfile) {
        HRESULT hr = S_OK;
        IMFAttributes* pAttributes = nullptr;
        IMFMediaType* pType = nullptr;
        const std::wstring path = m_settings.path.toStdWString();
        const UINT32 width = static_cast<UINT32>(m_settings.width);
        const UINT32 height = static_cast<UINT32>(m_settings.height);
        const UINT32 fps = static_cast<UINT32>(m_settings.fps);
        const UINT32 rate = static_cast<UINT32>(m_settings.sampleRate);

        // Each step runs only if all before it worked.
        auto step = [this, &hr](const char* what, auto action) {
            if (SUCCEEDED(hr)) {
                hr = action();
                if (FAILED(hr)) {
                    m_step = what;
                }
            }
        };

        step("Could not set up the video file", [&]() {
            return MFCreateAttributes(&pAttributes, 1);
        });
        if (SUCCEEDED(hr)) {
            pAttributes->SetUINT32(kHardwareTransforms, TRUE);
        }
        step("Could not create the video file (is the folder writable?)", [&]() {
            return MFCreateSinkWriterFromURL(path.c_str(), nullptr, pAttributes, &m_pWriter);
        });

        // Picture: H.264 in the file ...
        step("Could not set up the video", [&]() {
            return MFCreateMediaType(&pType);
        });
        if (SUCCEEDED(hr)) {
            pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            pType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(m_settings.videoBitsPerSecond));
            pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
            MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, fps, 1);
            MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (highProfile) {
                pType->SetUINT32(MF_MT_MPEG2_PROFILE, kH264High);
            }
        }
        step("Windows has no H.264 video encoder", [&]() {
            return m_pWriter->AddStream(pType, &m_videoStream);
        });
        release(pType);
        // ... fed with NV12 pictures.
        step("Could not set up the video", [&]() {
            return MFCreateMediaType(&pType);
        });
        if (SUCCEEDED(hr)) {
            pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            pType->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
            pType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            pType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
            MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, fps, 1);
            MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        }
        step("The H.264 encoder does not take the pictures", [&]() {
            return m_pWriter->SetInputMediaType(m_videoStream, pType, nullptr);
        });
        release(pType);

        // Sound: AAC in the file ...
        step("Could not set up the sound", [&]() {
            return MFCreateMediaType(&pType);
        });
        if (SUCCEEDED(hr)) {
            pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSecond);
        }
        step("Windows has no AAC sound encoder", [&]() {
            return m_pWriter->AddStream(pType, &m_audioStream);
        });
        release(pType);
        // ... fed with 16-bit stereo.
        step("Could not set up the sound", [&]() {
            return MFCreateMediaType(&pType);
        });
        if (SUCCEEDED(hr)) {
            pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
            pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }
        step("The AAC encoder does not take the sound", [&]() {
            return m_pWriter->SetInputMediaType(m_audioStream, pType, nullptr);
        });
        release(pType);

        step("Could not start writing the video file", [&]() {
            return m_pWriter->BeginWriting();
        });
        release(pAttributes);
        return hr;
    }

    bool writeSample(DWORD stream,
            const std::uint8_t* pData,
            std::size_t bytes,
            LONGLONG start,
            LONGLONG duration,
            const char* what) {
        if (!m_pWriter) {
            return false;
        }
        IMFMediaBuffer* pBuffer = nullptr;
        IMFSample* pSample = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &pBuffer);
        if (SUCCEEDED(hr)) {
            BYTE* pDestination = nullptr;
            hr = pBuffer->Lock(&pDestination, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                std::memcpy(pDestination, pData, bytes);
                pBuffer->Unlock();
                hr = pBuffer->SetCurrentLength(static_cast<DWORD>(bytes));
            }
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateSample(&pSample);
        }
        if (SUCCEEDED(hr)) {
            hr = pSample->AddBuffer(pBuffer);
        }
        if (SUCCEEDED(hr)) {
            hr = pSample->SetSampleTime(start);
        }
        if (SUCCEEDED(hr)) {
            hr = pSample->SetSampleDuration(duration);
        }
        if (SUCCEEDED(hr)) {
            hr = m_pWriter->WriteSample(stream, pSample);
        }
        release(pSample);
        release(pBuffer);
        if (FAILED(hr)) {
            qWarning().noquote() << "Video recording:"
                                 << hresultText(what, hr) << "at"
                                 << QString::number(start / 1e7, 'f', 2) << "s";
            return false;
        }
        return true;
    }

    void shutdown() {
        if (SUCCEEDED(m_hrStartup)) {
            MFShutdown();
            m_hrStartup = E_FAIL;
        }
        if (SUCCEEDED(m_hrCom)) {
            CoUninitialize();
            m_hrCom = E_FAIL;
        }
    }

    VideoRecorder::Settings m_settings;
    IMFSinkWriter* m_pWriter = nullptr;
    DWORD m_videoStream = 0;
    DWORD m_audioStream = 0;
    HRESULT m_hrCom = E_FAIL;
    HRESULT m_hrStartup = E_FAIL;
    const char* m_step = "";
};

} // namespace

std::unique_ptr<VideoRecorder::Encoder> VideoRecorder::makeEncoder() {
    return std::make_unique<MediaFoundationEncoder>();
}

#else

std::unique_ptr<VideoRecorder::Encoder> VideoRecorder::makeEncoder() {
    return nullptr; // video recording is Windows only for now
}

#endif
