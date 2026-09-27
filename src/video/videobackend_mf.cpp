#include "video/videobackend.h"

#if defined(_WIN32) && defined(__MEDIAFOUNDATION__)

#ifndef NOMINMAX
#define NOMINMAX // keep std::min / std::max usable
#endif
#include <windows.h>
// windows.h first
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace video {
namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr double kHundredNanoseconds = 1e7;

// MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING (Windows 8 and later).
// Mixxx builds for Windows 7 (_WIN32_WINNT=0x0601), which hides the name,
// so it is spelled out here.
const GUID kAdvancedVideoProcessing = {
        0x0f81da2c, 0xb537, 0x4672, {0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3}};

template<typename T>
void safeRelease(T** ppObject) {
    if (*ppObject) {
        (*ppObject)->Release();
        *ppObject = nullptr;
    }
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
            static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            wide.data(), size);
    // Mixxx uses forward slashes; Windows paths use backslashes.
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    return wide;
}

std::string codecName(const GUID& subtype) {
    if (subtype == MFVideoFormat_H264) {
        return "h264";
    }
    if (subtype == MFVideoFormat_HEVC) {
        return "hevc";
    }
    if (subtype == MFVideoFormat_MP4V || subtype == MFVideoFormat_MP43 ||
            subtype == MFVideoFormat_M4S2) {
        return "mpeg4";
    }
    if (subtype == MFVideoFormat_MPG1 || subtype == MFVideoFormat_MPEG2) {
        return "mpeg";
    }
    if (subtype == MFVideoFormat_WMV1 || subtype == MFVideoFormat_WMV2 ||
            subtype == MFVideoFormat_WMV3 || subtype == MFVideoFormat_WVC1) {
        return "wmv";
    }
    return "other";
}

class MediaFoundationBackend : public Backend {
  public:
    MediaFoundationBackend() {
        m_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        m_hrStartup = MFStartup(MF_VERSION);
    }

    ~MediaFoundationBackend() override {
        close();
        if (SUCCEEDED(m_hrStartup)) {
            MFShutdown();
        }
        if (SUCCEEDED(m_hrCom)) {
            CoUninitialize();
        }
    }

    OpenResult open(const std::string& path) override {
        close();
        if (FAILED(m_hrStartup)) {
            return OpenResult::Failed;
        }
        IMFAttributes* pAttributes = nullptr;
        if (FAILED(MFCreateAttributes(&pAttributes, 1))) {
            return OpenResult::Failed;
        }
        // Lets the reader turn the video into RGB32 pictures for us.
        pAttributes->SetUINT32(kAdvancedVideoProcessing, TRUE);
        const std::wstring widePath = widen(path);
        HRESULT hr = MFCreateSourceReaderFromURL(widePath.c_str(), pAttributes, &m_pReader);
        safeRelease(&pAttributes);
        if (FAILED(hr) || !m_pReader) {
            return OpenResult::Failed;
        }
        m_pReader->SetStreamSelection(
                static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (FAILED(m_pReader->SetStreamSelection(kVideoStream, TRUE))) {
            close();
            return OpenResult::NoVideo;
        }
        IMFMediaType* pNative = nullptr;
        if (FAILED(m_pReader->GetNativeMediaType(kVideoStream, 0, &pNative)) || !pNative) {
            close();
            return OpenResult::NoVideo;
        }
        GUID subtype = {};
        pNative->GetGUID(MF_MT_SUBTYPE, &subtype);
        m_codec = codecName(subtype);
        UINT32 rateNum = 0;
        UINT32 rateDen = 0;
        if (SUCCEEDED(MFGetAttributeRatio(pNative, MF_MT_FRAME_RATE, &rateNum, &rateDen)) &&
                rateNum > 0 && rateDen > 0) {
            m_frameSeconds = static_cast<double>(rateDen) / rateNum;
        }
        if (!(m_frameSeconds > 0.001 && m_frameSeconds < 1.0)) {
            m_frameSeconds = 1.0 / 30.0;
        }
        safeRelease(&pNative);

        IMFMediaType* pOutput = nullptr;
        if (FAILED(MFCreateMediaType(&pOutput))) {
            close();
            return OpenResult::Failed;
        }
        pOutput->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOutput->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = m_pReader->SetCurrentMediaType(kVideoStream, nullptr, pOutput);
        safeRelease(&pOutput);
        if (FAILED(hr) || !readFormat()) {
            // Windows has no decoder for this video.
            close();
            return OpenResult::Failed;
        }
        return OpenResult::Video;
    }

    std::string description() const override {
        char text[160];
        std::snprintf(text,
                sizeof(text),
                "Windows decoder, %s %dx%d %.2f fps",
                m_codec.c_str(),
                m_cropWidth,
                m_cropHeight,
                1.0 / m_frameSeconds);
        return text;
    }

    double frameSeconds() const override {
        return m_frameSeconds;
    }

    bool next(Frame* pFrame, bool* pFailed) override {
        if (!m_pReader) {
            return false;
        }
        for (int attempts = 0; attempts < 1000; ++attempts) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* pSample = nullptr;
            const HRESULT hr = m_pReader->ReadSample(
                    kVideoStream, 0, &streamIndex, &flags, &timestamp, &pSample);
            if (FAILED(hr)) {
                *pFailed = true;
                return false;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                readFormat();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                safeRelease(&pSample);
                return false;
            }
            if (!pSample) {
                continue; // a gap in the stream: read on
            }
            pFrame->seconds = timestamp / kHundredNanoseconds;
            pFrame->handle = std::shared_ptr<void>(pSample, [](void* p) {
                static_cast<IMFSample*>(p)->Release();
            });
            return true;
        }
        *pFailed = true;
        return false;
    }

