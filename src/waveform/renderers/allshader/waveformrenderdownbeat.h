#pragma once

#include <QColor>
#include <vector>

#include "rendergraph/geometrynode.h"
#include "track/trackid.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class QDomNode;
class SkinContext;

namespace allshader {
class WaveformRenderDownbeat;
} // namespace allshader

/// Auto DJ 2.0 plus Video Mixing: marks beat 1 of every bar with small flags
/// at the top and bottom edge of the waveform (drawn on top of it, so loud
/// music never hides them), and every 8-bar phrase start with bigger flags
/// and a line. Beat 1 = where the main beat kicks in (the Intro End marker,
/// AnalyzerEnergy::beatOnePhase); songs without one get no flags. Moving
/// the marker moves the flags. Two of these renderers are used: one for the bars, one
/// for the phrases (each draws in one colour).
class allshader::WaveformRenderDownbeat final
        : public QObject,
          public ::WaveformRendererAbstract,
          public rendergraph::GeometryNode {
    Q_OBJECT
  public:
    enum class Kind {
        Bars,
        Phrases,
    };
    explicit WaveformRenderDownbeat(WaveformWidgetRenderer* waveformWidget, Kind kind);

    // Pure virtual from WaveformRendererAbstract, not used
    void draw(QPainter* painter, QPaintEvent* event) override final;
    void setup(const QDomNode& node, const SkinContext& skinContext) override;
    void preprocess() override;

  private:
    bool preprocessInner();
    /// The frame positions of beat 1 of each bar (and whether it starts a
    /// phrase), worked out again when the song, its grid or its beat 1
    /// changes.
    bool updateBars();

    Kind m_kind;
    QColor m_color;
    TrackId m_trackId;
    const void* m_pBeats = nullptr; ///< the grid the bars were worked out for
    double m_introEndFrame = -2.0; ///< the Intro End marker they were worked out for
    std::vector<double> m_frames; ///< beat 1 of each bar (or each phrase)

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderDownbeat);
};
