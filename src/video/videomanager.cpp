#include "video/videomanager.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <QtDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#include "engine/channels/enginechannel.h"
#include "engine/enginexfader.h"
#include "library/coverart.h"
#include "mixer/playerinfo.h"
#include "moc_videomanager.cpp"
#include "track/track.h"
#include "video/videodecoder.h"
#include "video/videomix.h"
#include "waveform/visualplayposition.h"

namespace {

constexpr int kTickMs = 16; // about 60 times a second
constexpr qint64 kStatsEveryMs = 10000;

PollingControlProxy control(const QString& group, const QString& item) {
    return PollingControlProxy(ConfigKey(group, item), ControlFlag::AllowMissingOrInvalid);
}

} // namespace

struct VideoManager::Deck {
    Deck(const QString& deckGroup, bool useGraphicsCard)
            : group(deckGroup),
              decoder(std::make_unique<VideoDecoder>(deckGroup,
                      VideoManager::kCanvasWidth,
                      VideoManager::kCanvasHeight,
                      useGraphicsCard)),
              position(VisualPlayPosition::getVisualPlayPosition(deckGroup)),
              play(control(deckGroup, QStringLiteral("play"))),
              rateRatio(control(deckGroup, QStringLiteral("rate_ratio"))),
              volume(control(deckGroup, QStringLiteral("volume"))),
              orientation(control(deckGroup, QStringLiteral("orientation"))),
              trackSamples(control(deckGroup, QStringLiteral("track_samples"))),
              trackSampleRate(control(deckGroup, QStringLiteral("track_samplerate"))) {
    }

    QString group;
    std::unique_ptr<VideoDecoder> decoder;
    QSharedPointer<VisualPlayPosition> position;
    PollingControlProxy play;
    PollingControlProxy rateRatio;
    PollingControlProxy volume;
    PollingControlProxy orientation;
    PollingControlProxy trackSamples;
    PollingControlProxy trackSampleRate;
    TrackPointer track;
    QString location;
    QImage still; // cover art + title, for tracks without video
    VideoDecoder::State loggedState = VideoDecoder::State::Closed;
};

VideoScreen::VideoScreen(VideoManager* pManager, QWidget* pParent, Qt::WindowFlags flags)
        : QWidget(pParent, flags),
          m_pManager(pManager) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
}

void VideoScreen::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    const QImage& canvas = m_pManager->canvas();
    if (canvas.isNull()) {
        return;
    }
    const QRect target = videomix::fitRect(canvas.size(), size());
    // Smooth only when shrinking (the preview); the projector gets the
    // picture 1:1 (or a fast upscale).
    painter.setRenderHint(QPainter::SmoothPixmapTransform, target.width() < canvas.width());
    painter.drawImage(target, canvas);
}

void VideoScreen::keyPressEvent(QKeyEvent* pEvent) {
    if (pEvent->key() == Qt::Key_Escape) {
        close();
        m_pManager->windowClosed();
        return;
    }
    QWidget::keyPressEvent(pEvent);
}

void VideoScreen::mouseDoubleClickEvent(QMouseEvent* pEvent) {
    Q_UNUSED(pEvent);
    // Double-click toggles full screen (on the screen the window is on).
    if (isFullScreen()) {
        showNormal();
        unsetCursor();
    } else {
        showFullScreen();
        setCursor(Qt::BlankCursor);
    }
}

VideoManager::VideoManager(QWidget* pParentWindow)
        : QObject(pParentWindow),
          m_pParentWindow(pParentWindow),
          m_numDecks(control(QStringLiteral("[App]"), QStringLiteral("num_decks"))),
          m_crossfader(control(QStringLiteral("[Master]"), QStringLiteral("crossfader"))),
          m_xfaderCurve(control(QString(EngineXfader::kXfaderConfigKey),
                  QStringLiteral("xFaderCurve"))),
          m_xfaderCalibration(control(QString(EngineXfader::kXfaderConfigKey),
                  QStringLiteral("xFaderCalibration"))),
          m_xfaderMode(control(QString(EngineXfader::kXfaderConfigKey),
                  QStringLiteral("xFaderMode"))),
          m_xfaderReverse(control(QString(EngineXfader::kXfaderConfigKey),
                  QStringLiteral("xFaderReverse"))) {
    m_canvas = QImage(kCanvasWidth, kCanvasHeight, QImage::Format_RGB32);
    m_canvas.fill(Qt::black);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        tick();
    });
}

