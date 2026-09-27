#include "video/videoaudiotap.h"

#include <mutex>
#include <utility>

#include "engine/sidechain/enginesidechain.h"

namespace {

std::mutex s_mutex;
EngineSideChain* s_pSideChain = nullptr;
VideoAudioTap::Sink s_sink;

} // namespace

VideoAudioTap::VideoAudioTap(EngineSideChain* pSideChain) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_pSideChain = pSideChain;
}

VideoAudioTap::~VideoAudioTap() {
    shutdown();
}

void VideoAudioTap::process(const CSAMPLE* pBuffer, const std::size_t bufferSize) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_sink || !s_pSideChain) {
        return;
    }
    // Called from the side-channel thread before it counts this buffer, so
    // this is the number of the buffer's first frame.
    s_sink(pBuffer, bufferSize, s_pSideChain->framesRead());
}

void VideoAudioTap::shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_pSideChain = nullptr;
}

qint64 VideoAudioTap::engineFrames() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_pSideChain ? s_pSideChain->framesWritten() : -1;
}

void VideoAudioTap::setSink(Sink sink) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_sink = std::move(sink);
}
