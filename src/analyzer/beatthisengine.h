#pragma once

#include <QString>
#include <memory>
#include <mutex>
#include <vector>

#include "library/autodj/smart/beatcheck.h"

/// Auto DJ 2.0 plus Video Mixing beat check: runs the "Beat This!" model
/// (beatcheck.h) with Microsoft's ONNX Runtime.
///
/// The runtime is the one the stems engine brings (onnxruntime.dll in the
/// stems-engine folder), so nothing new needs installing; without it the
/// beat check is simply off. The models (MIT licence, see
/// res/beatcheck/LICENSE.txt):
///   res/beatcheck/mel_spectrogram.onnx   the sound as 128 mel bands, 50 a second
///   <stems-engine>/model/beat_this.onnx  beats and downbeats (83 MB, the best),
///   or res/beatcheck/beat_this_small.onnx (10 MB, nearly as good) without it.
/// It runs on the processor with two threads (5-15 s per song), so the
/// audio never stutters.
namespace beatcheck {

class Engine {
  public:
    ~Engine();

    /// The engine for this run of Mixxx, loaded on first use (and again
    /// after a failure only when the folders change). nullptr: off, with
    /// the reason.
    static std::shared_ptr<Engine> shared(const QString& runtimeFile,
            const QString& melModel,
            const QString& beatModel,
            QString* pError);

    /// Beats and downbeats of a song, from its mono audio at kSampleRate.
    bool analyse(const std::vector<float>& mono, Found* pFound, QString* pError);

  private:
    Engine() = default;
    bool init(const QString& runtimeFile,
            const QString& melModel,
            const QString& beatModel,
            QString* pError);
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;
    std::mutex m_mutex;
};

} // namespace beatcheck
