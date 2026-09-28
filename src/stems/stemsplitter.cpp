#include "stems/stemsplitter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QtDebug>
#include <algorithm>
#include <vector>

#include "moc_stemsplitter.cpp"
#include "sources/soundsourceproxy.h"
#include "stems/stemcache.h"
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

namespace {
StemSplitter* s_pInstance = nullptr;
} // namespace

StemSplitter* StemSplitter::instance() {
    return s_pInstance;
}

StemSplitter::StemSplitter(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig) {
    s_pInstance = this;
    m_enabled = pConfig->getValue(ConfigKey(kStemsGroup, "Enabled"), true);
    // Where the parts go: the DJ's folder (default: the settings folder) or
    // beside each song.
    stems::StemCache::setFolder(folder());
    stems::StemCache::setLocation(location());
    stems::StemCache::setPlaybackEnabled(
            pConfig->getValue(ConfigKey(kStemsGroup, "PlayStems"), true));
    m_thread = std::thread([this]() {
        run();
    });
}

StemSplitter::~StemSplitter() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
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
        m_batch.clear();
    }
}

QString StemSplitter::engineFolder() const {
    const QString folder = m_pConfig->getValue(ConfigKey(kStemsGroup, "EngineFolder"), QString());
    return folder.isEmpty() ? stems::Engine::defaultFolder() : folder;
}

bool StemSplitter::engineInstalled() const {
    return QFileInfo::exists(QDir(engineFolder()).filePath(QStringLiteral("onnxruntime.dll"))) &&
            QFileInfo::exists(QDir(engineFolder()).filePath(QStringLiteral("model/htdemucs.onnx")));
}

bool StemSplitter::canSplit(const TrackPointer& pTrack) {
    // The stem file keeps the song's own rate, and the AAC encoder only
    // takes 44.1 and 48 kHz (a 22 kHz song would not line up with its parts).
    const auto rate = pTrack ? pTrack->getSampleRate() : mixxx::audio::SampleRate();
    return pTrack && (!rate.isValid() || rate.value() == 44100 || rate.value() == 48000);
}

bool StemSplitter::needsSplit(const TrackPointer& pTrack) {
    if (!canSplit(pTrack)) {
        return false;
    }
    const QString location = pTrack->getLocation();
    return !location.endsWith(QStringLiteral(".stem.mp4"), Qt::CaseInsensitive) &&
            !location.endsWith(QStringLiteral(".stem.m4a"), Qt::CaseInsensitive) &&
            QFileInfo::exists(location) && stems::StemCache::readyFileFor(pTrack).isEmpty();
}

double StemSplitter::speed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_speedShared;
}

int StemSplitter::splitBatch(const QList<TrackPointer>& tracks) {
    int added = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_engineFailed) {
            return 0;
        }
        if (m_batchTotal == 0) {
            m_batchFailures.clear(); // a new list
        }
        for (const TrackPointer& pTrack : tracks) {
            if (!needsSplit(pTrack)) {
                continue;
            }
            const QString target = targetFor(pTrack);
            const bool known = target == m_current ||
                    std::any_of(m_batch.begin(), m_batch.end(), [&target](const Job& job) {
                        return job.target == target;
                    });
            if (!known) {
                m_batch.push_back(Job{pTrack, target});
                m_batchSeconds += std::max(0.0, pTrack->getDuration());
                ++added;
            }
        }
        m_batchTotal += added;
    }
    if (added > 0) {
        m_wake.notify_one();
    }
    return added;
}

void StemSplitter::stopBatch() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_currentFromBatch) {
            m_cancel = true;
        }
        m_batch.clear();
        m_batchSeconds = 0.0;
        m_batchTotal = 0;
        m_batchDone = 0;
        m_batchFailures.clear();
    }
    emit batchProgress(0, 0, 0.0);
}

QStringList StemSplitter::takeBatchFailures() {
    std::lock_guard<std::mutex> lock(m_mutex);
    QStringList failures;
    failures.swap(m_batchFailures);
    return failures;
}

bool StemSplitter::batchRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_batchTotal > 0 && m_batchDone < m_batchTotal;
}

QString StemSplitter::targetFor(const TrackPointer& pTrack) const {
    return stems::StemCache::fileFor(pTrack);
}

