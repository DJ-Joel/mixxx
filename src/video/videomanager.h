#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <vector>

#include "control/pollingcontrolproxy.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class QScreen;
class VideoDecoder;
class VideoManager;
class VideoRecorder;
class VisualPlayPosition;

/// A window that shows the mixed video, scaled to fit with black bars.
class VideoScreen : public QWidget {
  public:
    VideoScreen(VideoManager* pManager, QWidget* pParent, Qt::WindowFlags flags);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void keyPressEvent(QKeyEvent* pEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* pEvent) override;

  private:
    VideoManager* const m_pManager;
};

/// Auto DJ 2.0 plus Video Mixing: shows the music videos of the decks on a
/// screen or projector, following the decks.
///
/// Each deck's picture follows the deck's play position, so tempo changes,
/// beatmatching, loops and jumps carry the video along. The pictures are
/// mixed like the sound: by the deck volume faders and the crossfader, so
/// every mix (by hand or by Auto DJ) is also a video crossfade. A track with
/// no video shows its cover art and title. The audio engine is not touched:
/// if the video falls behind, it only shows older pictures.
class VideoManager : public QObject {
    Q_OBJECT
  public:
    explicit VideoManager(QWidget* pParentWindow);
    ~VideoManager() override;

    /// Full screen on this screen (projector, TV).
    void showOnScreen(QScreen* pScreen);
    /// In a normal window (for testing on one screen).
    void showInWindow();
    void setPreviewVisible(bool visible);
    bool isPreviewVisible() const;
    /// A video window (full screen, window or preview) is showing.
    bool isShowing() const {
        return anyWindowVisible();
    }
    /// Hides every video window and closes the video files.
    void stop();

    /// Picture timing: show the picture this many milliseconds later
    /// (negative = earlier) than the sound. Projectors and TVs often show
    /// the picture a little late; a negative value makes up for that.
    void setPictureDelayMs(int delayMs);
    int pictureDelayMs() const {
        return m_pictureDelayMs;
    }
    static constexpr int kMaxPictureDelayMs = 500;

    /// How the picture changes from one song to the next.
    enum class Transition {
        Crossfade, ///< blend like the sound (volume faders, crossfader)
        Cut,       ///< show one deck at a time; cut on a beat of the new song
    };
    void setTransition(Transition transition);
    Transition transition() const {
        return m_transition;
    }
    /// "Artist - Title" at the bottom for a few seconds when a song takes
    /// over the screen.
    void setShowTitles(bool show);
    bool showTitles() const {
        return m_showTitles;
    }
    /// Songs without video: the cover art pulses with the beat.
    void setMovingPictures(bool moving);
    bool movingPictures() const {
        return m_movingPictures;
    }
    /// The DJ's name and/or logo (an image file) in the top right corner.
    /// Empty = none.
    void setBrand(const QString& text, const QString& logoPath);
    /// Decode on the graphics card when possible.
    void setUseGraphicsCard(bool use);
    bool useGraphicsCard() const {
        return m_useGraphicsCard;
    }

    /// The mixed picture (1920x1080).
    const QImage& canvas() const {
        return m_canvas;
    }
    /// Called by a video window when the DJ closes it.
    void windowClosed();

    /// Video recording: saves the mixed picture (titles, logo, cuts) and
    /// the mixed sound together as one MP4 file. Works with or without a
    /// video window open. False (with the reason) if it cannot start.
    bool startRecording(const QString& path, QString* pError);
    /// Ends the recording; the file is completed a moment later
    /// (recordingFinished).
    void stopRecording();
    bool isRecording() const;
    /// How long the current recording is (seconds).
    double recordingSeconds() const;
    /// False where video recording is not available (not Windows).
    static bool canRecord();

  signals:
    /// A recording's file is complete. `error` is empty when it was saved.
    void recordingFinished(const QString& path, const QString& error);
  public:

    static constexpr int kCanvasWidth = 1920;
    static constexpr int kCanvasHeight = 1080;
    static constexpr int kMaxDecks = 4;

  private:
    struct Deck;
    void tick();
    void ensureRunning();
    bool anyWindowVisible() const;
    void updateDeckTrack(Deck& deck);
    QImage stillFor(const TrackPointer& pTrack) const;
    void logStats();

    QWidget* const m_pParentWindow;
    QTimer m_timer;
    QImage m_canvas;
    std::vector<std::unique_ptr<Deck>> m_decks;
    PollingControlProxy m_numDecks;
    PollingControlProxy m_crossfader;
    PollingControlProxy m_xfaderCurve;
    PollingControlProxy m_xfaderCalibration;
    PollingControlProxy m_xfaderMode;
    PollingControlProxy m_xfaderReverse;
    QPointer<VideoScreen> m_pOutput;
    QPointer<VideoScreen> m_pPreview;
    QString m_lastMixKey;
    QElapsedTimer m_statsTimer;
    int m_composed = 0;
    double m_composeMs = 0.0;
    int m_pictureDelayMs = 0;
    bool m_useGraphicsCard = true;

    // Video recording.
    void recordPicture(bool newPicture);
    void checkRecordingFinished();
    PollingControlProxy m_sampleRate;
    std::unique_ptr<VideoRecorder> m_pRecorder;
    bool m_recordingStopping = false;
    QTimer m_recordingCheck;

    // Video phase 3.
    void drawOverlays(QPainter* pPainter, double nowSec);
    Transition m_transition = Transition::Crossfade;
    bool m_showTitles = true;
    bool m_movingPictures = true;
    QString m_brandText;
    QImage m_brandLogo;
    QElapsedTimer m_clock;   ///< time for beats, cuts and titles
    int m_shownDeck = -1;    ///< Cut: the deck on screen
    int m_cutTarget = -1;    ///< Cut: the deck waiting for its beat
    double m_cutSince = 0.0; ///< Cut: when it started waiting
    int m_titleDeck = -1;    ///< the deck whose title is shown
    TrackId m_titledTrack;   ///< the song whose title was shown last
    QString m_titleText;
    double m_titleStart = -100.0;
};