VideoManager::~VideoManager() {
    m_timer.stop();
    delete m_pOutput;
    delete m_pPreview;
}

void VideoManager::ensureRunning() {
    if (!m_timer.isActive()) {
        m_lastMixKey.clear();
        m_statsTimer.start();
        m_composed = 0;
        m_composeMs = 0.0;
        m_timer.start();
        qInfo() << "Video: started";
    }
}

void VideoManager::showOnScreen(QScreen* pScreen) {
    if (!pScreen) {
        return;
    }
    if (!m_pOutput) {
        m_pOutput = new VideoScreen(this, m_pParentWindow, Qt::Window);
        m_pOutput->setWindowTitle(tr("Mixxx Video"));
    }
    m_pOutput->showNormal();
    m_pOutput->winId(); // make sure the native window exists
    if (QWindow* pWindow = m_pOutput->windowHandle()) {
        pWindow->setScreen(pScreen);
    }
    m_pOutput->setGeometry(pScreen->geometry());
    m_pOutput->showFullScreen();
    m_pOutput->setCursor(Qt::BlankCursor);
    qInfo() << "Video: full screen on" << pScreen->name() << pScreen->geometry();
    ensureRunning();
}

void VideoManager::showInWindow() {
    if (!m_pOutput) {
        m_pOutput = new VideoScreen(this, m_pParentWindow, Qt::Window);
        m_pOutput->setWindowTitle(tr("Mixxx Video"));
    }
    m_pOutput->showNormal();
    m_pOutput->unsetCursor();
    m_pOutput->resize(960, 540);
    m_pOutput->raise();
    ensureRunning();
}

void VideoManager::setPreviewVisible(bool visible) {
    if (visible) {
        if (!m_pPreview) {
            m_pPreview = new VideoScreen(this, m_pParentWindow, Qt::Tool);
            m_pPreview->setWindowTitle(tr("Video preview"));
            m_pPreview->resize(480, 270);
        }
        m_pPreview->show();
        m_pPreview->raise();
        ensureRunning();
    } else if (m_pPreview) {
        m_pPreview->hide();
    }
}

bool VideoManager::isPreviewVisible() const {
    return m_pPreview && m_pPreview->isVisible();
}

void VideoManager::stop() {
    if (m_pOutput) {
        m_pOutput->hide();
    }
    if (m_pPreview) {
        m_pPreview->hide();
    }
    windowClosed();
}

void VideoManager::windowClosed() {
    if (anyWindowVisible()) {
        return;
    }
    m_timer.stop();
    for (auto& pDeck : m_decks) {
        pDeck->decoder->open(QString()); // free the files and the decoders
        pDeck->location.clear();
        pDeck->track.reset();
    }
    qInfo() << "Video: stopped";
}

void VideoManager::setPictureDelayMs(int delayMs) {
    delayMs = std::clamp(delayMs, -kMaxPictureDelayMs, kMaxPictureDelayMs);
    if (delayMs != m_pictureDelayMs) {
        m_pictureDelayMs = delayMs;
        qDebug() << "Video: picture timing" << delayMs << "ms"; // many while sliding
    }
}

void VideoManager::setUseGraphicsCard(bool use) {
    if (use == m_useGraphicsCard) {
        return;
    }
    m_useGraphicsCard = use;
    qInfo() << "Video:" << (use ? "graphics card decoding on" : "graphics card decoding off");
    for (auto& pDeck : m_decks) {
        pDeck->decoder->setUseGraphicsCard(use);
    }
}