QString StemSplitter::stemFile(const TrackPointer& pTrack) const {
    return stems::StemCache::readyFileFor(pTrack);
}

QString StemSplitter::folder() const {
    QString chosen = m_pConfig->getValue(ConfigKey(kStemsGroup, "Folder"), QString());
    if (chosen.isEmpty()) {
        return m_pConfig->getSettingsPath();
    }
    // A "Mixxx Stems" folder itself was chosen: use it, do not make a
    // second "Mixxx Stems" inside it.
    QDir dir(chosen);
    if (dir.dirName().compare(stems::StemCache::folderName(), Qt::CaseInsensitive) == 0 &&
            dir.cdUp()) {
        chosen = dir.absolutePath();
    }
    return chosen;
}

void StemSplitter::setFolder(const QString& folder) {
    m_pConfig->setValue(ConfigKey(kStemsGroup, "Folder"), folder);
    stems::StemCache::setFolder(this->folder());
    qInfo().noquote() << "Stems: saving to"
                      << QDir::toNativeSeparators(QDir(this->folder()).filePath(
                                 stems::StemCache::folderName()));
}

stems::StemCache::Location StemSplitter::location() const {
    return m_pConfig->getValue(ConfigKey(kStemsGroup, "Location"), QString()) ==
                    QStringLiteral("songs")
            ? stems::StemCache::Location::NextToSong
            : stems::StemCache::Location::OneFolder;
}

void StemSplitter::setLocation(stems::StemCache::Location location) {
    const bool songs = location == stems::StemCache::Location::NextToSong;
    m_pConfig->setValue(ConfigKey(kStemsGroup, "Location"),
            songs ? QStringLiteral("songs") : QStringLiteral("folder"));
    stems::StemCache::setLocation(location);
    qInfo().noquote() << "Stems: saving" << (songs ? "next to each song" : "in one folder");
}

void StemSplitter::request(const TrackPointer& pTrack, bool urgent) {
    if (!pTrack || !m_enabled || !canSplit(pTrack)) {
        return;
    }
    const QString location = pTrack->getLocation();
    if (location.endsWith(QStringLiteral(".stem.mp4"), Qt::CaseInsensitive) ||
            location.endsWith(QStringLiteral(".stem.m4a"), Qt::CaseInsensitive) ||
            !QFileInfo::exists(location)) {
        return; // already a stem file, or not a file
    }
    if (!stems::StemCache::readyFileFor(pTrack).isEmpty()) {
        return; // split before
    }
    const QString target = targetFor(pTrack);
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
        bool fromBatch = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]() {
                return m_stop || !m_jobs.empty() || !m_batch.empty();
            });
            if (m_stop) {
                break;
            }
            // Songs in the decks and the Auto DJ queue always go first; the
            // STEM SPLIT list only when nothing else waits.
            fromBatch = m_jobs.empty();
            std::deque<Job>& from = fromBatch ? m_batch : m_jobs;
            job = std::move(from.front());
            from.pop_front();
            m_current = job.target;
            m_currentFromBatch = fromBatch;
            m_cancel = false;
        }
        // (A song asked for twice, e.g. loaded while in the list, is only
        // split once.)
        m_lastError.clear();
        const bool done = !stems::StemCache::readyFileFor(job.track).isEmpty() || split(job);
        int batchDone = 0;
        int batchTotal = 0;
        double secondsLeft = 0.0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_current.clear();
            m_currentFromBatch = false;
            m_speedShared = m_speed;
            if (fromBatch && m_batchTotal > 0) {
                if (!done && !m_cancel && !m_stop) {
                    m_batchFailures.append(job.track->getInfo() + QStringLiteral(": ") +
                            (m_lastError.isEmpty() ? tr("see the log") : m_lastError));
                }
                ++m_batchDone;
                m_batchSeconds = std::max(0.0, m_batchSeconds - job.track->getDuration());
                batchDone = m_batchDone;
                batchTotal = m_batchTotal;
                const double speed = m_speed > 0.0 ? m_speed : 3.0;
                secondsLeft = m_batchSeconds / speed;
                if (m_batchDone >= m_batchTotal) {
                    m_batchTotal = 0;
                    m_batchDone = 0;
                    m_batchSeconds = 0.0;
                }
            }
        }
        if (fromBatch && batchTotal > 0) {
            emit batchProgress(batchDone, batchTotal, secondsLeft);
        }
    }
    m_pEngine.reset();
}

