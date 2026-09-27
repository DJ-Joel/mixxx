#include "video/videobackend.h"

#if defined(_WIN32) && defined(__MEDIAFOUNDATION__)

// Mixxx builds for Windows 7 (_WIN32_WINNT=0x0601), which hides the
// Windows 8+ names used for graphics-card decoding. This file raises it for
// itself (it is built without the precompiled header, see CMakeLists.txt);
// the Windows 8+ functions are looked up at run time, so Mixxx still starts
// on older Windows and just decodes on the processor there.
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#if defined(WINVER) && WINVER < 0x0A00
#undef WINVER
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#ifndef NOMINMAX
#define NOMINMAX // keep std::min / std::max usable
#endif
#include <windows.h>
// windows.h first
#include <d3d10_1.h> // (not d3d10.h: the SDK wants d3d10_1.h first)
#include <d3d11.h>
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

// Spelled out, so nothing depends on which GUIDs the import libraries have.
// MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING (Windows 8 and later)
const GUID kAdvancedVideoProcessing = {
        0x0f81da2c, 0xb537, 0x4672, {0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3}};
// MF_SOURCE_READER_D3D_MANAGER
const GUID kD3DManager = {
        0xec822da2, 0xe1e9, 0x4b29, {0xa0, 0xd8, 0x56, 0x3c, 0x71, 0x9f, 0x52, 0x69}};
// MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS
const GUID kHardwareTransforms = {
        0xa634a91c, 0x822b, 0x41b9, {0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0}};

using CreateDxgiDeviceManager = HRESULT(WINAPI*)(UINT*, IMFDXGIDeviceManager**);

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
    // Video subtypes are a four-letter code in a common GUID.
    if (subtype.Data1 == MAKEFOURCC('V', 'P', '9', '0')) {
        return "vp9";
    }
    if (subtype.Data1 == MAKEFOURCC('A', 'V', '0', '1')) {
        return "av1";
    }
    return "other";
}

class MediaFoundationBackend : public Backend {
  public:
    explicit MediaFoundationBackend(bool useGraphicsCard)
            : m_useGraphicsCard(useGraphicsCard) {
        m_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        m_hrStartup = MFStartup(MF_VERSION);
    }

    ~MediaFoundationBackend() override {
        close();
        releaseGraphicsCard();
        if (SUCCEEDED(m_hrStartup)) {
            MFShutdown();
        }
        if (SUCCEEDED(m_hrCom)) {
            CoUninitialize();
        }
    }

    OpenResult open(const std::string& path) override {
        m_path = path;
        const OpenResult result = openReader(m_useGraphicsCard && setUpGraphicsCard());
        if (result == OpenResult::Failed && m_pDeviceManager) {
            // The graphics card could not do it: the processor then.
            releaseGraphicsCard();
            return openReader(false);
        }
        return result;
    }

    bool retryAnotherWay() override {
        if (!m_onGraphicsCard) {
            return false;
        }
        // Decoding on the graphics card failed part way: carry on with the
        // processor from the same place.
        const double resumeSec = m_lastSeconds;
        releaseGraphicsCard();
        if (openReader(false) != OpenResult::Video) {
            return false;
        }
        if (resumeSec > 0.0) {
            seek(resumeSec);
        }
        return true;
    }

  private:
    /// A Direct3D 11 device with video support, shared with the reader.
    bool setUpGraphicsCard() {
        releaseGraphicsCard();
        static const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        const HMODULE mfplat = GetModuleHandleW(L"mfplat.dll");
        if (!d3d11 || !mfplat) {
            return false;
        }
        const auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
                reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")));
        const auto createManager = reinterpret_cast<CreateDxgiDeviceManager>(
                reinterpret_cast<void*>(GetProcAddress(mfplat, "MFCreateDXGIDeviceManager")));
        if (!createDevice || !createManager) {
            return false; // Windows 7
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = createDevice(nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                D3D11_SDK_VERSION,
                &m_pDevice,
                nullptr,
                nullptr);
        if (FAILED(hr) || !m_pDevice) {
            releaseGraphicsCard();
            return false;
        }
        // The reader uses the device from its own threads.
        ID3D10Multithread* pMultithread = nullptr;
        if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(&pMultithread)))) {
            pMultithread->SetMultithreadProtected(TRUE);
            safeRelease(&pMultithread);
        }
        UINT resetToken = 0;
        if (FAILED(createManager(&resetToken, &m_pDeviceManager)) || !m_pDeviceManager ||
                FAILED(m_pDeviceManager->ResetDevice(m_pDevice, resetToken))) {
            releaseGraphicsCard();
            return false;
        }
        return true;
    }

    void releaseGraphicsCard() {
        close(); // the reader holds the manager
        safeRelease(&m_pDeviceManager);
        safeRelease(&m_pDevice);
    }

    OpenResult openReader(bool withGraphicsCard) {
        close();
        if (FAILED(m_hrStartup)) {
            return OpenResult::Failed;
        }
        IMFAttributes* pAttributes = nullptr;
        if (FAILED(MFCreateAttributes(&pAttributes, 3))) {
            return OpenResult::Failed;
        }
        // Lets the reader turn the video into RGB32 pictures for us (on the
        // graphics card when it has one).
        pAttributes->SetUINT32(kAdvancedVideoProcessing, TRUE);
        if (withGraphicsCard && m_pDeviceManager) {
            pAttributes->SetUnknown(kD3DManager, m_pDeviceManager);
            pAttributes->SetUINT32(kHardwareTransforms, TRUE);
        }
        const std::wstring widePath = widen(m_path);
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

  public:
    std::string description() const override {
        char text[200];
        std::snprintf(text,
                sizeof(text),
                "Windows decoder, %s %dx%d %.2f fps, %s",
                m_codec.c_str(),
                m_cropWidth,
                m_cropHeight,
                1.0 / m_frameSeconds,
                m_onGraphicsCard ? "graphics card"
                                 : (m_pDeviceManager ? "processor (the graphics card "
                                                       "does not decode this video)"
                                                     : "processor"));
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
            m_lastSeconds = pFrame->seconds;
            if (m_pDeviceManager && !m_checkedWhere) {
                // A picture in graphics-card memory = decoded there.
                m_checkedWhere = true;
                IMFMediaBuffer* pBuffer = nullptr;
                IMFDXGIBuffer* pDxgi = nullptr;
                if (SUCCEEDED(pSample->GetBufferByIndex(0, &pBuffer)) && pBuffer &&
                        SUCCEEDED(pBuffer->QueryInterface(IID_PPV_ARGS(&pDxgi)))) {
                    m_onGraphicsCard = true;
                }
                safeRelease(&pDxgi);
                safeRelease(&pBuffer);
            }
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
        m_onGraphicsCard = false;
        m_checkedWhere = false;
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

    const bool m_useGraphicsCard;
    HRESULT m_hrCom = E_FAIL;
    HRESULT m_hrStartup = E_FAIL;
    ID3D11Device* m_pDevice = nullptr;
    IMFDXGIDeviceManager* m_pDeviceManager = nullptr;
    bool m_onGraphicsCard = false; // decoded pictures are in graphics-card memory
    bool m_checkedWhere = false;
    double m_lastSeconds = 0.0;
    std::string m_path;
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

std::unique_ptr<Backend> makeMediaFoundationBackend(bool useGraphicsCard) {
    return std::make_unique<MediaFoundationBackend>(useGraphicsCard);
}

} // namespace video

#else // not Windows

namespace video {
std::unique_ptr<Backend> makeMediaFoundationBackend(bool) {
    return nullptr;
}
} // namespace video

#endif