bool VideoManager::anyWindowVisible() const {
    return (m_pOutput && m_pOutput->isVisible()) || (m_pPreview && m_pPreview->isVisible());
}

QImage VideoManager::stillFor(const TrackPointer& pTrack) const {
    QImage still(kCanvasWidth, kCanvasHeight, QImage::Format_RGB32);
    still.fill(Qt::black);
    if (!pTrack) {
        return still;
    }
    QPainter painter(&still);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    const QImage cover = pTrack->getCoverInfoWithLocation().loadImage(pTrack).image;
    const QRect coverArea(0, 60, kCanvasWidth, 760);
    if (!cover.isNull()) {
        QRect r = videomix::fitRect(cover.size(), coverArea.size());
        r.translate(coverArea.topLeft());
        painter.drawImage(r, cover);
    }
    QFont font = painter.font();
    font.setPixelSize(64);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QString artist = pTrack->getArtist();
    const QString title = pTrack->getTitle();
    painter.drawText(QRect(60, 850, kCanvasWidth - 120, 90),
            Qt::AlignCenter | Qt::TextSingleLine,
            title.isEmpty() ? pTrack->getInfo() : title);
    font.setPixelSize(48);
    font.setBold(false);
    painter.setFont(font);
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(QRect(60, 950, kCanvasWidth - 120, 70),
            Qt::AlignCenter | Qt::TextSingleLine,
            artist);
    return still;
}

void VideoManager::updateDeckTrack(Deck& deck) {
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(deck.group);
    const QString location = pTrack ? pTrack->getLocation() : QString();
    if (location != deck.location) {
        deck.location = location;
        deck.track = pTrack;
        deck.decoder->open(location);
        deck.still = pTrack ? stillFor(pTrack) : QImage();
        deck.loggedState = VideoDecoder::State::Opening;
    }
    // Say once per track what was found.
    const VideoDecoder::State state = deck.decoder->state();
    if (state != deck.loggedState && state != VideoDecoder::State::Opening) {
        deck.loggedState = state;
        if (state == VideoDecoder::State::NoVideo) {
            qInfo() << "Video:" << deck.group << "no video in" << location
                    << "- showing cover art and title";
        } else if (state == VideoDecoder::State::Failed) {
            qWarning() << "Video:" << deck.group << "could not read the video of" << location
                       << "- showing cover art and title";
        }
    }
}

