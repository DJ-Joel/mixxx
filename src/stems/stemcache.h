#pragma once

#include <QList>
#include <QStringList>
#include <QString>

#include "track/steminfo.h"
#include "track/track_decl.h"

/// Auto DJ 2.0 plus Video Mixing: where a song's stem file lives. Shared
/// by the splitter (which makes the files) and the decks (which play them).
/// Thread-safe.
namespace stems {

class StemCache {
  public:
    /// Where stem files are saved.
    enum class Location {
        OneFolder, ///< in "Mixxx Stems" inside the folder the DJ chose
        NextToSong ///< in "Mixxx Stems" beside each song (travels with a USB library)
    };
    /// The name of every folder holding stem files. The library scanner
    /// skips folders with this name, so the parts never show up as songs.
    static QString folderName() {
        return QStringLiteral("Mixxx Stems");
    }
    /// The folder the DJ chose (its "Mixxx Stems" subfolder holds the
    /// files). Empty = stems are off.
    static void setFolder(const QString& folder);
    static QString folder();
    /// Where stem files go when neither the chosen folder nor the song's
    /// folder can be written (a network share that is off): the settings
    /// folder. Stem files there are found too.
    static void setFallbackFolder(const QString& folder);
    static QString fallbackFolder();
    /// The stem file for this song in the "Mixxx Stems" folder inside
    /// `baseFolder` (it may not exist yet).
    static QString fileInFolder(const TrackPointer& pTrack, const QString& baseFolder);
    static void setLocation(Location location);
    static Location location();
    /// Whether decks play songs from their stem files.
    static void setPlaybackEnabled(bool enabled);
    static bool playbackEnabled();

    /// The stem file for this song at the chosen location (it may not
    /// exist yet): "Artist - Title [code].stem.mp4". The name is only there
    /// to be read; the song is found by the code, which depends on the song
    /// file's name, its length (whole seconds) and sample rate and on the
    /// format. Not on the tags, the file's size or date (editing or writing
    /// tags keeps the parts), nor on its drive or folder (a USB drive may
    /// get another letter).
    static QString fileFor(const TrackPointer& pTrack);
    static QString fileFor(const TrackPointer& pTrack, Location location);
    /// The finished stem file (at either location), or empty. Also finds
    /// stem files named the old way (before the code ignored the tags).
    static QString readyFileFor(const TrackPointer& pTrack);
    /// The splitter saved a new stem file: make it known at once.
    static void noteNewFile(const QString& stemFile);

    /// The code in a stem file's name for a song (see fileFor). The length
    /// is rounded to whole seconds; lookups also try one second either side,
    /// because Mixxx may correct a song's length a little once it is read.
    static QString codeFor(const QString& songFileName, double durationSec, int sampleRate);
    /// The code of a stem file named the old way (format 3: the song file's
    /// name, size and date).
    static QString oldCodeFor(const QString& songFileName, qint64 size, qint64 modifiedSecs);
    /// The code in a stem file's name ("... [code].stem.mp4"), or empty.
    static QString codeOfFile(const QString& stemFileName);

    /// A song of the library, as far as its stem file is concerned.
    struct SongFile {
        QString location; ///< the song file
        double durationSec = 0.0;
        int sampleRate = 0;
    };
    /// Clean up: the stem files in the "Mixxx Stems" folders (the chosen
    /// one, and the one beside each song) that none of these songs uses any
    /// more: the song was removed from the library, its file changed, or
    /// the file was named the old way and its tags were written since.
    /// Reads the disk, so run it in the background. Unfinished ".part"
    /// files are left alone (a split may be writing one).
    /// Safety: when some song files cannot be reached (a network drive that
    /// is off), nothing is returned and pMissingSongs says how many: a stem
    /// file named the old way cannot be checked without its song file.
    static QStringList unusedFiles(const QList<SongFile>& songs,
            qint64* pTotalBytes,
            int* pMissingSongs);
    /// Names and colours of the four parts.
    static QList<StemInfo> stemInfos();

    /// 2: the parts have the song's own sample rate (44100 or 48000 Hz).
    /// 3: the code no longer depends on the song's drive and folder.
    /// (The file itself is still format 3; only its code changed: see
    /// codeFor.)
    static constexpr int kFormatVersion = 3;
};

} // namespace stems
