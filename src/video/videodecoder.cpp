#include "video/videodecoder.h"

#include <QMutexLocker>
#include <QtDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

#include "video/videobackend.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

// A jump further ahead than this is a seek, not "decode forward".
constexpr double kMaxDecodeAheadSec = 2.0;
// Decode at most this many frames before looking at the target again, so a
// new target (the DJ jumps) is picked up quickly.
constexpr int kMaxFramesPerStep = 90;
constexpr double kDefaultFrameSec = 1.0 / 30.0;

using video::Backend;
using video::Picture;

// Graphics-card decoders to try with FFmpeg, best first.
const AVHWDeviceType kHwTypes[] = {
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_DXVA2,
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
};

QString avError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return QString::fromUtf8(text);
}

/// FFmpeg decoding: every system; on Windows for the kinds of video the
/// Windows decoder cannot play (Mixxx's FFmpeg has no H.264/H.265).
class FfmpegBackend : public Backend {
  public:
    FfmpegBackend(QString name, bool useGraphicsCard)
            : m_name(std::move(name)),
              m_useHw(useGraphicsCard) {
    }
    ~FfmpegBackend() override {
        close();
    }

    OpenResult open(const std::string& path) override {
        m_path = path;
        return openFile();
    }

    std::string description() const override {
        char text[200];
        std::snprintf(text,
                sizeof(text),
                "FFmpeg, %s %dx%d %.2f fps, %s",
                m_pCodec ? m_pCodec->codec->name : "?",
                m_pCodec ? m_pCodec->width : 0,
                m_pCodec ? m_pCodec->height : 0,
                1.0 / m_frameSec,
                m_pHwDevice ? ("graphics card (" + m_hwName + ")").c_str()
                            : "processor");
        return text;
    }

    double frameSeconds() const override {
        return m_frameSec;
    }

    bool next(Frame* pFrame, bool* pFailed) override {
        AVFrame* pDecoded = av_frame_alloc();
        std::shared_ptr<void> handle(pDecoded, [](void* p) {
            AVFrame* pF = static_cast<AVFrame*>(p);
            av_frame_free(&pF);
        });
        for (;;) {
            int err = avcodec_receive_frame(m_pCodec, pDecoded);
            if (err == 0) {
                pFrame->seconds = secondsOf(pDecoded);
                m_lastSec = pFrame->seconds;
                pFrame->handle = handle;
                return true;
            }
            if (err == AVERROR_EOF) {
                return false;
            }
            if (err != AVERROR(EAGAIN)) {
                *pFailed = true;
                return false;
            }
            if (m_eof) {
                return false;
            }
            // Feed the decoder the next packet of our stream.
            for (;;) {
                err = av_read_frame(m_pFormat, m_pPacket);
                if (err < 0) {
                    m_eof = true;
                    avcodec_send_packet(m_pCodec, nullptr); // drain
                    break;
                }
                if (m_pPacket->stream_index != m_stream) {
                    av_packet_unref(m_pPacket);
                    continue;
                }
                err = avcodec_send_packet(m_pCodec, m_pPacket);
                av_packet_unref(m_pPacket);
                if (err < 0 && err != AVERROR(EAGAIN)) {
                    *pFailed = true;
                    return false;
                }
                break;
            }
        }
    }

    bool seek(double seconds) override {
        const double streamSec = std::max(0.0, seconds + m_zeroSec);
        const auto ts = static_cast<int64_t>(streamSec / m_timeBase);
        const int err = av_seek_frame(m_pFormat, m_stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m_pCodec);
        m_eof = false;
        m_lastSec = -1.0;
        return err >= 0;
    }

