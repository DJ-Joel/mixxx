#include "waveform/renderers/allshader/waveformrenderdownbeat.h"

#include <QDomNode>
#include <algorithm>

#include "analyzer/analyzerenergy.h"
#include "moc_waveformrenderdownbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/unicolormaterial.h"
#include "rendergraph/vertexupdaters/vertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace allshader {

namespace {
constexpr int kBeatsPerBar = 4;
constexpr int kBarsPerPhrase = 8;
} // namespace

WaveformRenderDownbeat::WaveformRenderDownbeat(
        WaveformWidgetRenderer* waveformWidget, Kind kind)
        : ::WaveformRendererAbstract(waveformWidget),
          m_kind(kind) {
    initForRectangles<UniColorMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRenderDownbeat::setup(const QDomNode& node, const SkinContext& skinContext) {
    // Skins can set DownbeatColor / PhraseColor; otherwise white bars and
    // amber phrases (they stand out from the red/blue waveforms and the
    // hot cue colours).
    const QString name = m_kind == Kind::Bars ? QStringLiteral("DownbeatColor")
                                              : QStringLiteral("PhraseColor");
    QColor color(skinContext.selectString(node, name));
    if (!color.isValid()) {
        color = m_kind == Kind::Bars ? QColor(255, 255, 255, 230) : QColor(255, 176, 0, 255);
    }
    m_color = WSkinColor::getCorrectColor(color).toRgb();
}

void WaveformRenderDownbeat::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderDownbeat::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
    }
}

bool WaveformRenderDownbeat::updateBars() {
    const TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    const mixxx::BeatsPointer pBeats = pTrack ? pTrack->getBeats() : mixxx::BeatsPointer();
    if (!pTrack || !pBeats) {
        m_frames.clear();
        m_trackId = TrackId();
        m_pBeats = nullptr;
        return false;
    }
    // Beat 1 comes from the Intro End marker: redo when it moves.
    const CuePointer pIntro = pTrack->findCueByType(mixxx::CueType::Intro);
    const mixxx::audio::FramePos introEnd =
            pIntro ? pIntro->getEndPosition() : mixxx::audio::kInvalidFramePos;
    const double introEndFrame = introEnd.isValid() ? introEnd.value() : -1.0;
    if (pTrack->getId() == m_trackId && pBeats.get() == m_pBeats &&
            introEndFrame == m_introEndFrame) {
        return !m_frames.empty(); // nothing changed
    }
    m_trackId = pTrack->getId();
    m_pBeats = pBeats.get();
    m_introEndFrame = introEndFrame;
    m_frames.clear();
    bool known = false;
    const int phase = AnalyzerEnergy::beatOnePhase(pTrack, &known);
    if (!known) {
        return false; // no Intro End marker: beat 1 unknown, no flags
    }
    // The same beats the analysis counted (AnalyzerEnergy::beatGrid).
    const double sampleRate = pTrack->getSampleRate().value();
    const double endFrame = (pTrack->getDuration() + 1.0) * sampleRate;
    std::vector<double> beats;
    for (auto it = pBeats->iteratorFrom(pBeats->firstBeat()); beats.size() < 20000; ++it) {
        const double frame = it->value();
        if ((sampleRate > 0.0 && frame > endFrame) || (!beats.empty() && frame <= beats.back())) {
            break;
        }
        beats.push_back(frame);
    }
    const int step = m_kind == Kind::Bars ? kBeatsPerBar : kBeatsPerBar * kBarsPerPhrase;
    for (std::size_t i = static_cast<std::size_t>(std::max(0, phase)); i < beats.size();
            i += static_cast<std::size_t>(step)) {
        m_frames.push_back(beats[i]);
    }
    return !m_frames.empty();
}

bool WaveformRenderDownbeat::preprocessInner() {
    if (!updateBars() || !m_color.alpha()) {
        return false;
    }
    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }
    const auto positionType = ::WaveformRendererAbstract::Play;
    // Engine sample positions are frames x 2.
    const double firstFrame =
            m_waveformRenderer->getFirstDisplayedPosition(positionType) * trackSamples / 2.0;
    const double lastFrame =
            m_waveformRenderer->getLastDisplayedPosition(positionType) * trackSamples / 2.0;
    const auto from = std::lower_bound(m_frames.begin(), m_frames.end(), firstFrame);
    const auto to = std::upper_bound(m_frames.begin(), m_frames.end(), lastFrame);
    const int count = static_cast<int>(to - from);

    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth());
    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    const bool phrases = m_kind == Kind::Phrases;
    // Bars: a flag at each edge. Phrases: bigger flags and a thin line.
    const float halfWidth = phrases ? 3.f : 1.5f;
    const float flag = breadth * (phrases ? 0.2f : 0.12f);
    const int rectanglesPer = phrases ? 3 : 2;
    const int verticesPerRectangle = 6;
    const int reserved = count * rectanglesPer * verticesPerRectangle;
    geometry().allocate(reserved);
    VertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::Point2D>()};
    for (auto it = from; it != to; ++it) {
        double x = m_waveformRenderer->transformSamplePositionInRendererWorld(
                *it * 2.0, positionType);
        x = qRound(x * devicePixelRatio) / devicePixelRatio;
        const float cx = static_cast<float>(x);
        vertexUpdater.addRectangle({cx - halfWidth, 0.f}, {cx + halfWidth, flag});
        vertexUpdater.addRectangle({cx - halfWidth, breadth - flag}, {cx + halfWidth, breadth});
        if (phrases) {
            vertexUpdater.addRectangle({cx, 0.f}, {cx + 1.f, breadth});
        }
    }
    markDirtyGeometry();
    DEBUG_ASSERT(reserved == vertexUpdater.index());
    material().setUniform(1, m_color);
    markDirtyMaterial();
    return true;
}

} // namespace allshader
