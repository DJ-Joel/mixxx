#pragma once

#include <QString>

#include "track/track_decl.h"
#include "track/trackid.h"

/// A small, thread-safe snapshot of the track data that Auto DJ 2.0 needs
/// for mixing decisions. It is copied out of `Track` on the GUI thread so
/// that the scorer and sequencer can run on a worker thread without ever
/// touching `Track` objects or the database.
struct TrackFeatures {
    TrackId id;
    double bpm = 0.0;          ///< 0 = unknown
    int camelotNumber = 0;     ///< 1..12, 0 = unknown
    bool camelotMinor = false; ///< true = "A" (minor), false = "B" (major)
    double energy = 0.0;       ///< 1..10 from AnalyzerEnergy, 0 = unknown
    double loudnessLufs = 0.0; ///< integrated loudness, 0 = unknown
    double durationSec = 0.0;
    bool isStem = false;

    bool hasBpm() const {
        return bpm > 0.0;
    }
    bool hasKey() const {
        return camelotNumber >= 1 && camelotNumber <= 12;
    }
    bool hasEnergy() const {
        return energy > 0.0;
    }

    /// Camelot notation, e.g. "8A". Returns "?" when the key is unknown.
    QString camelotText() const {
        if (!hasKey()) {
            return QStringLiteral("?");
        }
        return QString::number(camelotNumber) +
                (camelotMinor ? QChar('A') : QChar('B'));
    }

    /// Reads BPM, key, ReplayGain and stem info from a loaded Track.
    /// Must be called on a thread that may access the Track.
    static TrackFeatures fromTrack(const TrackPointer& pTrack);

    /// Converts an OpenKey number (1..12) to a Camelot number (1..12).
    /// Returns 0 for an invalid input. OpenKey 1 (C major / A minor) = Camelot 8.
    static int camelotFromOpenKey(int openKeyNumber);
};
