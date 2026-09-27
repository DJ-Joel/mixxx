// Built without the precompiled header (see CMakeLists.txt), so the Windows
// headers below get NOMINMAX before anything else includes them.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "stems/stemfilewriter.h"

#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <vector>

#include "stems/stemmath.h"

#if defined(_WIN32) && defined(__MEDIAFOUNDATION__)

#include <windows.h>
// windows.h first
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <string>

namespace stems {
namespace {

constexpr UINT32 kAacBytesPerSecond = 24000; // 192 kbit/s per track
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

class MediaFoundationStemWriter : public StemFileWriter {
  public:
    ~MediaFoundationStemWriter() override {
        release(m_pWriter);
        shutdown();
    }

    bool open(const QString& path, int sampleRate, QString* pError) override {
        m_path = path;
        m_sampleRate = sampleRate;
        m_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        m_hrStartup = MFStartup(MF_VERSION);
        if (FAILED(m_hrStartup)) {
            *pError = hresultText("Windows Media Foundation did not start", m_hrStartup);
            return false;
        }
        const std::wstring widePath = path.toStdWString();
        // Say it is an MP4: Windows otherwise goes by the file name ending,
        // and the file is written under a temporary name.
        IMFAttributes* pAttributes = nullptr;
        HRESULT hr = MFCreateAttributes(&pAttributes, 1);
        if (SUCCEEDED(hr)) {
            hr = pAttributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateSinkWriterFromURL(widePath.c_str(), nullptr, pAttributes, &m_pWriter);
        }
        release(pAttributes);
        if (FAILED(hr)) {
            *pError = hresultText("Could not create the stem file", hr);
            return false;
        }
        for (int track = 0; track < kTracks && SUCCEEDED(hr); ++track) {
            IMFMediaType* pOut = nullptr;
            IMFMediaType* pIn = nullptr;
            hr = MFCreateMediaType(&pOut);
            if (SUCCEEDED(hr)) {
                pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                pOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
                pOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                pOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSecond);
                hr = m_pWriter->AddStream(pOut, &m_streams[track]);
            }
            if (SUCCEEDED(hr)) {
                hr = MFCreateMediaType(&pIn);
            }
            if (SUCCEEDED(hr)) {
                pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                pIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
                pIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                pIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                pIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * 4);
                pIn->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                hr = m_pWriter->SetInputMediaType(m_streams[track], pIn, nullptr);
            }
            release(pOut);
            release(pIn);
        }
        if (SUCCEEDED(hr)) {
            hr = m_pWriter->BeginWriting();
        }
        if (FAILED(hr)) {
            *pError = hresultText("Windows has no usable AAC encoder", hr);
            release(m_pWriter);
            return false;
        }
        return true;
    }

    bool write(const float* pPlanar, std::int64_t frames, QString* pError) override {
        if (!m_pWriter || frames <= 0) {
            return m_pWriter != nullptr;
        }
        const LONGLONG start = m_written * kHundredNanoseconds / m_sampleRate;
        const LONGLONG end = (m_written + frames) * kHundredNanoseconds / m_sampleRate;
        const DWORD bytes = static_cast<DWORD>(frames * 4);
        for (int track = 0; track < kTracks; ++track) {
            const float* pLeft = pPlanar + static_cast<std::size_t>(track) * 2 * frames;
            const float* pRight = pLeft + frames;
            IMFMediaBuffer* pBuffer = nullptr;
            IMFSample* pSample = nullptr;
            HRESULT hr = MFCreateMemoryBuffer(bytes, &pBuffer);
            if (SUCCEEDED(hr)) {
                BYTE* pDest = nullptr;
                hr = pBuffer->Lock(&pDest, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    auto* pPcm = reinterpret_cast<std::int16_t*>(pDest);
                    for (std::int64_t i = 0; i < frames; ++i) {
                        pPcm[2 * i] = toPcm(pLeft[i]);
                        pPcm[2 * i + 1] = toPcm(pRight[i]);
                    }
                    pBuffer->Unlock();
                    hr = pBuffer->SetCurrentLength(bytes);
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
                hr = pSample->SetSampleDuration(end - start);
            }
            if (SUCCEEDED(hr)) {
                hr = m_pWriter->WriteSample(m_streams[track], pSample);
            }
            release(pSample);
            release(pBuffer);
            if (FAILED(hr)) {
                *pError = hresultText("Could not write the stem file", hr);
                return false;
            }
        }
        m_written += frames;
        return true;
    }

    bool finish(QString* pError) override {
        if (!m_pWriter) {
            return false;
        }
        const HRESULT hr = m_pWriter->Finalize();
        release(m_pWriter);
        shutdown();
        if (FAILED(hr)) {
            *pError = hresultText("Could not complete the stem file", hr);
            return false;
        }
        if (!addStemManifestToFile(m_path, stemManifest())) {
            *pError = QStringLiteral("Could not add the stem information to the file");
            return false;
        }
        return true;
    }

  private:
    static std::int16_t toPcm(float value) {
        return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
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

    QString m_path;
    int m_sampleRate = 44100;
    IMFSinkWriter* m_pWriter = nullptr;
    DWORD m_streams[kTracks] = {};
    std::int64_t m_written = 0;
    HRESULT m_hrCom = E_FAIL;
    HRESULT m_hrStartup = E_FAIL;
};

} // namespace

std::unique_ptr<StemFileWriter> StemFileWriter::create() {
    return std::make_unique<MediaFoundationStemWriter>();
}

} // namespace stems

#else

std::unique_ptr<stems::StemFileWriter> stems::StemFileWriter::create() {
    return nullptr;
}

#endif
