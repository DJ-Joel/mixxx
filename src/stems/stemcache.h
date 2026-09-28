#pragma once

#include <QList>
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
    static void setLocation(Location location);
    static Location location();
    /// Whether decks play songs from their stem files.
    static void setPlaybackEnabled(bool enabled);
    static bool playbackEnabled();

    /// The stem file for this song at the chosen location (it may not
    /// exist yet). The name tells the song; the code in it depends on the
    /// song file's name, size and date (not its drive or folder, so a USB
    /// drive may get another letter) and on the format, so an outdated stem
    /// file is never used.
    static QString fileFor(const TrackPointer& pTrack);
    static QString fileFor(const TrackPointer& pTrack, Location location);
    /// The finished stem file (at either location), or empty.
    static QString readyFileFor(const TrackPointer& pTrack);
    /// Names and colours of the four parts.
    static QList<StemInfo> stemInfos();

    /// 2: the parts have the song's own sample rate (44100 or 48000 Hz).
    /// 3: the code no longer depends on the song's drive and folder.
    static constexpr int kFormatVersion = 3;
};

} // namespace stems
