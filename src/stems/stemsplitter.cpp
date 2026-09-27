#include "stems/stemsplitter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtDebug>
#include <algorithm>
#include <vector>

#include "moc_stemsplitter.cpp"
#include "sources/soundsourceproxy.h"
#include "stems/stemengine.h"
#include "stems/stemfilewriter.h"
#include "stems/stemmath.h"
#include "track/track.h"
#include "util/samplebuffer.h"

namespace {

const QString kStemsGroup = QStringLiteral("[Stems]");
constexpr int kReadChunkFrames = 65536;

QString minutes(double seconds) {
    const int whole = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

StemSplitter::StemSplitter(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig) {
    m_folder = QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("stems"));
    m_enabled = pConfig->getValue(ConfigKey(kStemsGroup, "Enabled"), true);
    m_thread = std::thread([this]() {
        run();
    });
}

StemSplitter::~StemSplitter() {
    m_stop = true;
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool StemSplitter::isEnabled() const {
    return m_enabled;
}

void StemSplitter::setEnabled(bool enabled) {
    m_enabled = enabled;
    m_pConfig->setValue(ConfigKey(kStemsGroup, "Enabled"), enabled);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.clear();
    }
}

QString StemSplitter::targetFor(const TrackPointer& pTrack) const {
    const QString location = pTrack->getLocation();
    const QFileInfo info(location);
    // The name says which song; the code changes if the file changes.
    const QByteArray key = (location + QString::number(info.size()) +
            QString::number(info.lastModified().toSecsSinceEpoch()))
                                   .toUtf8();
    const QString code = QString::fromLatin1(
            QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(10));
    QString name = pTrack->getArtist().trimmed();
    const QString title = pTrack->getTitle().trimmed();
    if (!name.isEmpty() && !title.isEmpty()) {
        name += QStringLiteral(" - ") + title;
    } else if (!title.isEmpty()) {
        name = title;
    } else if (name.isEmpty()) {
        name = info.completeBaseName();
    }
    static const QRegularExpression kUnsafe(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    name.replace(kUnsafe, QStringLiteral("_"));
    name = name.left(80).trimmed();
    return QDir(m_folder).filePath(QStringLiteral("%1 [%2].stem.mp4").arg(name, code));
}

QString StemSplitter::stemFile(const TrackPointer& pTrack) const {
    if (!pTrack) {
        return QString();
    }
    const QString target = targetFor(pTrack);
    return QFileInfo::exists(target) ? target : QString();
}

void StemSplitter::request(const TrackPointer& pTrack, bool urgent) {
    if (!pTrack || !m_enabled) {
        return;
    }
    const QString location = pTrack->getLocation();
    if (location.endsWith(QStringLiteral(".stem.mp4"), Qt::CaseInsensitive) ||
            location.endsWith(QStringLiteral(".stem.m4a"), Qt::CaseInsensitive) ||
            !QFileInfo::exists(location)) {
        return; // already a stem file, or not a file
    }
    const QString target = targetFor(pTrack);
    if (QFileInfo::exists(target)) {
        return; // split before
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_engineFailed || target == m_current) {
            return;
        }
        auto found = std::find_if(m_jobs.begin(), m_jobs.end(), [&target](const Job& job) {
            return job.target == target;
        });
        if (found != m_jobs.end()) {
            if (!urgent) {
                return;
            }
            m_jobs.erase(found);
        }
        Job job{pTrack, target};
        if (urgent) {
            m_jobs.push_front(std::move(job));
        } else {
            m_jobs.push_back(std::move(job));
        }
        while (static_cast<int>(m_jobs.size()) > kMaxWaiting) {
            m_jobs.pop_back(); // the least urgent; asked again later if needed
        }
    }
    m_wake.notify_one();
}

void StemSplitter::run() {
    while (!m_stop) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]() {
                return m_stop || !m_jobs.empty();
            });
            if (m_stop) {
                break;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            m_current = job.target;
        }
        split(job);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_current.clear();
        }
    }
    m_pEngine.reset();
}

