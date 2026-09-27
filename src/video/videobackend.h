#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// Auto DJ 2.0 video: one way of decoding a video file (Windows Media
/// Foundation, or FFmpeg). VideoDecoder drives it: open, read frames in
/// order, seek, and turn a frame into a picture. Plain C++ (no Qt) so the
/// Windows part can be checked on its own.
namespace video {

/// A decoded picture: 32-bit pixels in B, G, R, X byte order (what Qt calls
/// Format_RGB32), top row first.
struct Picture {
    int width = 0;
    int height = 0;
    int stride = 0; ///< bytes per row
    double displayAspect = 0.0; ///< width / height as shown (0 = width/height)
    std::vector<std::uint8_t> pixels;
};

class Backend {
  public:
    virtual ~Backend() = default;

    enum class OpenResult {
        Video,   ///< has a video stream this backend can decode
        NoVideo, ///< opened, but there is no video stream
        Failed,  ///< cannot open the file, or cannot decode its video
    };
    /// `path` is UTF-8.
    virtual OpenResult open(const std::string& path) = 0;
    /// For the log, e.g. "Windows decoder, h264 640x480 29.97 fps,
    /// graphics card". Where it decodes is only known for sure once a
    /// frame has been decoded.
    virtual std::string description() const = 0;
    virtual double frameSeconds() const = 0;

    /// A decoded frame, kept by the backend-specific handle.
    struct Frame {
        double seconds = -1.0; ///< from the start of the track's audio
        std::shared_ptr<void> handle;
        explicit operator bool() const {
            return static_cast<bool>(handle);
        }
    };
    /// The next frame. False at the end of the video; *pFailed is set on
    /// an error.
    virtual bool next(Frame* pFrame, bool* pFailed) = 0;
    /// Continue from the last key frame at or before `seconds`.
    virtual bool seek(double seconds) = 0;
    virtual bool toPicture(const Frame& frame, Picture* pPicture) = 0;
    /// After an error: try again another way (e.g. without the graphics
    /// card). False if there is no other way.
    virtual bool retryAnotherWay() {
        return false;
    }
};

/// The Windows decoder (Media Foundation: H.264, H.265 and whatever else
/// Windows can play). nullptr on other systems. Must be created, used and
/// destroyed on one thread. `useGraphicsCard`: let the graphics card decode
/// and convert the pictures (falls back to the processor if it cannot).
std::unique_ptr<Backend> makeMediaFoundationBackend(bool useGraphicsCard);

} // namespace video
