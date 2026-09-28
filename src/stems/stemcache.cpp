#include "stems/stemcache.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <atomic>

#include "track/track.h"

namespace stems {
namespace {

QMutex s_mutex;
QString s_folder;
std::atomic<int> s_location{0};
std::atomic<bool> s_playback{true};

} // namespace

void StemCache::setFolder(const QString& folder) {
    const QMutexLocker lock(&s_mutex);
    s_folder = folder;
}

QString StemCache::folder() {
    const QMutexLocker lock(&s_mutex);
    return s_folder;
}

void StemCache::setLocation(Location location) {
    s_location = static_cast<int>(location);
}

StemCache::Location StemCache::location() {
    return static_cast<Location>(s_location.load());
}

void StemCache::setPlaybackEnabled(bool enabled) {
    s_playback = enabled;
}

bool StemCache::playbackEnabled() {
    return s_playback;
}

QString StemCache::fileFor(const TrackPointer& pTrack) {
    return fileFor(pTrack, location());
}

QString StemCache::fileFor(const TrackPointer& pTrack, Location where) {
    if (!pTrack) {
        return QString();
    }
    const QString songLocation = pTrack->getLocation();
    const QFileInfo info(songLocation);
    QString base;
    if (where == Location::NextToSong) {
        base = info.absolutePath();
    } else {
        base = folder();
        if (base.isEmpty()) {
            return QString();
        }
    }
    const QByteArray key = (info.fileName() + QString::number(info.size()) +
            QString::number(info.lastModified().toSecsSinceEpoch()) +
            QStringLiteral("|format") + QString::number(kFormatVersion))
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
    return QDir(QDir(base).filePath(folderName()))
            .filePath(QStringLiteral("%1 [%2].stem.mp4").arg(name, code));
}

QString StemCache::readyFileFor(const TrackPointer& pTrack) {
    // The chosen location first, then the other one (so changing the
    // setting does not lose stem files already made).
    const Location first = location();
    const Location second =
            first == Location::OneFolder ? Location::NextToSong : Location::OneFolder;
    for (const Location where : {first, second}) {
        const QString file = fileFor(pTrack, where);
        if (!file.isEmpty() && QFileInfo::exists(file)) {
            return file;
        }
    }
    return QString();
}

QList<StemInfo> StemCache::stemInfos() {
    // The model's order; the colours match the stem manifest.
    return {
            StemInfo(QStringLiteral("Drums"), QColor(0xFD, 0x4A, 0x4A)),
            StemInfo(QStringLiteral("Bass"), QColor(0xFF, 0xFF, 0x00)),
            StemInfo(QStringLiteral("Other"), QColor(0x00, 0xE8, 0xE8)),
            StemInfo(QStringLiteral("Vocals"), QColor(0xFF, 0x00, 0xFF)),
    };
}

} // namespace stems
