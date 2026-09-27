#include "stems/stemengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QtDebug>
#include <algorithm>
#include <thread>
#include <vector>

#include "stems/stemmath.h"

QString stems::Engine::defaultFolder() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("stems-engine"));
}

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include "onnxruntime_c_api.h"

namespace stems {
namespace {

/// Older ONNX Runtime files work too: only functions they already have
/// are used.
constexpr uint32_t kApiVersion = 18;

std::wstring wide(const QString& text) {
    return QDir::toNativeSeparators(text).toStdWString();
}

/// The NVIDIA files depend on each other; load them all from their folder
/// (in several passes, so the order does not matter). Once loaded, ONNX
/// Runtime and cuDNN find them by name.
int preloadDlls(const QString& folder) {
    const QStringList files = QDir(folder).entryList({QStringLiteral("*.dll")}, QDir::Files);
    std::vector<bool> loaded(files.size(), false);
    int count = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < files.size(); ++i) {
            if (loaded[i]) {
                continue;
            }
            const std::wstring path = wide(QDir(folder).filePath(files[i]));
            if (LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                loaded[i] = true;
                ++count;
                progress = true;
            }
        }
    }
    for (int i = 0; i < files.size(); ++i) {
        if (!loaded[i]) {
            qInfo().noquote() << "Stems: could not load" << files[i];
        }
    }
    return count;
}

class OnnxEngine : public Engine {
  public:
    ~OnnxEngine() override {
        if (m_pApi) {
            if (m_pSession) {
                m_pApi->ReleaseSession(m_pSession);
            }
            if (m_pMemory) {
                m_pApi->ReleaseMemoryInfo(m_pMemory);
            }
            if (m_pEnv) {
                m_pApi->ReleaseEnv(m_pEnv);
            }
        }
        // The DLLs stay loaded (unloading NVIDIA's files is not safe).
    }

