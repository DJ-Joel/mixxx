#include "analyzer/beatthisengine.h"

#include <QDir>
#include <QFileInfo>
#include <QtDebug>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
// After windows.h
#include "onnxruntime_c_api.h"

namespace beatcheck {

namespace {
/// Older ONNX Runtime files work too: only functions they already have
/// are used (as for the stems engine).
constexpr uint32_t kApiVersion = 18;

#if defined(_WIN32)
std::wstring nativePath(const QString& path) {
    return QDir::toNativeSeparators(path).toStdWString();
}
#else
std::string nativePath(const QString& path) {
    return QDir::toNativeSeparators(path).toStdString();
}
#endif

const OrtApi* loadApi(const QString& runtimeFile, QString* pError) {
#if defined(_WIN32)
    const std::wstring path = nativePath(runtimeFile);
    HMODULE hRuntime = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hRuntime) {
        *pError = QStringLiteral("%1 could not be loaded (error %2)")
                          .arg(QDir::toNativeSeparators(runtimeFile))
                          .arg(GetLastError());
        return nullptr;
    }
    void* pGet = reinterpret_cast<void*>(GetProcAddress(hRuntime, "OrtGetApiBase"));
#else
    void* hRuntime = dlopen(nativePath(runtimeFile).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!hRuntime) {
        *pError = QStringLiteral("%1 could not be loaded (%2)")
                          .arg(runtimeFile, QString::fromUtf8(dlerror()));
        return nullptr;
    }
    void* pGet = dlsym(hRuntime, "OrtGetApiBase");
#endif
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto getApiBase = reinterpret_cast<GetApiBase>(pGet);
    const OrtApiBase* pBase = getApiBase ? getApiBase() : nullptr;
    const OrtApi* pApi = pBase ? pBase->GetApi(kApiVersion) : nullptr;
    if (!pApi) {
        *pError = QStringLiteral("%1 is too old or not ONNX Runtime").arg(runtimeFile);
        return nullptr;
    }
    qInfo().noquote() << "Beat check: ONNX Runtime" << pBase->GetVersionString();
    return pApi;
}
} // namespace

struct Engine::Impl {
    const OrtApi* pApi = nullptr;
    OrtEnv* pEnv = nullptr;
    OrtMemoryInfo* pMemory = nullptr;
    OrtSession* pMel = nullptr;
    OrtSession* pBeat = nullptr;

    ~Impl() {
        if (!pApi) {
            return;
        }
        if (pBeat) {
            pApi->ReleaseSession(pBeat);
        }
        if (pMel) {
            pApi->ReleaseSession(pMel);
        }
        if (pMemory) {
            pApi->ReleaseMemoryInfo(pMemory);
        }
        if (pEnv) {
            pApi->ReleaseEnv(pEnv);
        }
    }

    bool check(OrtStatus* pStatus, QString* pError) const {
        if (!pStatus) {
            return true;
        }
        if (pError) {
            *pError = QString::fromUtf8(pApi->GetErrorMessage(pStatus)).left(300);
        }
        pApi->ReleaseStatus(pStatus);
        return false;
    }

    bool createSession(const QString& model, OrtSession** ppSession, QString* pError) const {
        OrtSessionOptions* pOptions = nullptr;
        if (!check(pApi->CreateSessionOptions(&pOptions), pError)) {
            return false;
        }
        bool ok = check(pApi->SetSessionGraphOptimizationLevel(pOptions, ORT_ENABLE_ALL), pError);
        // Two threads, never busy-waiting: the audio engine needs the processor.
        ok = ok && check(pApi->SetIntraOpNumThreads(pOptions, 2), pError);
        ok = ok && check(pApi->SetInterOpNumThreads(pOptions, 1), pError);
        ok = ok &&
                check(pApi->AddSessionConfigEntry(
                              pOptions, "session.intra_op.allow_spinning", "0"),
                        pError);
        const auto path = nativePath(model);
        ok = ok && check(pApi->CreateSession(pEnv, path.c_str(), pOptions, ppSession), pError);
        pApi->ReleaseSessionOptions(pOptions);
        return ok;
    }

    /// Runs `pSession` on one float tensor; the outputs are copied out.
    bool run(OrtSession* pSession,
            const char* inputName,
            float* pData,
            const std::vector<int64_t>& shape,
            const std::vector<const char*>& outputNames,
            std::vector<std::vector<float>>* pOutputs,
            std::vector<std::vector<int64_t>>* pShapes,
            QString* pError) const {
        std::size_t count = 1;
        for (int64_t d : shape) {
            count *= static_cast<std::size_t>(d);
        }
        OrtValue* pIn = nullptr;
        if (!check(pApi->CreateTensorWithDataAsOrtValue(pMemory,
                           pData,
                           count * sizeof(float),
                           shape.data(),
                           shape.size(),
                           ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                           &pIn),
                    pError)) {
            return false;
        }
        std::vector<OrtValue*> outs(outputNames.size(), nullptr);
        const char* inNames[1] = {inputName};
        bool ok = check(pApi->Run(pSession,
                                nullptr,
                                inNames,
                                &pIn,
                                1,
                                outputNames.data(),
                                outputNames.size(),
                                outs.data()),
                pError);
        pOutputs->assign(outs.size(), {});
        pShapes->assign(outs.size(), {});
        for (std::size_t i = 0; ok && i < outs.size(); ++i) {
            OrtTensorTypeAndShapeInfo* pInfo = nullptr;
            ok = check(pApi->GetTensorTypeAndShape(outs[i], &pInfo), pError);
            std::size_t dims = 0;
            ok = ok && check(pApi->GetDimensionsCount(pInfo, &dims), pError);
            std::vector<int64_t> outShape(dims);
            ok = ok && check(pApi->GetDimensions(pInfo, outShape.data(), dims), pError);
            std::size_t elements = 0;
            ok = ok && check(pApi->GetTensorShapeElementCount(pInfo, &elements), pError);
            if (pInfo) {
                pApi->ReleaseTensorTypeAndShapeInfo(pInfo);
            }
            float* pOut = nullptr;
            ok = ok && check(pApi->GetTensorMutableData(outs[i], reinterpret_cast<void**>(&pOut)),
                               pError);
            if (ok) {
                (*pOutputs)[i].assign(pOut, pOut + elements);
                (*pShapes)[i] = outShape;
            }
        }
        for (OrtValue* pOut : outs) {
            if (pOut) {
                pApi->ReleaseValue(pOut);
            }
        }
        pApi->ReleaseValue(pIn);
        return ok;
    }
};

