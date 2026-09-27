#pragma once

#include <QString>
#include <cstdint>
#include <memory>

/// Auto DJ 2.0 plus Video Mixing: writes a stem file (".stem.mp4", the NI
/// stem format Mixxx plays): five AAC stereo tracks - the whole mix, then
/// drums, bass, other, vocals - plus the stem manifest. Uses the AAC
/// encoder built into Windows (Media Foundation).
namespace stems {

class StemFileWriter {
  public:
    static constexpr int kTracks = 5; ///< mix + four parts

    virtual ~StemFileWriter() = default;
    /// nullptr where there is no encoder (not Windows).
    static std::unique_ptr<StemFileWriter> create();

    /// 44100 or 48000 Hz only (AAC).
    virtual bool open(const QString& path, int sampleRate, QString* pError) = 0;
    /// `frames` more frames for each track: planar [5][2][frames] (the mix
    /// first, then the parts in the model's order).
    virtual bool write(const float* pPlanar, std::int64_t frames, QString* pError) = 0;
    /// Completes the file (and adds the manifest).
    virtual bool finish(QString* pError) = 0;
};

} // namespace stems