    bool init(const QString& folder, bool allowGraphicsCard, QString* pError) {
        const QString runtime = QDir(folder).filePath(QStringLiteral("onnxruntime.dll"));
        const QString model = QDir(folder).filePath(QStringLiteral("model/htdemucs.onnx"));
        if (!QFileInfo::exists(runtime) || !QFileInfo::exists(model)) {
            *pError = QStringLiteral("the stems engine is not installed (%1)")
                              .arg(QDir::toNativeSeparators(folder));
            return false;
        }
        const QString cuda = QDir(folder).filePath(QStringLiteral("cuda"));
        const bool haveCuda = allowGraphicsCard && QDir(cuda).exists();
        if (haveCuda) {
            qInfo() << "Stems: loaded" << preloadDlls(cuda) << "NVIDIA files";
        }
        const std::wstring runtimePath = wide(runtime);
        HMODULE hRuntime = LoadLibraryExW(
                runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hRuntime) {
            *pError = QStringLiteral("onnxruntime.dll could not be loaded (error %1)")
                              .arg(GetLastError());
            return false;
        }
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        const auto getApiBase = reinterpret_cast<GetApiBase>(
                reinterpret_cast<void*>(GetProcAddress(hRuntime, "OrtGetApiBase")));
        const OrtApiBase* pBase = getApiBase ? getApiBase() : nullptr;
        m_pApi = pBase ? pBase->GetApi(kApiVersion) : nullptr;
        if (!m_pApi) {
            *pError = QStringLiteral("onnxruntime.dll is too old or not ONNX Runtime");
            return false;
        }
        qInfo().noquote() << "Stems: ONNX Runtime" << pBase->GetVersionString();
        if (!check(m_pApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "mixxx-stems", &m_pEnv), pError) ||
                !check(m_pApi->CreateCpuMemoryInfo(
                               OrtArenaAllocator, OrtMemTypeDefault, &m_pMemory),
                        pError)) {
            return false;
        }
        const std::wstring modelPath = wide(model);
        if (haveCuda) {
            QString why;
            if (createSession(modelPath, true, &why) && warmUp(&why)) {
                m_device = QStringLiteral("graphics card (CUDA)");
                return true;
            }
            qInfo().noquote() << "Stems: the graphics card could not be used:" << why
                              << "- using the processor";
            if (m_pSession) {
                m_pApi->ReleaseSession(m_pSession);
                m_pSession = nullptr;
            }
        }
        if (!createSession(modelPath, false, pError) || !warmUp(pError)) {
            return false;
        }
        m_device = QStringLiteral("processor (%1 threads)").arg(m_threads);
        return true;
    }

    bool run(const float* pInput, float* pOutput, QString* pError) override {
        const int64_t inShape[3] = {1, 2, kModelSegment};
        const int64_t outShape[4] = {1, kStemCount, 2, kModelSegment};
        const size_t inBytes = sizeof(float) * 2 * kModelSegment;
        const size_t outBytes = inBytes * kStemCount;
        OrtValue* pIn = nullptr;
        OrtValue* pOut = nullptr;
        bool ok = check(m_pApi->CreateTensorWithDataAsOrtValue(m_pMemory,
                                const_cast<float*>(pInput),
                                inBytes,
                                inShape,
                                3,
                                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                &pIn),
                          pError) &&
                check(m_pApi->CreateTensorWithDataAsOrtValue(m_pMemory,
                              pOutput,
                              outBytes,
                              outShape,
                              4,
                              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                              &pOut),
                        pError);
        if (ok) {
            const char* inNames[1] = {"input"};
            const char* outNames[1] = {"output"};
            ok = check(m_pApi->Run(m_pSession, nullptr, inNames, &pIn, 1, outNames, 1, &pOut),
                    pError);
        }
        if (pIn) {
            m_pApi->ReleaseValue(pIn);
        }
        if (pOut) {
            m_pApi->ReleaseValue(pOut);
        }
        return ok;
    }

    QString device() const override {
        return m_device;
    }

  private:
    bool check(OrtStatus* pStatus, QString* pError) {
        if (!pStatus) {
            return true;
        }
        if (pError) {
            *pError = QString::fromUtf8(m_pApi->GetErrorMessage(pStatus)).left(300);
        }
        m_pApi->ReleaseStatus(pStatus);
        return false;
    }

    bool createSession(const std::wstring& modelPath, bool graphicsCard, QString* pError) {
        OrtSessionOptions* pOptions = nullptr;
        if (!check(m_pApi->CreateSessionOptions(&pOptions), pError)) {
            return false;
        }
        bool ok = check(m_pApi->SetSessionGraphOptimizationLevel(pOptions, ORT_ENABLE_ALL), pError);
        // Never busy-wait: the audio engine needs the processor.
        ok = ok &&
                check(m_pApi->AddSessionConfigEntry(
                              pOptions, "session.intra_op.allow_spinning", "0"),
                        pError);
        if (graphicsCard) {
            ok = ok && check(m_pApi->SetIntraOpNumThreads(pOptions, 2), pError);
            OrtCUDAProviderOptionsV2* pCuda = nullptr;
            ok = ok && check(m_pApi->CreateCUDAProviderOptions(&pCuda), pError);
            if (ok) {
                const char* keys[1] = {"device_id"};
                const char* values[1] = {"0"}; // CUDA counts NVIDIA chips only
                ok = check(m_pApi->UpdateCUDAProviderOptions(pCuda, keys, values, 1), pError) &&
                        check(m_pApi->SessionOptionsAppendExecutionProvider_CUDA_V2(
                                      pOptions, pCuda),
                                pError);
            }
            if (pCuda) {
                m_pApi->ReleaseCUDAProviderOptions(pCuda);
            }
        } else {
            // Half the cores, so Mixxx and the audio keep running smoothly.
            m_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
            ok = ok && check(m_pApi->SetIntraOpNumThreads(pOptions, m_threads), pError);
        }
        ok = ok &&
                check(m_pApi->CreateSession(m_pEnv, modelPath.c_str(), pOptions, &m_pSession),
                        pError);
        m_pApi->ReleaseSessionOptions(pOptions);
        return ok;
    }

    bool warmUp(QString* pError) {
        // The first run prepares everything (several seconds on the
        // graphics card); errors show up here rather than in a song.
        std::vector<float> input(static_cast<size_t>(2) * kModelSegment, 0.0f);
        std::vector<float> output(static_cast<size_t>(kStemCount) * 2 * kModelSegment);
        return run(input.data(), output.data(), pError);
    }

    const OrtApi* m_pApi = nullptr;
    OrtEnv* m_pEnv = nullptr;
    OrtMemoryInfo* m_pMemory = nullptr;
    OrtSession* m_pSession = nullptr;
    QString m_device;
    int m_threads = 0;
};

} // namespace

std::unique_ptr<Engine> Engine::load(
        const QString& folder, bool allowGraphicsCard, QString* pError) {
    auto pEngine = std::make_unique<OnnxEngine>();
    if (!pEngine->init(folder, allowGraphicsCard, pError)) {
        return nullptr;
    }
    return pEngine;
}

} // namespace stems

#else

std::unique_ptr<stems::Engine> stems::Engine::load(const QString&, bool, QString* pError) {
    *pError = QStringLiteral("stem splitting is Windows only for now");
    return nullptr;
}

#endif