bool StemSplitter::copyFile(const QString& from, const QString& to, QString* pError) {
    QFile in(from);
    QFile out(to);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) {
        *pError = QStringLiteral("cannot write to ") + QDir::toNativeSeparators(to);
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    QByteArray block;
    while (!(block = in.read(1 << 20)).isEmpty()) {
        if (m_stop || m_cancel) {
            *pError = QStringLiteral("stopped");
            return false;
        }
        if (timer.elapsed() > 5 * 60 * 1000) {
            *pError = QStringLiteral("copying took too long");
            return false;
        }
        if (out.write(block) != block.size()) {
            *pError = QStringLiteral("could not write (drive full or gone?)");
            return false;
        }
    }
    return out.flush();
}

bool StemSplitter::moveFinished(const QString& partial,
        QString* pTarget,
        const Job& job,
        QString* pError) {
    // Beside the song if chosen; if that place cannot be written (a
    // read-only drive or share), the DJ's folder instead.
    QStringList places{*pTarget};
    const QString fallback =
            stems::StemCache::fileFor(job.track, stems::StemCache::Location::OneFolder);
    if (!fallback.isEmpty() && fallback != *pTarget) {
        places.append(fallback);
    }
    for (const QString& place : places) {
        if (!QDir().mkpath(QFileInfo(place).absolutePath())) {
            *pError = QStringLiteral("cannot make the folder ") +
                    QDir::toNativeSeparators(QFileInfo(place).absolutePath());
            continue;
        }
        QFile::remove(place);
        if (QFile::rename(partial, place)) { // same drive: instant
            *pTarget = place;
            return true;
        }
        const QString copying = place + QStringLiteral(".part");
        QFile::remove(copying);
        if (copyFile(partial, copying, pError) && QFile::rename(copying, place)) {
            QFile::remove(partial);
            *pTarget = place;
            return true;
        }
        QFile::remove(copying);
        if (m_stop || m_cancel) {
            break;
        }
        qInfo().noquote() << "Stems: could not save to"
                          << QDir::toNativeSeparators(QFileInfo(place).absolutePath()) << "-"
                          << *pError;
    }
    return false;
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
            m_batch.clear();
            m_batchTotal = 0;
            m_batchDone = 0;
            return false;
        }
        qInfo().noquote() << "Stems: ready on the" << m_pEngine->device() << "- start-up"
                          << QString::number(timer.elapsed() / 1000.0, 'f', 1) << "s";
    }
    QElapsedTimer timer;
    timer.start();
    const QString title = job.track->getInfo();
    double expectedSeconds = 0.0;
    {
        // A first guess until this computer's own speed is known.
        const double speed = m_speed > 0.0
                ? m_speed
                : (m_pEngine->device().contains(QStringLiteral("graphics")) ? 15.0 : 3.0);
        const double songSeconds = job.track->getDuration();
        expectedSeconds = songSeconds / speed;
        if (songSeconds > 0.0) {
            qInfo().noquote() << "Stems: splitting" << title << "- ready in about"
                              << QString::number(songSeconds / speed, 'f', 0) << "s";
        }
    }

    // 1. The song's sound, exactly as the deck plays it.
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());
    mixxx::AudioSourcePointer pSource = SoundSourceProxy(job.track).openAudioSource(params);
    const int channels = pSource ? static_cast<int>(pSource->getSignalInfo().getChannelCount()) : 0;
    if (!pSource || channels < 1 || channels > 2) {
        m_lastError = pSource ? tr("%1 channels (only mono and stereo songs can be split)").arg(channels)
                              : tr("the file could not be read");
        qWarning().noquote() << "Stems: could not read" << job.track->getLocation() << "-"
                             << m_lastError;
        return false;
    }
    const int rate = static_cast<int>(pSource->getSignalInfo().getSampleRate());
    if (rate != 44100 && rate != 48000) {
        m_lastError = tr("%1 Hz (only 44.1 and 48 kHz songs can be split)").arg(rate);
        qInfo().noquote() << "Stems: not split:" << title << "-" << m_lastError;
        return false;
    }
    std::vector<float> interleaved;
    interleaved.reserve(static_cast<std::size_t>(pSource->frameIndexRange().length()) * 2);
    mixxx::SampleBuffer buffer(kReadChunkFrames * channels);
    mixxx::IndexRange remaining = pSource->frameIndexRange();
    while (!remaining.empty()) {
        if (m_stop || m_cancel) {
            return false;
        }
        const auto range = remaining.splitAndShrinkFront(
                std::min<SINT>(kReadChunkFrames, remaining.length()));
        const auto read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                range, mixxx::SampleBuffer::WritableSlice(buffer)));
        const CSAMPLE* pData = read.readableData();
        if (channels == 2) {
            interleaved.insert(interleaved.end(), pData, pData + read.readableLength());
        } else {
            // A mono song: the same sound on both sides.
            for (SINT i = 0; i < read.readableLength(); ++i) {
                interleaved.push_back(pData[i]);
                interleaved.push_back(pData[i]);
            }
        }
        remaining = intersect(remaining, pSource->frameIndexRange());
    }
    pSource.reset();
    const std::int64_t songFrames = static_cast<std::int64_t>(interleaved.size() / 2);
    if (songFrames < stems::kModelSampleRate) {
        qInfo().noquote() << "Stems: too short to split:" << title;
        m_lastError = tr("too short");
        return false;
    }
    const double seconds = static_cast<double>(songFrames) / rate;

    // 2. At the model's sample rate. The stem file keeps the song's own
    // rate (44100 or 48000 Hz), so its frames match the song's exactly
    // (cue points, beat grid and loops stay where they are).
    const std::vector<float> original = stems::toPlanar(interleaved.data(), songFrames);
    std::vector<float>().swap(interleaved);
    const int outRate = (rate == 44100 || rate == 48000) ? rate : stems::kModelSampleRate;
    std::vector<float> mix = original;
    std::int64_t frames = songFrames;
    if (rate != stems::kModelSampleRate) {
        std::vector<float> left = stems::resample(
                original.data(), songFrames, rate, stems::kModelSampleRate);
        std::vector<float> right = stems::resample(
                original.data() + songFrames, songFrames, rate, stems::kModelSampleRate);
        frames = static_cast<std::int64_t>(left.size());
        mix.assign(left.begin(), left.end());
        mix.insert(mix.end(), right.begin(), right.end());
    }
    // The mix track: the song itself at the output rate.
    const std::vector<float>& mixOut = outRate == rate ? original : mix;
    const std::int64_t mixOutFrames = outRate == rate ? songFrames : frames;
    float mean = 0.0f;
    float std = 1.0f;
    stems::meanAndStd(mix, &mean, &std);

    // 3. Split piece by piece, writing the finished part as we go.
    // Beside the song if chosen; if that place cannot be written (a
    // read-only drive or share), the DJ's folder instead.
    // The file is made on this computer first (a network share, cloud
    // folder or slow USB drive can make writing piece by piece take for
    // ever) and moved to its place when finished.
    QString target = job.target;
    QString error;
    const QString workFolder = QDir(QStandardPaths::writableLocation(
                                            QStandardPaths::TempLocation))
                                       .filePath(QStringLiteral("Mixxx Stems work"));
    QDir().mkpath(workFolder);
    const QString partial = QDir(workFolder).filePath(
            QFileInfo(target).fileName() + QStringLiteral(".part"));
    QFile::remove(partial);
    auto pWriter = stems::StemFileWriter::create();
    if (!pWriter || !pWriter->open(partial, outRate, &error)) {
        m_lastError = tr("could not create the stem file");
        qWarning().noquote() << "Stems: could not create the stem file:"
                             << (error.isEmpty() ? QStringLiteral("no encoder") : error);
        pWriter.reset();
        QFile::remove(partial);
        return false;
    }
    // Give up on a song that takes far longer than expected, so one bad
    // song never blocks the others.
    const qint64 limitMs = static_cast<qint64>(
            1000.0 * std::max(180.0, 6.0 * std::max(expectedSeconds, 0.0)));
    constexpr int kParts = stems::kStemCount * 2;
    std::vector<stems::Resampler> back;
    if (outRate != stems::kModelSampleRate) {
        for (int part = 0; part < kParts; ++part) {
            back.emplace_back(stems::kModelSampleRate, outRate);
        }
    }
    std::int64_t written = 0; // output frames written so far
    std::vector<float> chunk;
    std::vector<std::vector<float>> converted(kParts);
    // Writes `count` output frames: the mix, then the parts ([8][count]).
    auto writeOut = [&](const std::vector<std::vector<float>>& parts, std::int64_t count) {
        if (count <= 0) {
            return true;
        }
        chunk.assign(static_cast<std::size_t>(stems::StemFileWriter::kTracks) * 2 * count, 0.0f);
        for (int channel = 0; channel < 2; ++channel) {
            for (std::int64_t i = 0; i < count; ++i) {
                const std::int64_t at = written + i;
                chunk[channel * count + i] = at < mixOutFrames ? mixOut[channel * mixOutFrames + at] : 0.0f;
            }
        }
        for (int part = 0; part < kParts; ++part) {
            std::copy(parts[part].begin(), parts[part].begin() + count,
                    chunk.begin() + (2 + part) * count);
        }
        written += count;
        return pWriter->write(chunk.data(), count, &error);
    };
    // Hands finished model-rate parts ([8][count]) on, converting the rate
    // if needed.
    auto emitParts = [&](const std::vector<float>& parts, std::int64_t count, bool end) {
        if (back.empty()) {
            for (int part = 0; part < kParts; ++part) {
                converted[part].assign(parts.begin() + part * count, parts.begin() + (part + 1) * count);
            }
            return writeOut(converted, count);
        }
        for (int part = 0; part < kParts; ++part) {
            converted[part].clear();
            back[part].push(parts.data() + part * count, count, &converted[part]);
            if (end) {
                back[part].finish(&converted[part]);
            }
        }
        std::int64_t ready = static_cast<std::int64_t>(converted[0].size());
        for (int part = 1; part < kParts; ++part) {
            ready = std::min<std::int64_t>(ready, converted[part].size());
        }
        return writeOut(converted, ready);
    };

    const auto pieces = stems::planPieces(frames);
    stems::Blender blender(frames);
    std::vector<float> input;
    std::vector<float> output(static_cast<std::size_t>(stems::kStemCount) * 2 * stems::kModelSegment);
    std::vector<float> parts;
    bool ok = true;
    for (std::size_t k = 0; k < pieces.size() && ok; ++k) {
        if (m_stop) {
            ok = false;
            error = QStringLiteral("Mixxx is closing");
            break;
        }
        if (m_cancel) {
            ok = false;
            error = QStringLiteral("stopped");
            break;
        }
        if (timer.elapsed() > limitMs) {
            ok = false;
            error = QStringLiteral("took too long - skipped");
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
        const bool last = k + 1 == pieces.size();
        const std::int64_t upTo = last ? frames : pieces[k + 1].offset;
        const std::int64_t count = blender.flush(upTo, mean, std, &parts);
        ok = emitParts(parts, count, last);
    }
    ok = ok && pWriter->finish(&error);
    pWriter.reset();
    if (ok) {
        ok = moveFinished(partial, &target, job, &error);
    }
    if (!ok) {
        QFile::remove(partial);
        m_lastError = error;
        qWarning().noquote() << "Stems: splitting" << title << "failed:" << error;
        return false;
    }
    const double took = timer.elapsed() / 1000.0;
    const double speedNow = seconds / std::max(took, 0.001);
    m_speed = m_speed > 0.0 ? 0.7 * m_speed + 0.3 * speedNow : speedNow;
    qInfo().noquote() << "Stems: split" << title << "(" << minutes(seconds) << ") in"
                      << QString::number(took, 'f', 1) << "s on the" << m_pEngine->device()
                      << "=" << QString::number(seconds / std::max(took, 0.001), 'f', 0)
                      << "x faster than playing (" << outRate << "Hz)"
                      << "->" << QDir::toNativeSeparators(target);
    emit stemsReady(job.track->getId(), target);
    return true;
}