    bool toPicture(const Frame& frame, Picture* pPicture) override {
        AVFrame* pFrame = static_cast<AVFrame*>(frame.handle.get());
        AVFrame* pSource = pFrame;
        AVFrame* pCpuFrame = nullptr;
        if (pFrame->format == m_hwPixFmt && m_hwPixFmt != AV_PIX_FMT_NONE) {
            pCpuFrame = av_frame_alloc();
            if (av_hwframe_transfer_data(pCpuFrame, pFrame, 0) < 0) {
                av_frame_free(&pCpuFrame);
                return false;
            }
            pSource = pCpuFrame;
        }
        const int width = pSource->width;
        const int height = pSource->height;
        bool ok = false;
        if (width > 0 && height > 0) {
            m_pSws = sws_getCachedContext(m_pSws,
                    width,
                    height,
                    static_cast<AVPixelFormat>(pSource->format),
                    width,
                    height,
                    AV_PIX_FMT_BGRA, // = QImage::Format_RGB32 in memory
                    SWS_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
            if (m_pSws) {
                pPicture->width = width;
                pPicture->height = height;
                pPicture->stride = width * 4;
                pPicture->displayAspect = 0.0;
                if (pFrame->sample_aspect_ratio.num > 0 && pFrame->sample_aspect_ratio.den > 0) {
                    pPicture->displayAspect = static_cast<double>(width) / height *
                            av_q2d(pFrame->sample_aspect_ratio);
                }
                pPicture->pixels.resize(static_cast<std::size_t>(pPicture->stride) * height);
                uint8_t* dst[4] = {pPicture->pixels.data(), nullptr, nullptr, nullptr};
                int dstStride[4] = {pPicture->stride, 0, 0, 0};
                sws_scale(m_pSws, pSource->data, pSource->linesize, 0, height, dst, dstStride);
                ok = true;
            }
        }
        if (pCpuFrame) {
            av_frame_free(&pCpuFrame);
        }
        return ok;
    }

    bool retryAnotherWay() override {
        if (!m_pHwDevice) {
            return false;
        }
        qWarning() << "Video:" << m_name
                   << "graphics card decoding failed, switching to processor decoding";
        m_useHw = false;
        return openFile() == OpenResult::Video;
    }

  private:
    static AVPixelFormat chooseFormat(AVCodecContext* pCtx, const AVPixelFormat* pFormats) {
        const auto* pSelf = static_cast<const FfmpegBackend*>(pCtx->opaque);
        for (const AVPixelFormat* p = pFormats; *p != AV_PIX_FMT_NONE; ++p) {
            if (pSelf && *p == pSelf->m_hwPixFmt) {
                return *p;
            }
        }
        // The graphics card cannot do this one: the first software format.
        for (const AVPixelFormat* p = pFormats; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* pDesc = av_pix_fmt_desc_get(*p);
            if (pDesc && !(pDesc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                return *p;
            }
        }
        return pFormats[0];
    }

    void close() {
        if (m_pSws) {
            sws_freeContext(m_pSws);
            m_pSws = nullptr;
        }
        if (m_pPacket) {
            av_packet_free(&m_pPacket);
        }
        if (m_pCodec) {
            avcodec_free_context(&m_pCodec);
        }
        if (m_pHwDevice) {
            av_buffer_unref(&m_pHwDevice);
        }
        if (m_pFormat) {
            avformat_close_input(&m_pFormat);
        }
        m_stream = -1;
        m_eof = false;
        m_hwPixFmt = AV_PIX_FMT_NONE;
        m_hwName.clear();
        m_lastSec = -1.0;
    }

    OpenResult openFile() {
        close();
        int err = avformat_open_input(&m_pFormat, m_path.c_str(), nullptr, nullptr);
        if (err < 0) {
            qWarning() << "Video:" << m_name << "FFmpeg cannot open"
                       << QString::fromStdString(m_path) << avError(err);
            return OpenResult::Failed;
        }
        err = avformat_find_stream_info(m_pFormat, nullptr);
        if (err < 0) {
            return OpenResult::Failed;
        }
        // Is there a video stream at all (whether or not we can decode it)?
        const int anyVideo = av_find_best_stream(
                m_pFormat, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (anyVideo < 0 ||
                (m_pFormat->streams[anyVideo]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            return OpenResult::NoVideo; // no video, or only cover art
        }
        const AVCodec* pDecoder = nullptr;
        m_stream = av_find_best_stream(m_pFormat, AVMEDIA_TYPE_VIDEO, anyVideo, -1, &pDecoder, 0);
        if (m_stream < 0 || !pDecoder) {
            qWarning() << "Video:" << m_name << "FFmpeg has no decoder for the video in"
                       << QString::fromStdString(m_path);
            return OpenResult::Failed;
        }
        AVStream* pStream = m_pFormat->streams[m_stream];
        m_timeBase = av_q2d(pStream->time_base);
        // Mixxx's second 0 is the start of the audio stream.
        m_zeroSec = 0.0;
        const int audio = av_find_best_stream(m_pFormat, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio >= 0 && m_pFormat->streams[audio]->start_time != AV_NOPTS_VALUE) {
            m_zeroSec = m_pFormat->streams[audio]->start_time *
                    av_q2d(m_pFormat->streams[audio]->time_base);
        } else if (m_pFormat->start_time != AV_NOPTS_VALUE) {
            m_zeroSec = m_pFormat->start_time / static_cast<double>(AV_TIME_BASE);
        }
        const AVRational rate = av_guess_frame_rate(m_pFormat, pStream, nullptr);
        m_frameSec = rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : kDefaultFrameSec;
        if (!(m_frameSec > 0.001 && m_frameSec < 1.0)) {
            m_frameSec = kDefaultFrameSec;
        }

        m_pCodec = avcodec_alloc_context3(pDecoder);
        if (!m_pCodec || avcodec_parameters_to_context(m_pCodec, pStream->codecpar) < 0) {
            return OpenResult::Failed;
        }
        m_pCodec->opaque = this;
        if (m_useHw) {
            for (AVHWDeviceType type : kHwTypes) {
                for (int i = 0;; ++i) {
                    const AVCodecHWConfig* pConfig = avcodec_get_hw_config(pDecoder, i);
                    if (!pConfig) {
                        break;
                    }
                    if ((pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                            pConfig->device_type == type &&
                            av_hwdevice_ctx_create(&m_pHwDevice, type, nullptr, nullptr, 0) >=
                                    0) {
                        m_hwPixFmt = pConfig->pix_fmt;
                        m_hwName = av_hwdevice_get_type_name(type);
                        break;
                    }
                }
                if (m_pHwDevice) {
                    break;
                }
            }
        }
        if (m_pHwDevice) {
            m_pCodec->hw_device_ctx = av_buffer_ref(m_pHwDevice);
            m_pCodec->get_format = &FfmpegBackend::chooseFormat;
            m_pCodec->extra_hw_frames = 4; // we keep two frames ourselves
        } else {
            m_pCodec->thread_count = 0; // processor: as many threads as useful
        }
        err = avcodec_open2(m_pCodec, pDecoder, nullptr);
        if (err < 0) {
            qWarning() << "Video:" << m_name << "cannot start the FFmpeg decoder" << avError(err);
            return OpenResult::Failed;
        }
        m_pPacket = av_packet_alloc();
        return OpenResult::Video;
    }

    double secondsOf(const AVFrame* pFrame) const {
        const int64_t pts = pFrame->best_effort_timestamp != AV_NOPTS_VALUE
                ? pFrame->best_effort_timestamp
                : pFrame->pts;
        if (pts == AV_NOPTS_VALUE) {
            return m_lastSec >= 0.0 ? m_lastSec + m_frameSec : 0.0;
        }
        return pts * m_timeBase - m_zeroSec;
    }

    const QString m_name;
    std::string m_path;
    bool m_useHw;
    AVFormatContext* m_pFormat = nullptr;
    AVCodecContext* m_pCodec = nullptr;
    AVBufferRef* m_pHwDevice = nullptr;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    std::string m_hwName;
    SwsContext* m_pSws = nullptr;
    AVPacket* m_pPacket = nullptr;
    int m_stream = -1;
    double m_timeBase = 0.0;
    double m_zeroSec = 0.0;
    double m_frameSec = kDefaultFrameSec;
    bool m_eof = false;
    double m_lastSec = -1.0;
};

/// A decoded picture at its own size, kept for loops and jumps back.
struct RecentPicture {
    double seconds = 0.0;
    double aspect = 0.0; ///< width / height as shown
    QImage image;
    std::size_t bytes() const {
        return static_cast<std::size_t>(image.sizeInBytes());
    }
};

/// The picture as a QImage at its own size.
RecentPicture toRecent(double seconds, const Picture& picture) {
    RecentPicture recent;
    if (picture.width <= 0 || picture.height <= 0 || picture.pixels.empty()) {
        return recent;
    }
    QImage image(picture.width, picture.height, QImage::Format_RGB32);
    for (int y = 0; y < picture.height; ++y) {
        std::memcpy(image.scanLine(y),
                picture.pixels.data() + static_cast<std::size_t>(picture.stride) * y,
                static_cast<std::size_t>(picture.width) * 4);
    }
    recent.seconds = seconds;
    recent.aspect = picture.displayAspect > 0.0
            ? picture.displayAspect
            : static_cast<double>(picture.width) / picture.height;
    recent.image = std::move(image);
    return recent;
}

/// The picture scaled to fit the canvas (shape kept).
QImage toCanvasSize(const RecentPicture& picture, int maxWidth, int maxHeight) {
    const QImage& image = picture.image;
    if (image.isNull()) {
        return {};
    }
    const double aspect = picture.aspect;
    int width = maxWidth;
    int height = static_cast<int>(std::lround(maxWidth / aspect));
    if (height > maxHeight) {
        height = maxHeight;
        width = static_cast<int>(std::lround(maxHeight * aspect));
    }
    if (width == image.width() && height == image.height()) {
        return image;
    }
    return image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace

VideoDecoder::VideoDecoder(QString name, int maxWidth, int maxHeight, bool useGraphicsCard)
        : m_name(std::move(name)),
          m_maxWidth(maxWidth),
          m_maxHeight(maxHeight),
          m_useGraphicsCard(useGraphicsCard) {
    m_thread = std::thread([this]() {
        run();
    });
}

VideoDecoder::~VideoDecoder() {
    {
        QMutexLocker locker(&m_mutex);
        m_stop = true;
        m_wake.wakeAll();
    }
    m_thread.join();
}

void VideoDecoder::open(const QString& path) {
    QMutexLocker locker(&m_mutex);
    if (path == m_requestedPath && !m_pathChanged && m_state != State::Failed) {
        return;
    }
    m_requestedPath = path;
    m_pathChanged = true;
    m_state = path.isEmpty() ? State::Closed : State::Opening;
    m_frame = QImage();
    ++m_serial;
    m_wake.wakeAll();
}

void VideoDecoder::setTarget(double seconds) {
    QMutexLocker locker(&m_mutex);
    if (seconds != m_target) {
        m_target = seconds;
        m_targetChanged = true;
        m_wake.wakeAll();
    }
}

void VideoDecoder::setUseGraphicsCard(bool use) {
    QMutexLocker locker(&m_mutex);
    if (use == m_useGraphicsCard) {
        return;
    }
    m_useGraphicsCard = use;
    if (!m_requestedPath.isEmpty()) {
        m_pathChanged = true; // open it again the new way
        m_wake.wakeAll();
    }
}

QImage VideoDecoder::frame(quint64* pSerial) const {
    QMutexLocker locker(&m_mutex);
    if (pSerial) {
        *pSerial = m_serial;
    }
    return m_frame;
}

VideoDecoder::State VideoDecoder::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

VideoDecoder::Stats VideoDecoder::takeStats() {
    QMutexLocker locker(&m_mutex);
    const Stats stats = m_stats;
    m_stats = Stats();
    return stats;
}

void VideoDecoder::run() {
    // The backends live on this thread only (the Windows decoder needs that).
    std::unique_ptr<Backend> pBackend;
    Backend::Frame candidate; // latest decoded frame at or before the target
    Backend::Frame pending;   // decoded frame after the target (shown later)
    double shownSec = -1e9;
    bool moreWork = false; // the last step stopped before reaching the target
    // The last pictures shown, in order, for loops and jumps back.
    std::deque<RecentPicture> recent;
    std::size_t recentBytes = 0;
    // After a seek: the pictures from the key frame up to the target are
    // not shown, unless getting there takes too long.
    bool catchingUp = false;
    bool described = false; // said where it decodes (after the first picture)
    auto catchUpStarted = std::chrono::steady_clock::now();
    constexpr double kMaxCatchUpMs = 400.0;

    const auto publish = [this](QImage image) {
        QMutexLocker locker(&m_mutex);
        if (!m_pathChanged) {
            m_frame = std::move(image);
            ++m_serial;
        }
    };

    for (;;) {
        QString path;
        bool pathChanged = false;
        bool useGraphicsCard = true;
        double target = 0.0;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_stop && !m_pathChanged && !m_targetChanged && !moreWork) {
                m_wake.wait(&m_mutex, 20);
            }
            if (m_stop) {
                break;
            }
            pathChanged = m_pathChanged;
            m_pathChanged = false;
            path = m_requestedPath;
            target = m_target;
            m_targetChanged = false;
            useGraphicsCard = m_useGraphicsCard;
        }
        if (pathChanged) {
            pBackend.reset();
            candidate = Backend::Frame();
            pending = Backend::Frame();
            shownSec = -1e9;
            moreWork = false;
            recent.clear();
            recentBytes = 0;
            catchingUp = false;
            described = false;
            State state = State::Closed;
            if (!path.isEmpty()) {
                // The Windows decoder first (it has H.264/H.265), then FFmpeg.
                const std::string utf8 = path.toStdString();
                state = State::Failed;
                std::unique_ptr<Backend> candidates[] = {
                        video::makeMediaFoundationBackend(useGraphicsCard),
                        std::make_unique<FfmpegBackend>(m_name, useGraphicsCard),
                };
                for (auto& pCandidate : candidates) {
                    if (!pCandidate) {
                        continue;
                    }
                    const Backend::OpenResult result = pCandidate->open(utf8);
                    if (result == Backend::OpenResult::Video) {
                        pBackend = std::move(pCandidate);
                        state = State::Video;
                        break;
                    }
                    if (result == Backend::OpenResult::NoVideo) {
                        state = State::NoVideo;
                    }
                }
                if (pBackend) {
                    qInfo().noquote() << "Video:" << m_name << "opened" << path;
                }
            }
            QMutexLocker locker(&m_mutex);
            if (m_pathChanged) {
                continue; // already another file: open that one
            }
            m_state = state;
        }
        if (!pBackend) {
            moreWork = false;
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        const double frameSec = pBackend->frameSeconds();
        int seeks = 0;
        int shown = 0;
        int fromMemory = 0;

        // A loop or a jump back into the last pictures: show the kept one,
        // no decoding. Only inside them (with a later picture kept too);
        // at the end, decoding carries on as usual.
        if (!recent.empty() && target >= recent.front().seconds - 0.5 * frameSec) {
            auto after = std::upper_bound(recent.begin(),
                    recent.end(),
                    target + 0.5 * frameSec,
                    [](double t, const RecentPicture& p) { return t < p.seconds; });
            if (after != recent.begin() && after != recent.end()) {
                const RecentPicture& at = *(after - 1);
                // A gap (pictures skipped while decoding fast) is a miss.
                if (after->seconds - at.seconds <= 3.5 * frameSec) {
                    if (at.seconds != shownSec) {
                        const QImage image = toCanvasSize(at, m_maxWidth, m_maxHeight);
                        if (!image.isNull()) {
                            shownSec = at.seconds;
                            ++shown;
                            ++fromMemory;
                            publish(image);
                        }
                    }
                    moreWork = false;
                    QMutexLocker locker(&m_mutex);
                    m_stats.framesShown += shown;
                    m_stats.fromMemory += fromMemory;
                    continue;
                }
            }
        }

        // Backwards (a loop or a jump back), or far ahead: seek. Otherwise
        // decode forward to the target.
        const double decodedUpTo = std::max(candidate.seconds, pending.seconds);
        if ((candidate && target < candidate.seconds - 0.5 * frameSec) ||
                (!candidate && shownSec > -1e8 && target < shownSec - 0.5 * frameSec) ||
                target > decodedUpTo + kMaxDecodeAheadSec) {
            pBackend->seek(target);
            candidate = Backend::Frame();
            pending = Backend::Frame();
            recent.clear(); // no longer one piece with what comes next
            recentBytes = 0;
            catchingUp = true;
            catchUpStarted = started;
            ++seeks;
        }
        bool failed = false;
        moreWork = true;
        for (int n = 0; n < kMaxFramesPerStep; ++n) {
            if (!pending) {
                if (!pBackend->next(&pending, &failed)) {
                    pending = Backend::Frame();
                    moreWork = false;
                    break; // end of the video, or an error
                }
            }
            if (pending.seconds > target + 0.5 * frameSec) {
                moreWork = false;
                break; // not its time yet
            }
            candidate = std::move(pending);
            pending = Backend::Frame();
        }
        if (failed) {
            candidate = Backend::Frame();
            pending = Backend::Frame();
            recent.clear();
            recentBytes = 0;
            if (!pBackend->retryAnotherWay()) {
                qWarning() << "Video:" << m_name << "cannot decode the video of" << path;
                pBackend.reset();
                QMutexLocker locker(&m_mutex);
                if (!m_pathChanged) {
                    m_state = State::Failed;
                }
            } else {
                qInfo().noquote() << "Video:" << m_name << "carries on:"
                                  << QString::fromStdString(pBackend->description());
            }
            continue;
        }
        if (catchingUp && candidate) {
            const double waitedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - catchUpStarted)
                                            .count();
            if (candidate.seconds >= target - 3.5 * frameSec || !moreWork ||
                    waitedMs > kMaxCatchUpMs) {
                catchingUp = false;
            }
        }
        if (candidate && candidate.seconds != shownSec && !catchingUp) {
            Picture picture;
            RecentPicture decoded;
            if (pBackend->toPicture(candidate, &picture)) {
                decoded = toRecent(candidate.seconds, picture);
            }
            const QImage image = toCanvasSize(decoded, m_maxWidth, m_maxHeight);
            if (!image.isNull()) {
                shownSec = candidate.seconds;
                ++shown;
                publish(image);
                if (!described) {
                    // Now it is known where it decodes.
                    described = true;
                    qInfo().noquote() << "Video:" << m_name << "decoding:"
                                      << QString::fromStdString(pBackend->description());
                }
                // Keep it for loops and jumps back (in time order).
                if (!recent.empty() && decoded.seconds <= recent.back().seconds) {
                    recent.clear();
                    recentBytes = 0;
                }
                recentBytes += decoded.bytes();
                recent.push_back(std::move(decoded));
                while (recentBytes > kRecentPicturesBytes && recent.size() > 1) {
                    recentBytes -= recent.front().bytes();
                    recent.pop_front();
                }
            }
        }
        const double busyMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                                      .count();
        QMutexLocker locker(&m_mutex);
        m_stats.framesShown += shown;
        m_stats.seeks += seeks;
        m_stats.busyMs += busyMs;
    }
}