bool StemSplitter::split(const Job& job) {
    if (!m_pEngine) {
        QString folder = m_pConfig->getValue(ConfigKey(kStemsGroup, "EngineFolder"), QString());
        if (folder.isEmpty()) {
            folder = stems::Engine::defaultFolder();
        }
        const bool graphicsCard = m_pConfig->getValue(ConfigKey(kStemsGroup, "GraphicsCard"), true);
        QElapsedTimer timer;
        timer.start();
        QString error;
        m_pEngine = stems::Engine::load(folder, graphicsCard, &error);
        if (!m_pEngine) {
            qInfo().noquote() << "Stems: off -" << error;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_engineFailed = true;
            m_jobs.clear();
            return false;
        }
        qInfo().noquote() << "Stems: ready on the" << m_pEngine->device() << "- start-up"
                          << QString::number(timer.elapsed() / 1000.0, 'f', 1) << "s";
    }
    QElapsedTimer timer;
    timer.start();
    const QString title = job.track->getInfo();

    // 1. The song's sound, exactly as the deck plays it.
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());
    mixxx::AudioSourcePointer pSource = SoundSourceProxy(job.track).openAudioSource(params);
    if (!pSource || pSource->getSignalInfo().getChannelCount() != 2) {
        qWarning().noquote() << "Stems: could not read" << job.track->getLocation();
        return false;
    }
    const int rate = static_cast<int>(pSource->getSignalInfo().getSampleRate());
    std::vector<float> interleaved;
    interleaved.reserve(static_cast<std::size_t>(pSource->frameIndexRange().length()) * 2);
    mixxx::SampleBuffer buffer(kReadChunkFrames * 2);
    mixxx::IndexRange remaining = pSource->frameIndexRange();
    while (!remaining.empty()) {
        if (m_stop) {
            return false;
        }
        const auto range = remaining.splitAndShrinkFront(
                std::min<SINT>(kReadChunkFrames, remaining.length()));
        const auto read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                range, mixxx::SampleBuffer::WritableSlice(buffer)));
        const CSAMPLE* pData = read.readableData();
        interleaved.insert(interleaved.end(), pData, pData + read.readableLength());
        remaining = intersect(remaining, pSource->frameIndexRange());
    }
    pSource.reset();
    const std::int64_t songFrames = static_cast<std::int64_t>(interleaved.size() / 2);
    if (songFrames < stems::kModelSampleRate) {
        qInfo().noquote() << "Stems: too short to split:" << title;
        return false;
    }
    const double seconds = static_cast<double>(songFrames) / rate;

    // 2. At the model's sample rate.
    std::vector<float> mix = stems::toPlanar(interleaved.data(), songFrames);
    std::vector<float>().swap(interleaved);
    std::int64_t frames = songFrames;
    if (rate != stems::kModelSampleRate) {
        std::vector<float> left = stems::resample(mix.data(), songFrames, rate, stems::kModelSampleRate);
        std::vector<float> right = stems::resample(
                mix.data() + songFrames, songFrames, rate, stems::kModelSampleRate);
        frames = static_cast<std::int64_t>(left.size());
        mix.assign(left.begin(), left.end());
        mix.insert(mix.end(), right.begin(), right.end());
    }
    float mean = 0.0f;
    float std = 1.0f;
    stems::meanAndStd(mix, &mean, &std);

    // 3. Split piece by piece, writing the finished part as we go.
    QDir().mkpath(m_folder);
    const QString partial = job.target + QStringLiteral(".part");
    QFile::remove(partial);
    auto pWriter = stems::StemFileWriter::create();
    QString error;
    if (!pWriter || !pWriter->open(partial, stems::kModelSampleRate, &error)) {
        qWarning().noquote() << "Stems: could not create the stem file:"
                             << (pWriter ? error : QStringLiteral("no encoder"));
        pWriter.reset();
        QFile::remove(partial);
        return false;
    }
    const auto pieces = stems::planPieces(frames);
    stems::Blender blender(frames);
    std::vector<float> input;
    std::vector<float> output(static_cast<std::size_t>(stems::kStemCount) * 2 * stems::kModelSegment);
    std::vector<float> parts;
    std::vector<float> chunk;
    std::int64_t done = 0;
    bool ok = true;
    for (std::size_t k = 0; k < pieces.size() && ok; ++k) {
        if (m_stop) {
            ok = false;
            error = QStringLiteral("Mixxx is closing");
            break;
        }
        blender.fillInput(pieces[k], mix, &input);
        for (float& sample : input) {
            sample = (sample - mean) / std;
        }
        // Silence outside the song must stay silence after normalising.
        const std::int64_t before = std::max<std::int64_t>(0, -pieces[k].start);
        const std::int64_t after = std::max<std::int64_t>(
                0, pieces[k].start + stems::kModelSegment - frames);
        for (int channel = 0; channel < 2; ++channel) {
            float* pChannel = input.data() + static_cast<std::size_t>(channel) * stems::kModelSegment;
            std::fill(pChannel, pChannel + before, 0.0f);
            std::fill(pChannel + stems::kModelSegment - after, pChannel + stems::kModelSegment, 0.0f);
        }
        if (!m_pEngine->run(input.data(), output.data(), &error)) {
            ok = false;
            break;
        }
        blender.add(pieces[k], output.data());
        const std::int64_t upTo = k + 1 < pieces.size() ? pieces[k + 1].offset : frames;
        const std::int64_t count = blender.flush(upTo, mean, std, &parts);
        // [mix L][mix R][drums L][drums R]...[vocals R], each `count` long.
        chunk.resize(static_cast<std::size_t>(stems::StemFileWriter::kTracks) * 2 * count);
        for (int channel = 0; channel < 2; ++channel) {
            std::copy(mix.begin() + channel * frames + done,
                    mix.begin() + channel * frames + done + count,
                    chunk.begin() + channel * count);
        }
        std::copy(parts.begin(), parts.end(), chunk.begin() + 2 * count);
        ok = pWriter->write(chunk.data(), count, &error);
        done += count;
    }
    ok = ok && pWriter->finish(&error);
    pWriter.reset();
    if (ok) {
        QFile::remove(job.target);
        ok = QFile::rename(partial, job.target);
        if (!ok) {
            error = QStringLiteral("could not rename the finished file");
        }
    }
    if (!ok) {
        QFile::remove(partial);
        qWarning().noquote() << "Stems: splitting" << title << "failed:" << error;
        return false;
    }
    const double took = timer.elapsed() / 1000.0;
    qInfo().noquote() << "Stems: split" << title << "(" << minutes(seconds) << ") in"
                      << QString::number(took, 'f', 1) << "s on the" << m_pEngine->device()
                      << "=" << QString::number(seconds / std::max(took, 0.001), 'f', 0)
                      << "x faster than playing" << (rate != stems::kModelSampleRate
                              ? QStringLiteral("(converted from %1 Hz)").arg(rate)
                              : QString())
                      << "->" << QDir::toNativeSeparators(job.target);
    emit stemsReady(job.track->getId(), job.target);
    return true;
}
