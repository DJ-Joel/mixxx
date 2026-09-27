#pragma once

#include <QtGlobal>
#include <cstddef>
#include <functional>

#include "engine/sidechain/sidechainworker.h"

class EngineSideChain;

/// Auto DJ 2.0 plus Video Mixing: hands the recording mix (the same sound
/// Mixxx's own "Record" button saves) to the video recorder.
///
/// It is one more worker on Mixxx's recording side channel, so the audio
/// engine is not touched. There is only one side channel, so the tap is
/// reached through static functions.
class VideoAudioTap : public SideChainWorker {
  public:
    /// Called on the side-channel thread with interleaved stereo samples
    /// and the number of the first stereo frame in them (counted from
    /// when Mixxx started).
    using Sink = std::function<void(
            const float* pSamples, std::size_t sampleCount, qint64 firstFrame)>;

    explicit VideoAudioTap(EngineSideChain* pSideChain);
    ~VideoAudioTap() override;

    void process(const CSAMPLE* pBuffer, const std::size_t bufferSize) override;
    void shutdown() override;

    /// Stereo frames the engine has made so far (the clock the recorded
    /// pictures are stamped with), or -1 when there is no side channel.
    static qint64 engineFrames();
    /// Who gets the sound. An empty function = nobody. When this returns,
    /// the old sink is no longer being called.
    static void setSink(Sink sink);
};
