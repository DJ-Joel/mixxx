#include "stems/stemcache.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <QRegularExpression>
#include <algorithm>
#include <atomic>
#include <cmath>

#include "track/track.h"

namespace stems {
namespace {

QMutex s_mutex;
QString s_folder;
QString s_fallbackFolder;
std::atomic<int> s_location{0};
std::atomic<bool> s_playback{true};

// The stem files of each "Mixxx Stems" folder by their code, read once and
// read again only when the folder changes (a file added, removed or renamed
// changes the folder's date). Songs are looked up many times (every load,
// every queued song), so the folder is not listed each time.
struct FolderIndex {
    QDateTime folderDate;
    QHash<QString, QString> fileByCode; // code -> file name
};
QMutex s_indexMutex;
QHash<QString, FolderIndex> s_indexes; // folder path -> its index

QString sha1Code(const QString& text) {
    return QString::fromLatin1(
            QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
}

// The readable part of a stem file name: "Artist - Title".
QString readableName(const TrackPointer& pTrack, const QFileInfo& songInfo) {
    QString name = pTrack->getArtist().trimmed();
    const QString title = pTrack->getTitle().trimmed();
    if (!name.isEmpty() && !title.isEmpty()) {
        name += QStringLiteral(" - ") + title;
    } else if (!title.isEmpty()) {
        name = title;
    } else if (name.isEmpty()) {
        name = songInfo.completeBaseName();
    }
    static const QRegularExpression kUnsafe(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    name.replace(kUnsafe, QStringLiteral("_"));
    return name.left(80).trimmed();
}

// The folder that holds the stem files for a song at a location (empty =
// no folder chosen).
QString stemFolderFor(const QFileInfo& songInfo, StemCache::Location where) {
    QString base;
    if (where == StemCache::Location::NextToSong) {
        base = songInfo.absolutePath();
    } else {
        base = StemCache::folder();
        if (base.isEmpty()) {
            return QString();
        }
    }
    return QDir(base).filePath(StemCache::folderName());
}

// The stem file with this code in a folder, or empty.
QString findByCode(const QString& folder, const QString& code) {
    const QFileInfo folderInfo(folder);
    if (!folderInfo.isDir()) {
        return QString();
    }
    const QDateTime date = folderInfo.lastModified();
    const QMutexLocker lock(&s_indexMutex);
    FolderIndex& index = s_indexes[folder];
    if (!index.folderDate.isValid() || index.folderDate != date) {
        index.folderDate = date;
        index.fileByCode.clear();
        const QStringList files = QDir(folder).entryList(
                {QStringLiteral("*.stem.mp4")}, QDir::Files);
        for (const QString& file : files) {
            const QString fileCode = StemCache::codeOfFile(file);
            if (!fileCode.isEmpty()) {
                index.fileByCode.insert(fileCode, file);
            }
        }
    }
    const QString file = index.fileByCode.value(code);
    if (file.isEmpty()) {
        return QString();
    }
    const QString path = QDir(folder).filePath(file);
    return QFileInfo::exists(path) ? path : QString();
}

} // namespace

void StemCache::setFolder(const QString& folder) {
    const QMutexLocker lock(&s_mutex);
    s_folder = folder;
}

QString StemCache::folder() {
    const QMutexLocker lock(&s_mutex);
    return s_folder;
}

void StemCache::setFallbackFolder(const QString& folder) {
    const QMutexLocker lock(&s_mutex);
    s_fallbackFolder = folder;
}

QString StemCache::fallbackFolder() {
    const QMutexLocker lock(&s_mutex);
    return s_fallbackFolder;
}

QString StemCache::fileInFolder(const TrackPointer& pTrack, const QString& baseFolder) {
    if (!pTrack || baseFolder.isEmpty()) {
        return QString();
    }
    const QFileInfo info(pTrack->getLocation());
    const QString code = codeFor(info.fileName(),
            pTrack->getDuration(),
            static_cast<int>(pTrack->getSampleRate().value()));
    return QDir(QDir(baseFolder).filePath(folderName()))
            .filePath(QStringLiteral("%1 [%2].stem.mp4").arg(readableName(pTrack, info), code));
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

QString StemCache::codeFor(const QString& songFileName, double durationSec, int sampleRate) {
    return sha1Code(songFileName + QStringLiteral("|len") +
            QString::number(std::lround(std::max(0.0, durationSec))) + QStringLiteral("|rate") +
            QString::number(sampleRate) + QStringLiteral("|format") +
            QString::number(kFormatVersion) + QStringLiteral("|tagfree"));
}

QString StemCache::oldCodeFor(const QString& songFileName, qint64 size, qint64 modifiedSecs) {
    return sha1Code(songFileName + QString::number(size) + QString::number(modifiedSecs) +
            QStringLiteral("|format") + QString::number(kFormatVersion));
}

QString StemCache::codeOfFile(const QString& stemFileName) {
    static const QRegularExpression kCode(
            QStringLiteral("\\[([0-9a-f]{10})\\]\\.stem\\.mp4$"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = kCode.match(stemFileName);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

QString StemCache::fileFor(const TrackPointer& pTrack, Location where) {
    if (!pTrack) {
        return QString();
    }
    const QFileInfo info(pTrack->getLocation());
    const QString stemFolder = stemFolderFor(info, where);
    if (stemFolder.isEmpty()) {
        return QString();
    }
    const QString code = codeFor(info.fileName(),
            pTrack->getDuration(),
            static_cast<int>(pTrack->getSampleRate().value()));
    return QDir(stemFolder).filePath(
            QStringLiteral("%1 [%2].stem.mp4").arg(readableName(pTrack, info), code));
}

QString StemCache::readyFileFor(const TrackPointer& pTrack) {
    if (!pTrack) {
        return QString();
    }
    const QFileInfo info(pTrack->getLocation());
    const double duration = pTrack->getDuration();
    const int sampleRate = static_cast<int>(pTrack->getSampleRate().value());
    // The chosen location first, then the other one (so changing the
    // setting does not lose stem files already made).
    const Location first = location();
    const Location second =
            first == Location::OneFolder ? Location::NextToSong : Location::OneFolder;
    for (const Location where : {first, second}) {
        const QString stemFolder = stemFolderFor(info, where);
        if (stemFolder.isEmpty()) {
            continue;
        }
        // By its code, whatever the tags say now. Also a second either side
        // (Mixxx may correct a song's length a little once it is read); the
        // deck checks the length exactly before it plays the parts.
        for (const double shift : {0.0, -1.0, 1.0}) {
            const QString file = findByCode(stemFolder,
                    codeFor(info.fileName(), duration + shift, sampleRate));
            if (!file.isEmpty()) {
                return file;
            }
        }
        // A stem file named the old way (code from the song file's size and
        // date, name from its tags): still used until those change.
        const QString oldFile = QDir(stemFolder).filePath(
                QStringLiteral("%1 [%2].stem.mp4")
                        .arg(readableName(pTrack, info),
                                oldCodeFor(info.fileName(),
                                        info.size(),
                                        info.lastModified().toSecsSinceEpoch())));
        if (QFileInfo::exists(oldFile)) {
            return oldFile;
        }
    }
    // Saved in the settings folder because the chosen one could not be
    // written at the time.
    const QString fallback = fallbackFolder();
    if (!fallback.isEmpty() && QDir::cleanPath(fallback) != QDir::cleanPath(folder())) {
        const QString stemFolder = QDir(fallback).filePath(folderName());
        for (const double shift : {0.0, -1.0, 1.0}) {
            const QString file = findByCode(stemFolder,
                    codeFor(info.fileName(), duration + shift, sampleRate));
            if (!file.isEmpty()) {
                return file;
            }
        }
    }
    return QString();
}

QStringList StemCache::unusedFiles(
        const QList<SongFile>& songs, qint64* pTotalBytes, int* pMissingSongs) {
    *pTotalBytes = 0;
    *pMissingSongs = 0;
    QSet<QString> used;
    QSet<QString> folders;
    for (const QString& base : {folder(), fallbackFolder()}) {
        if (!base.isEmpty()) {
            folders.insert(QDir::cleanPath(QDir(base).filePath(folderName())));
        }
    }
    for (const SongFile& song : songs) {
        const QFileInfo info(song.location);
        for (const double shift : {0.0, -1.0, 1.0}) {
            used.insert(codeFor(info.fileName(), song.durationSec + shift, song.sampleRate));
        }
        if (info.exists()) {
            // A stem file named the old way is still used while the song
            // file has the same size and date.
            used.insert(oldCodeFor(
                    info.fileName(), info.size(), info.lastModified().toSecsSinceEpoch()));
            folders.insert(QDir::cleanPath(QDir(info.absolutePath()).filePath(folderName())));
        } else {
            ++*pMissingSongs;
        }
    }
    QStringList unused;
    if (*pMissingSongs > 0) {
        return unused; // cannot be sure: see the header
    }
    for (const QString& stemFolder : std::as_const(folders)) {
        const QDir dir(stemFolder);
        if (!dir.exists()) {
            continue;
        }
        const QFileInfoList files =
                dir.entryInfoList({QStringLiteral("*.stem.mp4")}, QDir::Files);
        for (const QFileInfo& file : files) {
            const QString code = codeOfFile(file.fileName());
            if (!code.isEmpty() && used.contains(code)) {
                continue;
            }
            unused.append(file.absoluteFilePath());
            *pTotalBytes += file.size();
        }
    }
    return unused;
}

void StemCache::noteNewFile(const QString& stemFile) {
    const QFileInfo info(stemFile);
    const QString code = codeOfFile(info.fileName());
    if (code.isEmpty()) {
        return;
    }
    const QMutexLocker lock(&s_indexMutex);
    const auto it = s_indexes.find(info.absolutePath());
    if (it != s_indexes.end()) {
        it->fileByCode.insert(code, info.fileName());
        it->folderDate = QFileInfo(info.absolutePath()).lastModified();
    }
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
