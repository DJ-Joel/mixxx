#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QList>
#include <atomic>

#include "preferences/usersettings.h"
#include "soundio/soundmanagerutil.h"
#include "util/fifo.h"
#include "util/mutex.h"
#include "util/types.h"

class SideChainWorker;

class EngineSideChain : public QThread, public AudioDestination {
    Q_OBJECT
  public:
    EngineSideChain(UserSettingsPointer pConfig, CSAMPLE* sidechainMix);
    ~EngineSideChain() override;

    // Not thread-safe, wait-free. Submit buffer of samples to the sidechain for
    // processing. Should only be called from a single writer thread (typically
    // the engine callback).
    void writeSamples(const CSAMPLE* pBuffer, int iFrames);

    // Thin wrapper around writeSamples that is used by SoundManager when receiving
    // from a sound card input instead of the engine
    void receiveBuffer(const AudioInput& input,
            const CSAMPLE* pBuffer,
            unsigned int iFrames) override;

    // Thread-safe, blocking.
    void addSideChainWorker(SideChainWorker* pWorker);

    static constexpr int SIDECHAIN_BUFFER_SIZE = 65536;

    /// Auto DJ 2.0 plus Video Mixing: how many stereo frames the engine has
    /// handed to the sidechain so far (thread-safe). The video recorder uses
    /// it as its clock, so the picture stays in step with the recorded sound.
    qint64 framesWritten() const {
        return m_framesWritten.load(std::memory_order_acquire);
    }
    /// How many stereo frames the sidechain workers have been given so far.
    /// Inside SideChainWorker::process() it is the number of the first frame
    /// of that buffer (the frames are handed over in the order written).
    qint64 framesRead() const {
        return m_framesRead.load(std::memory_order_acquire);
    }

  private:
    void run() override;

    UserSettingsPointer m_pConfig;
    // Indicates that the thread should exit.
    volatile bool m_bStopThread;

    FIFO<CSAMPLE> m_sampleFifo;
    CSAMPLE* m_pWorkBuffer;
    CSAMPLE* m_pSidechainMix;

    // Provides thread safety around the wait condition below.
    QMutex m_waitLock;
    // Allows sleeping until we have samples to process.
    QWaitCondition m_waitForSamples;

    // Sidechain workers registered with EngineSideChain.
    MMutex m_workerLock;
    QList<SideChainWorker*> m_workers GUARDED_BY(m_workerLock);

    std::atomic<qint64> m_framesWritten{0};
    std::atomic<qint64> m_framesRead{0};
};
