#pragma once

#include <QByteArray>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <vector>

/// Auto DJ 2.0 plus Video Mixing: stem splitting maths, free of Mixxx and
/// of the AI engine, so it can be unit-tested.
///
/// The AI model (Demucs v4, "htdemucs") takes exactly kModelSegment stereo
/// frames at 44100 Hz and returns four parts: drums, bass, other, vocals.
/// A song is split into pieces of that length that overlap by a quarter,
/// and the pieces are blended with a triangle weight, the same way Demucs
/// does it.
namespace stems {

constexpr int kModelSampleRate = 44100;
constexpr int kModelSegment = 343980; ///< 7.8 s, the length the model was trained on
constexpr int kStemCount = 4;         ///< drums, bass, other, vocals (the model's order)
constexpr double kOverlap = 0.25;

/// One piece of the song for the model.
struct Piece {
    std::int64_t offset = 0; ///< first song frame this piece contributes
    std::int64_t size = 0;   ///< how many song frames it contributes
    std::int64_t start = 0;  ///< song frame at the model input's start
                             ///< (before 0 or past the end = silence)
};

/// The pieces covering a song of `length` frames.
std::vector<Piece> planPieces(std::int64_t length,
        int segment = kModelSegment,
        double overlap = kOverlap);

/// Blends the model's answers for the pieces into four whole parts. It
/// only keeps one model segment in memory: add the pieces in order, and
/// after each one flush() up to the next piece's offset (those frames are
/// final). Flush up to the song length at the end.
class Blender {
  public:
    /// `length` song frames; `segment` = the model's input length.
    Blender(std::int64_t length, int segment = kModelSegment);

    /// The model input for `piece` (planar, [2][segment]), cut from the
    /// normalised song `planar` ([2][length]); silence outside the song.
    void fillInput(const Piece& piece,
            const std::vector<float>& planar,
            std::vector<float>* pInput) const;
    /// Adds the model output ([4][2][segment]) for `piece`.
    void add(const Piece& piece, const float* pOutput);
    /// The finished parts from the last flush up to song frame `upTo`, as
    /// [4][2][count] (planar), un-normalised with mean/std. Returns count.
    std::int64_t flush(std::int64_t upTo, float mean, float std, std::vector<float>* pOut);

  private:
    std::int64_t m_length;
    int m_segment;
    std::int64_t m_base = 0; ///< song frame of m_sum[.][0]
    std::vector<float> m_weight;
    std::vector<float> m_sum;
    std::vector<float> m_total;
};

/// Mean and standard deviation of the whole song (both channels), used to
/// normalise the model input like Demucs does.
void meanAndStd(const std::vector<float>& samples, float* pMean, float* pStd);

/// Interleaved stereo <-> planar ([2][frames]).
std::vector<float> toPlanar(const float* pInterleaved, std::int64_t frames);
void toInterleaved(const float* pPlanar, std::int64_t frames, float* pInterleaved);

/// Good-quality sample rate conversion (windowed sinc) of one channel.
std::vector<float> resample(const float* pIn, std::int64_t frames, int fromRate, int toRate);
/// Number of frames resample() returns.
std::int64_t resampledLength(std::int64_t frames, int fromRate, int toRate);

/// The NI stem manifest (JSON) for drums, bass, other, vocals.
QByteArray stemManifest();
/// The moov box with the stem manifest added as moov/udta/stem (an
/// existing udta box is extended, else one is added). Empty if `moov` is
/// not a valid moov box.
QByteArray moovWithManifest(const QByteArray& moov, const QByteArray& manifest);
/// Adds the manifest to an MP4 file whose moov box is the last box (as
/// Windows writes it; then no other offsets change). False otherwise.
bool addStemManifestToFile(const QString& path, const QByteArray& manifest);

} // namespace stems
