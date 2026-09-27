#pragma once

#include <QString>
#include <memory>

/// Auto DJ 2.0 plus Video Mixing: the AI model that splits music into
/// drums, bass, other and vocals (Demucs v4 run by Microsoft's ONNX
/// Runtime).
///
/// Nothing of it is built into Mixxx: it is loaded at run time from the
/// "stems engine" folder, so Mixxx starts and works normally without it:
///   <folder>/onnxruntime.dll (+ onnxruntime_providers_*.dll)
///   <folder>/cuda/*.dll          NVIDIA CUDA + cuDNN (optional)
///   <folder>/model/htdemucs.onnx the model
/// With the NVIDIA files it runs on the graphics card, otherwise on the
/// processor (slower, and it only uses half the processor cores so the
/// audio never stutters).
namespace stems {

class Engine {
  public:
    virtual ~Engine() = default;

    /// Loads the engine, or returns nullptr with the reason.
    static std::unique_ptr<Engine> load(
            const QString& folder, bool allowGraphicsCard, QString* pError);
    /// The default folder: "stems-engine" next to mixxx.exe.
    static QString defaultFolder();

    /// One model run: kModelSegment stereo frames in ([2][segment],
    /// normalised), four parts out ([4][2][segment]).
    virtual bool run(const float* pInput, float* pOutput, QString* pError) = 0;
    /// "graphics card (CUDA)" or "processor".
    virtual QString device() const = 0;
};

} // namespace stems