    bool seek(double seconds) override {
        if (!m_pReader) {
            return false;
        }
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>(std::max(0.0, seconds) * kHundredNanoseconds);
        const GUID timeFormat = {}; // GUID_NULL: 100-nanosecond units
        return SUCCEEDED(m_pReader->SetCurrentPosition(timeFormat, position));
    }

    bool toPicture(const Frame& frame, Picture* pPicture) override {
        auto* pSample = static_cast<IMFSample*>(frame.handle.get());
        if (!pSample || m_width <= 0 || m_height <= 0) {
            return false;
        }
        IMFMediaBuffer* pBuffer = nullptr;
        if (FAILED(pSample->ConvertToContiguousBuffer(&pBuffer)) || !pBuffer) {
            return false;
        }
        bool ok = false;
        IMF2DBuffer* p2D = nullptr;
        BYTE* pTopRow = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(pBuffer->QueryInterface(IID_PPV_ARGS(&p2D))) &&
                SUCCEEDED(p2D->Lock2D(&pTopRow, &pitch))) {
            ok = copyPicture(pTopRow, pitch, pPicture);
            p2D->Unlock2D();
        } else {
            BYTE* pData = nullptr;
            DWORD maxLength = 0;
            DWORD length = 0;
            if (SUCCEEDED(pBuffer->Lock(&pData, &maxLength, &length))) {
                // A negative stride means the rows are stored bottom-up.
                const LONG stride = m_stride != 0 ? m_stride : m_width * 4;
                BYTE* pFirst = stride < 0 ? pData + (m_height - 1) * static_cast<LONG>(-stride)
                                          : pData;
                if (length >= static_cast<DWORD>(std::abs(stride)) * m_height) {
                    ok = copyPicture(pFirst, stride, pPicture);
                }
                pBuffer->Unlock();
            }
        }
        safeRelease(&p2D);
        safeRelease(&pBuffer);
        return ok;
    }

  private:
    void close() {
        safeRelease(&m_pReader);
        m_width = m_height = 0;
        m_cropX = m_cropY = m_cropWidth = m_cropHeight = 0;
        m_stride = 0;
        m_aspect = 0.0;
    }

    /// The current output format: size, row stride, visible area, shape.
    bool readFormat() {
        IMFMediaType* pType = nullptr;
        if (FAILED(m_pReader->GetCurrentMediaType(kVideoStream, &pType)) || !pType) {
            return false;
        }
        UINT32 width = 0;
        UINT32 height = 0;
        MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
        m_width = static_cast<int>(width);
        m_height = static_cast<int>(height);
        UINT32 stride = 0;
        if (SUCCEEDED(pType->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
            m_stride = static_cast<LONG>(static_cast<INT32>(stride));
        } else {
            LONG computed = 0;
            MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &computed);
            m_stride = computed;
        }
        // The visible part (e.g. 1080 of 1088 coded rows).
        m_cropX = 0;
        m_cropY = 0;
        m_cropWidth = m_width;
        m_cropHeight = m_height;
        MFVideoArea area;
        if (SUCCEEDED(pType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                    reinterpret_cast<UINT8*>(&area),
                    sizeof(area),
                    nullptr))) {
            const int x = area.OffsetX.value;
            const int y = area.OffsetY.value;
            const int w = static_cast<int>(area.Area.cx);
            const int h = static_cast<int>(area.Area.cy);
            if (w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= m_width && y + h <= m_height) {
                m_cropX = x;
                m_cropY = y;
                m_cropWidth = w;
                m_cropHeight = h;
            }
        }
        UINT32 parNum = 1;
        UINT32 parDen = 1;
        m_aspect = 0.0;
        if (SUCCEEDED(MFGetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, &parNum, &parDen)) &&
                parNum > 0 && parDen > 0 && m_cropHeight > 0) {
            m_aspect = static_cast<double>(m_cropWidth) * parNum / (m_cropHeight * static_cast<double>(parDen));
        }
        safeRelease(&pType);
        return m_width > 0 && m_height > 0;
    }

    bool copyPicture(const BYTE* pTopRow, LONG pitch, Picture* pPicture) const {
        if (!pTopRow || pitch == 0) {
            return false;
        }
        pPicture->width = m_cropWidth;
        pPicture->height = m_cropHeight;
        pPicture->stride = m_cropWidth * 4;
        pPicture->displayAspect = m_aspect;
        pPicture->pixels.resize(static_cast<std::size_t>(pPicture->stride) * m_cropHeight);
        for (int y = 0; y < m_cropHeight; ++y) {
            const BYTE* pSource = pTopRow + static_cast<std::ptrdiff_t>(pitch) * (y + m_cropY) +
                    m_cropX * 4;
            std::memcpy(pPicture->pixels.data() + static_cast<std::size_t>(pPicture->stride) * y,
                    pSource,
                    static_cast<std::size_t>(pPicture->stride));
        }
        return true;
    }

    HRESULT m_hrCom = E_FAIL;
    HRESULT m_hrStartup = E_FAIL;
    IMFSourceReader* m_pReader = nullptr;
    std::string m_codec;
    double m_frameSeconds = 1.0 / 30.0;
    int m_width = 0;
    int m_height = 0;
    LONG m_stride = 0;
    int m_cropX = 0;
    int m_cropY = 0;
    int m_cropWidth = 0;
    int m_cropHeight = 0;
    double m_aspect = 0.0;
};

} // namespace

std::unique_ptr<Backend> makeMediaFoundationBackend() {
    return std::make_unique<MediaFoundationBackend>();
}

} // namespace video

#else // not Windows

namespace video {
std::unique_ptr<Backend> makeMediaFoundationBackend() {
    return nullptr;
}
} // namespace video

#endif
