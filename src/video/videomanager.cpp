#include "video/videomanager.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QFontMetrics>
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
#include "video/videoaudiotap.h"
#include "video/videodecoder.h"
#include "video/videomix.h"
#include "video/videorecorder.h"
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
              beatActive(control(deckGroup, QStringLiteral("beat_active"))),
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
    PollingControlProxy beatActive;
    PollingControlProxy rateRatio;
    PollingControlProxy volume;
    PollingControlProxy orientation;
    PollingControlProxy trackSamples;
    PollingControlProxy trackSampleRate;
    TrackPointer track;
    QString location;
    QImage still; // cover art + title, for tracks without video
    bool beatWasActive = false;
    double lastBeatSec = -100.0; // when its last beat started (m_clock)
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
                  QStringLiteral("xFaderReverse"))),
          m_sampleRate(control(QStringLiteral("[App]"), QStringLiteral("samplerate"))) {
    m_canvas = QImage(kCanvasWidth, kCanvasHeight, QImage::Format_RGB32);
    m_canvas.fill(Qt::black);
    m_clock.start();
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        tick();
    });
    m_recordingCheck.setInterval(250);
    connect(&m_recordingCheck, &QTimer::timeout, this, [this]() {
        checkRecordingFinished();
    });
}

VideoManager::~VideoManager() {
    m_timer.stop();
    m_recordingCheck.stop();
    if (m_pRecorder) {
        // Mixxx is closing: complete the file with the sound received so far.
        VideoAudioTap::setSink({});
        m_pRecorder.reset(); // waits for the file to be completed
    }
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
    if (anyWindowVisible() || m_pRecorder) {
        return; // (a recording keeps the video running without a window)
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

void VideoManager::setTransition(Transition transition) {
    if (transition != m_transition) {
        m_transition = transition;
        m_cutTarget = -1;
        m_lastMixKey.clear();
        qInfo() << "Video: transitions"
                << (transition == Transition::Cut ? "cut on the beat" : "crossfade");
    }
}

void VideoManager::setShowTitles(bool show) {
    m_showTitles = show;
    m_lastMixKey.clear();
}

void VideoManager::setMovingPictures(bool moving) {
    m_movingPictures = moving;
    m_lastMixKey.clear();
}

void VideoManager::setBrand(const QString& text, const QString& logoPath) {
    m_brandText = text.trimmed();
    m_brandLogo = QImage();
    if (!logoPath.isEmpty()) {
        const QImage logo(logoPath);
        if (logo.isNull()) {
            qWarning() << "Video: cannot read the logo" << logoPath;
        } else {
            // At most 400 x 160 on the 1920 x 1080 screen.
            m_brandLogo = logo.scaled(400, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    m_lastMixKey.clear();
}

void VideoManager::drawOverlays(QPainter* pPainter, double nowSec) {
    // The DJ's name and/or logo, top right.
    constexpr int kMargin = 40;
    int y = kMargin;
    if (!m_brandLogo.isNull()) {
        pPainter->setOpacity(0.85);
        pPainter->drawImage(QPoint(kCanvasWidth - kMargin - m_brandLogo.width(), y), m_brandLogo);
        y += m_brandLogo.height() + 10;
    }
    if (!m_brandText.isEmpty()) {
        QFont font = pPainter->font();
        font.setPixelSize(44);
        font.setBold(true);
        pPainter->setFont(font);
        const QRect area(kMargin, y, kCanvasWidth - 2 * kMargin, 60);
        pPainter->setOpacity(0.6);
        pPainter->setPen(Qt::black);
        pPainter->drawText(area.translated(3, 3), Qt::AlignRight | Qt::AlignTop, m_brandText);
        pPainter->setOpacity(0.85);
        pPainter->setPen(Qt::white);
        pPainter->drawText(area, Qt::AlignRight | Qt::AlignTop, m_brandText);
    }
    // The song title, bottom, while it is fading in, showing or fading out.
    const double titleOpacity =
            m_showTitles ? videomix::titleOpacity(nowSec - m_titleStart) : 0.0;
    if (titleOpacity > 0.0 && !m_titleText.isEmpty()) {
        const QRect band(0, kCanvasHeight - 190, kCanvasWidth, 130);
        pPainter->setOpacity(0.55 * titleOpacity);
        pPainter->fillRect(band, Qt::black);
        QFont font = pPainter->font();
        font.setPixelSize(56);
        font.setBold(true);
        pPainter->setFont(font);
        pPainter->setOpacity(titleOpacity);
        pPainter->setPen(Qt::white);
        pPainter->drawText(band.adjusted(60, 0, -60, 0),
                Qt::AlignCenter | Qt::TextSingleLine,
                QFontMetrics(font).elidedText(m_titleText, Qt::ElideRight, band.width() - 120));
    }
    pPainter->setOpacity(1.0);
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
    if (!anyWindowVisible() && !m_pRecorder) {
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

    const double nowSec = m_clock.elapsed() / 1000.0;
    QVector<videomix::DeckInput> inputs;
    QVector<QImage> pictures;
    QVector<bool> isStill;
    QString mixKey;
    for (int i = 0; i < deckCount; ++i) {
        Deck& deck = *m_decks[i];
        updateDeckTrack(deck);
        videomix::DeckInput input;
        input.loaded = static_cast<bool>(deck.track);
        QImage picture;
        bool still = false;
        quint64 serial = 0;
        if (deck.track) {
            input.playing = deck.play.toBool();
            // The deck's position in seconds of the track (engine time).
            const double samples = deck.trackSamples.get();
            const double rate = deck.trackSampleRate.get();
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
            // Beats (for cuts and pulses): the start of each beat.
            const bool beat = deck.beatActive.toBool();
            if (beat && !deck.beatWasActive && input.playing) {
                deck.lastBeatSec = nowSec;
            }
            deck.beatWasActive = beat;
            input.volume = deck.volume.get();
            const auto orientation = static_cast<int>(deck.orientation.get());
            input.xfaderGain = orientation == EngineChannel::LEFT
                    ? gainLeft
                    : (orientation == EngineChannel::RIGHT ? gainRight : 1.0);
            picture = deck.decoder->frame(&serial);
            if (picture.isNull()) {
                picture = deck.still;
                serial = deck.still.cacheKey();
                still = true;
            }
        }
        inputs.append(input);
        pictures.append(picture);
        isStill.append(still);
        mixKey += QString::number(serial) + QChar(':');
    }
    QVector<double> weights = videomix::weights(inputs);

    // Cut on the beat: one deck at a time. A new deck takes over on its
    // next beat (or after kCutWaitSec), or at once when the one on screen
    // went silent.
    if (m_transition == Transition::Cut) {
        const int target = videomix::mainDeck(weights, m_shownDeck);
        if (target != m_shownDeck) {
            const bool shownSilent = m_shownDeck < 0 || m_shownDeck >= weights.size() ||
                    weights[m_shownDeck] <= 0.0;
            if (target != m_cutTarget) {
                m_cutTarget = target;
                m_cutSince = nowSec;
            }
            const bool beatNow = target >= 0 && m_decks[target]->lastBeatSec >= m_cutSince;
            if (shownSilent || target < 0 || beatNow ||
                    nowSec - m_cutSince >= videomix::kCutWaitSec) {
                m_shownDeck = target;
                m_cutTarget = -1;
            }
        } else {
            m_cutTarget = -1;
        }
        for (int i = 0; i < weights.size(); ++i) {
            weights[i] = i == m_shownDeck ? 1.0 : 0.0;
        }
    }

    // Song titles: when a song takes over the screen.
    const int titleDeck = m_transition == Transition::Cut
            ? m_shownDeck
            : videomix::mainDeck(weights, m_titleDeck);
    m_titleDeck = titleDeck;
    if (titleDeck >= 0 && m_decks[titleDeck]->track &&
            m_decks[titleDeck]->track->getId() != m_titledTrack) {
        const TrackPointer& pTrack = m_decks[titleDeck]->track;
        m_titledTrack = pTrack->getId();
        const QString artist = pTrack->getArtist();
        const QString title = pTrack->getTitle();
        m_titleText = artist.isEmpty() || title.isEmpty()
                ? pTrack->getInfo()
                : artist + QStringLiteral(" - ") + title;
        m_titleStart = nowSec;
    }

    // Moving pictures: songs without video pulse with their beat.
    QVector<double> pulses(deckCount, 0.0);
    for (int i = 0; i < deckCount; ++i) {
        if (m_movingPictures && isStill[i] && inputs[i].playing && weights[i] > 0.0) {
            pulses[i] = videomix::beatPulse(nowSec - m_decks[i]->lastBeatSec);
        }
        mixKey += QString::number(std::lround(pulses[i] * 2000)) + QChar(';');
    }
    for (double w : weights) {
        mixKey += QString::number(std::lround(w * 200)) + QChar(',');
    }
    if (m_showTitles) {
        mixKey += QString::number(std::lround(videomix::titleOpacity(nowSec - m_titleStart) * 50));
    }

    const bool newPicture = mixKey != m_lastMixKey;
    if (newPicture) {
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
            const int i = order[k];
            const QImage& picture = pictures[i];
            if (opacities[k] <= 0.0 || picture.isNull()) {
                continue;
            }
            // Each layer is the full screen: the picture with black bars.
            painter.setOpacity(opacities[k]);
            painter.fillRect(m_canvas.rect(), Qt::black);
            QRectF target = videomix::fitRect(picture.size(), m_canvas.size());
            if (pulses[i] > 0.0) {
                // A little bigger on the beat, around the centre.
                const QPointF centre = target.center();
                target.setSize(target.size() * (1.0 + pulses[i]));
                target.moveCenter(centre);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            }
            painter.drawImage(target, picture);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        }
        painter.setOpacity(1.0);
        drawOverlays(&painter, nowSec);
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
    if (m_pRecorder) {
        recordPicture(newPicture);
    }
    if (m_statsTimer.elapsed() >= kStatsEveryMs) {
        logStats();
    }
}

bool VideoManager::canRecord() {
    return VideoRecorder::isSupported();
}

bool VideoManager::startRecording(const QString& path, QString* pError) {
    if (m_pRecorder) {
        *pError = m_recordingStopping
                ? tr("The last recording is still being saved. Try again in a moment.")
                : tr("A video recording is already running.");
        return false;
    }
    const qint64 engineFrame = VideoAudioTap::engineFrames();
    if (engineFrame < 0) {
        *pError = tr("Mixxx's recording channel is not available, so the sound "
                     "cannot be recorded.");
        return false;
    }
    VideoRecorder::Settings settings;
    settings.path = path;
    settings.width = kCanvasWidth;
    settings.height = kCanvasHeight;
    settings.sampleRate = static_cast<int>(m_sampleRate.get());
    settings.startFrame = engineFrame;
    auto pRecorder = std::make_unique<VideoRecorder>();
    if (!pRecorder->start(settings, pError)) {
        return false;
    }
    m_pRecorder = std::move(pRecorder);
    m_recordingStopping = false;
    VideoRecorder* pTarget = m_pRecorder.get();
    VideoAudioTap::setSink([pTarget](const float* pSamples, std::size_t count, qint64 first) {
        pTarget->addAudio(pSamples, count, first);
    });
    m_lastMixKey.clear(); // draw the first picture at once
    ensureRunning();
    m_recordingCheck.start();
    return true;
}

void VideoManager::stopRecording() {
    if (!m_pRecorder || m_recordingStopping) {
        return;
    }
    m_recordingStopping = true;
    // The sound up to now still has to arrive, so the sound stays connected
    // until the file is complete (checkRecordingFinished).
    const qint64 engineFrame = VideoAudioTap::engineFrames();
    m_pRecorder->stop(engineFrame >= 0 ? engineFrame : m_pRecorder->settings().startFrame);
}

bool VideoManager::isRecording() const {
    return m_pRecorder && !m_recordingStopping;
}

double VideoManager::recordingSeconds() const {
    return m_pRecorder ? m_pRecorder->secondsRecorded(VideoAudioTap::engineFrames()) : 0.0;
}

void VideoManager::recordPicture(bool newPicture) {
    const qint64 engineFrame = VideoAudioTap::engineFrames();
    if (engineFrame < 0 || m_recordingStopping) {
        return;
    }
    // The picture on the canvas belongs to the sound "picture timing"
    // earlier (or later), so it is stamped with that time: the recording is
    // in step even when the screen is set to show the picture early.
    const qint64 delayFrames = static_cast<qint64>(std::llround(
            m_pictureDelayMs / 1000.0 * m_pRecorder->settings().sampleRate));
    m_pRecorder->addPicture(engineFrame - delayFrames, newPicture ? m_canvas : QImage());
}

void VideoManager::checkRecordingFinished() {
    if (!m_pRecorder || !m_pRecorder->isFinished()) {
        return;
    }
    VideoAudioTap::setSink({}); // before the recorder goes away
    const QString path = m_pRecorder->settings().path;
    const QString error = m_pRecorder->error();
    m_pRecorder.reset();
    m_recordingStopping = false;
    m_recordingCheck.stop();
    if (!anyWindowVisible()) {
        windowClosed(); // nothing else needs the video now
    }
    emit recordingFinished(path, error);
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