void VideoManager::tick() {
    if (!anyWindowVisible()) {
        windowClosed();
        return;
    }
    const int deckCount = std::clamp(static_cast<int>(m_numDecks.get()), 0, kMaxDecks);
    while (static_cast<int>(m_decks.size()) < deckCount) {
        m_decks.push_back(std::make_unique<Deck>(
                QStringLiteral("[Channel%1]").arg(m_decks.size() + 1), m_useGraphicsCard));
    }

    CSAMPLE_GAIN gainLeft = 1.0f;
    CSAMPLE_GAIN gainRight = 1.0f;
    EngineXfader::getXfadeGains(m_crossfader.get(),
            m_xfaderCurve.get(),
            m_xfaderCalibration.get(),
            m_xfaderMode.get(),
            m_xfaderReverse.toBool(),
            &gainLeft,
            &gainRight);

    QVector<videomix::DeckInput> inputs;
    QVector<QImage> pictures;
    QString mixKey;
    for (int i = 0; i < deckCount; ++i) {
        Deck& deck = *m_decks[i];
        updateDeckTrack(deck);
        videomix::DeckInput input;
        input.loaded = static_cast<bool>(deck.track);
        QImage picture;
        quint64 serial = 0;
        if (deck.track) {
            // The deck's position in seconds of the track (engine time).
            const double samples = deck.trackSamples.get();
            const double rate = deck.trackSampleRate.get();
            input.playing = deck.play.toBool();
            if (samples > 0.0 && rate > 0.0) {
                double seconds = deck.position->getEnginePlayPos() * samples / 2.0 / rate;
                // Picture timing, in real time: at a changed tempo that is
                // a different amount of the track.
                if (m_pictureDelayMs != 0 && input.playing) {
                    const double speed = std::max(0.0, deck.rateRatio.get());
                    seconds -= m_pictureDelayMs / 1000.0 * speed;
                }
                deck.decoder->setTarget(std::max(0.0, seconds));
            }
            input.volume = deck.volume.get();
            const auto orientation = static_cast<int>(deck.orientation.get());
            input.xfaderGain = orientation == EngineChannel::LEFT
                    ? gainLeft
                    : (orientation == EngineChannel::RIGHT ? gainRight : 1.0);
            picture = deck.decoder->frame(&serial);
            if (picture.isNull()) {
                picture = deck.still;
                serial = deck.still.cacheKey();
            }
        }
        inputs.append(input);
        pictures.append(picture);
        mixKey += QString::number(serial) + QChar(':');
    }
    const QVector<double> weights = videomix::weights(inputs);
    for (double w : weights) {
        mixKey += QString::number(std::lround(w * 200)) + QChar(',');
    }

    if (mixKey != m_lastMixKey) {
        m_lastMixKey = mixKey;
        const auto started = std::chrono::steady_clock::now();
        // Heaviest first, so the blend is built up from the main picture.
        QVector<int> order(deckCount);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&weights](int a, int b) {
            return weights[a] > weights[b];
        });
        QVector<double> orderedWeights;
        for (int i : order) {
            orderedWeights.append(weights[i]);
        }
        const QVector<double> opacities = videomix::layerOpacities(orderedWeights);
        QPainter painter(&m_canvas);
        painter.fillRect(m_canvas.rect(), Qt::black);
        for (int k = 0; k < order.size(); ++k) {
            const QImage& picture = pictures[order[k]];
            if (opacities[k] <= 0.0 || picture.isNull()) {
                continue;
            }
            // Each layer is the full screen: the picture with black bars.
            painter.setOpacity(opacities[k]);
            painter.fillRect(m_canvas.rect(), Qt::black);
            painter.drawImage(videomix::fitRect(picture.size(), m_canvas.size()), picture);
        }
        painter.end();
        m_composeMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                               .count();
        ++m_composed;
        if (m_pOutput && m_pOutput->isVisible()) {
            m_pOutput->update();
        }
        if (m_pPreview && m_pPreview->isVisible()) {
            m_pPreview->update();
        }
    }
    if (m_statsTimer.elapsed() >= kStatsEveryMs) {
        logStats();
    }
}

void VideoManager::logStats() {
    const double seconds = m_statsTimer.restart() / 1000.0;
    for (auto& pDeck : m_decks) {
        const VideoDecoder::Stats stats = pDeck->decoder->takeStats();
        if (stats.framesShown == 0 && stats.seeks == 0) {
            continue;
        }
        qInfo().noquote() << "Video:" << pDeck->group << stats.framesShown << "frames ("
                          << QString::number(stats.framesShown / seconds, 'f', 1) << "/s),"
                          << stats.seeks << "seeks," << stats.fromMemory
                          << "from memory (loops, jumps back), decoding"
                          << QString::number(stats.framesShown > stats.fromMemory
                                             ? stats.busyMs /
                                                     (stats.framesShown - stats.fromMemory)
                                             : stats.busyMs,
                                     'f',
                                     1)
                          << "ms per frame";
    }
    qInfo().noquote() << "Video: mixed" << m_composed << "pictures ("
                      << QString::number(m_composed / seconds, 'f', 1) << "/s),"
                      << QString::number(m_composed > 0 ? m_composeMs / m_composed : 0.0, 'f', 1)
                      << "ms each";
    m_composed = 0;
    m_composeMs = 0.0;
}
