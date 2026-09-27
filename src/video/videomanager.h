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

class QScreen;
class VideoDecoder;
class VideoManager;
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

/// Auto DJ 2.0 video mixing: shows the music videos of the decks on a
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
};