Engine::~Engine() = default;

std::shared_ptr<Engine> Engine::shared(const QString& runtimeFile,
        const QString& melModel,
        const QString& beatModel,
        QString* pError) {
    static std::mutex s_mutex;
    static std::shared_ptr<Engine> s_pEngine;
    static QString s_key;
    static QString s_error;
    const std::lock_guard<std::mutex> lock(s_mutex);
    const QString key = runtimeFile + QChar('|') + melModel + QChar('|') + beatModel;
    if (key == s_key) {
        if (!s_pEngine && pError) {
            *pError = s_error;
        }
        return s_pEngine;
    }
    s_key = key;
    s_error.clear();
    std::shared_ptr<Engine> pEngine(new Engine());
    if (pEngine->init(runtimeFile, melModel, beatModel, &s_error)) {
        s_pEngine = pEngine;
    } else {
        s_pEngine.reset();
        if (pError) {
            *pError = s_error;
        }
    }
    return s_pEngine;
}

bool Engine::init(const QString& runtimeFile,
        const QString& mel,
        const QString& beat,
        QString* pError) {
    if (!QFileInfo::exists(runtimeFile)) {
        *pError = QStringLiteral("ONNX Runtime is not installed (%1; it comes with the stems "
                                 "engine)")
                          .arg(QDir::toNativeSeparators(runtimeFile));
        return false;
    }
    if (!QFileInfo::exists(mel) || !QFileInfo::exists(beat)) {
        *pError = QStringLiteral("the beat check model is missing (%1, %2)")
                          .arg(QDir::toNativeSeparators(mel), QDir::toNativeSeparators(beat));
        return false;
    }
    auto pImpl = std::make_unique<Impl>();
    pImpl->pApi = loadApi(runtimeFile, pError);
    if (!pImpl->pApi) {
        return false;
    }
    if (!pImpl->check(pImpl->pApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "mixxx-beatcheck",
                              &pImpl->pEnv),
                pError) ||
            !pImpl->check(pImpl->pApi->CreateCpuMemoryInfo(
                                  OrtArenaAllocator, OrtMemTypeDefault, &pImpl->pMemory),
                    pError) ||
            !pImpl->createSession(mel, &pImpl->pMel, pError) ||
            !pImpl->createSession(beat, &pImpl->pBeat, pError)) {
        return false;
    }
    m_pImpl = std::move(pImpl);
    return true;
}

bool Engine::analyse(const std::vector<float>& mono, Found* pFound, QString* pError) {
    const std::lock_guard<std::mutex> lock(m_mutex); // one song at a time
    if (!m_pImpl || mono.empty()) {
        if (pError) {
            *pError = QStringLiteral("no engine or no audio");
        }
        return false;
    }
    // The sound as mel bands, the whole song at once (as Beat This does).
    std::vector<float> audio = mono;
    std::vector<std::vector<float>> outputs;
    std::vector<std::vector<int64_t>> shapes;
    if (!m_pImpl->run(m_pImpl->pMel,
                "audio_pcm",
                audio.data(),
                {1, static_cast<int64_t>(audio.size())},
                {"mel_spectrogram"},
                &outputs,
                &shapes,
                pError)) {
        return false;
    }
    if (shapes[0].size() != 3 || shapes[0][2] != kMelBands) {
        *pError = QStringLiteral("unexpected mel spectrogram shape");
        return false;
    }
    const int frames = static_cast<int>(shapes[0][1]);
    const std::vector<float> spect = std::move(outputs[0]);
    // The beat model, 30 s at a time.
    Aggregator aggregator(frames);
    std::vector<float> chunk;
    for (int start : chunkStarts(frames)) {
        const int length = chunkInput(spect, frames, start, &chunk);
        if (length <= 2 * kBorderFrames) {
            continue;
        }
        if (!m_pImpl->run(m_pImpl->pBeat,
                    "spectrogram",
                    chunk.data(),
                    {1, length, kMelBands},
                    {"beat", "downbeat"},
                    &outputs,
                    &shapes,
                    pError)) {
            return false;
        }
        if (static_cast<int>(outputs[0].size()) != length ||
                static_cast<int>(outputs[1].size()) != length) {
            *pError = QStringLiteral("unexpected beat model output");
            return false;
        }
        aggregator.add(start, outputs[0].data(), outputs[1].data(), length);
    }
    *pFound = peaks(aggregator.beat(), aggregator.downbeat());
    return true;
}

} // namespace beatcheck
