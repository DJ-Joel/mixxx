#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "stems/stemcache.h"

// Auto DJ 2.0 plus Video Mixing: stem files are found by a code that does
// not depend on the song's tags, size or date, and old ones can be cleaned up.

using stems::StemCache;

namespace {

void writeFile(const QString& path, const QByteArray& data = QByteArray("x")) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(data);
}

QString stemName(const QString& readable, const QString& code) {
    return QStringLiteral("%1 [%2].stem.mp4").arg(readable, code);
}

} // namespace

TEST(StemCacheTest, CodeIgnoresEverythingButNameLengthAndRate) {
    const QString code = StemCache::codeFor(QStringLiteral("song.mp3"), 241.2, 44100);
    EXPECT_EQ(10, code.size());
    // The same song: the same code (the length is counted in whole seconds).
    EXPECT_EQ(code, StemCache::codeFor(QStringLiteral("song.mp3"), 241.4, 44100));
    // Another file, length or rate: another code.
    EXPECT_NE(code, StemCache::codeFor(QStringLiteral("other.mp3"), 241.2, 44100));
    EXPECT_NE(code, StemCache::codeFor(QStringLiteral("song.mp3"), 250.0, 44100));
    EXPECT_NE(code, StemCache::codeFor(QStringLiteral("song.mp3"), 241.2, 48000));
    // The old way (size and date) gives a different code.
    EXPECT_NE(code, StemCache::oldCodeFor(QStringLiteral("song.mp3"), 1000, 12345));
}

TEST(StemCacheTest, CodeIsReadBackFromTheFileName) {
    EXPECT_EQ(QStringLiteral("0123456789"),
            StemCache::codeOfFile(QStringLiteral("Artist - Title [0123456789].stem.mp4")));
    // Tags edited since: the readable part differs, the code is the same.
    EXPECT_EQ(QStringLiteral("abcdef0123"),
            StemCache::codeOfFile(QStringLiteral("New Artist - [x] New [abcdef0123].stem.mp4")));
    EXPECT_TRUE(StemCache::codeOfFile(QStringLiteral("Artist - Title.stem.mp4")).isEmpty());
    EXPECT_TRUE(StemCache::codeOfFile(QStringLiteral("a [0123456789].stem.mp4.part")).isEmpty());
    EXPECT_TRUE(StemCache::codeOfFile(QStringLiteral("a [01234].stem.mp4")).isEmpty());
}

TEST(StemCacheTest, CleanUpFindsOnlyUnusedFiles) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString musicDir = temp.filePath(QStringLiteral("music"));
    const QString chosen = temp.filePath(QStringLiteral("settings"));
    StemCache::setFolder(chosen);
    const QString stemDir = QDir(chosen).filePath(StemCache::folderName());

    const QString kept = QDir(musicDir).filePath(QStringLiteral("kept.mp3"));
    const QString removed = QDir(musicDir).filePath(QStringLiteral("removed.mp3"));
    writeFile(kept);
    writeFile(removed);
    const QString keptStem = QDir(stemDir).filePath(stemName(QStringLiteral("A - Kept"),
            StemCache::codeFor(QStringLiteral("kept.mp3"), 200.0, 44100)));
    // Found one second either side too (the length may be corrected a little).
    const QString keptNearStem = QDir(stemDir).filePath(stemName(QStringLiteral("A - Kept"),
            StemCache::codeFor(QStringLiteral("kept.mp3"), 201.0, 44100)));
    const QString removedStem = QDir(stemDir).filePath(stemName(QStringLiteral("B - Gone"),
            StemCache::codeFor(QStringLiteral("removed.mp3"), 180.0, 44100)));
    const QFileInfo keptInfo(kept);
    const QString keptOldStem = QDir(stemDir).filePath(stemName(QStringLiteral("A - Kept"),
            StemCache::oldCodeFor(QStringLiteral("kept.mp3"),
                    keptInfo.size(),
                    keptInfo.lastModified().toSecsSinceEpoch())));
    const QString staleOldStem = QDir(stemDir).filePath(stemName(QStringLiteral("A - Kept"),
            StemCache::oldCodeFor(QStringLiteral("kept.mp3"), 999999, 1)));
    const QString nextToSongStem = QDir(QDir(musicDir).filePath(StemCache::folderName()))
                                           .filePath(stemName(QStringLiteral("B - Gone"),
                                                   QStringLiteral("0000000000")));
    const QString partial = QDir(stemDir).filePath(QStringLiteral("x [1111111111].stem.mp4.part"));
    for (const QString& file : {keptStem, keptNearStem, removedStem, keptOldStem,
                 staleOldStem, nextToSongStem, partial}) {
        writeFile(file, QByteArray(100, 'x'));
    }

    // Only "kept.mp3" is still in the library.
    const QList<StemCache::SongFile> songs{{kept, 200.4, 44100}};
    qint64 bytes = 0;
    int missing = 0;
    QStringList unused = StemCache::unusedFiles(songs, &bytes, &missing);
    unused.sort();
    QStringList expected{removedStem, staleOldStem, nextToSongStem};
    for (QString& file : expected) {
        file = QFileInfo(file).absoluteFilePath();
    }
    expected.sort();
    EXPECT_EQ(expected, unused);
    EXPECT_EQ(300, bytes);
    EXPECT_EQ(0, missing);
}

TEST(StemCacheTest, CleanUpRemovesNothingWhenSongsCannotBeReached) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString chosen = temp.filePath(QStringLiteral("settings"));
    StemCache::setFolder(chosen);
    const QString stemDir = QDir(chosen).filePath(StemCache::folderName());
    writeFile(QDir(stemDir).filePath(stemName(QStringLiteral("A"), QStringLiteral("2222222222"))));
    // A song on a drive that is not connected: its old-style stem file
    // cannot be checked, so nothing at all is offered for removal.
    const QList<StemCache::SongFile> songs{
            {temp.filePath(QStringLiteral("offline/song.mp3")), 200.0, 44100}};
    qint64 bytes = 0;
    int missing = 0;
    EXPECT_TRUE(StemCache::unusedFiles(songs, &bytes, &missing).isEmpty());
    EXPECT_EQ(1, missing);
    EXPECT_EQ(0, bytes);
}
