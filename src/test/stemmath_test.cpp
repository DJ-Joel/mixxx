#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cmath>
#include <vector>

#include "stems/stemmath.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

/// A pretend model that returns the input split evenly into four parts.
void fakeModel(const std::vector<float>& input, int segment, std::vector<float>* pOutput) {
    pOutput->assign(static_cast<std::size_t>(stems::kStemCount) * 2 * segment, 0.0f);
    for (int stem = 0; stem < stems::kStemCount; ++stem) {
        for (int i = 0; i < 2 * segment; ++i) {
            (*pOutput)[static_cast<std::size_t>(stem) * 2 * segment + i] = input[i] / 4.0f;
        }
    }
}

QByteArray box(const char* type, const QByteArray& payload) {
    QByteArray result(8, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(8 + payload.size()), result.data());
    std::copy(type, type + 4, result.data() + 4);
    return result + payload;
}

} // namespace

TEST(StemMathTest, PiecesCoverTheSong) {
    const std::int64_t length = 1000;
    const auto pieces = stems::planPieces(length, 100, 0.25);
    ASSERT_FALSE(pieces.empty());
    EXPECT_EQ(0, pieces.front().offset);
    EXPECT_EQ(75, pieces[1].offset); // a quarter overlap
    std::vector<int> covered(length, 0);
    for (const auto& piece : pieces) {
        EXPECT_LE(piece.size, 100);
        for (std::int64_t i = 0; i < piece.size; ++i) {
            ++covered[piece.offset + i];
        }
    }
    for (int count : covered) {
        EXPECT_GE(count, 1);
    }
    // The short last piece is centred in the model input.
    const auto& last = pieces.back();
    EXPECT_EQ(length, last.offset + last.size);
    EXPECT_EQ(last.offset - (100 - last.size) / 2, last.start);
    EXPECT_TRUE(stems::planPieces(0, 100).empty());
}

TEST(StemMathTest, BlendingGivesBackTheSong) {
    // With a model that splits evenly, the four parts add up to the song,
    // however the pieces overlap.
    const int segment = 400;
    const std::int64_t length = 2345;
    std::vector<float> song(2 * length);
    for (std::int64_t i = 0; i < 2 * length; ++i) {
        song[i] = static_cast<float>(std::sin(i * 0.013) * 0.5);
    }
    float mean = 0.0f;
    float std = 1.0f;
    stems::meanAndStd(song, &mean, &std);
    std::vector<float> normalised(song.size());
    for (std::size_t i = 0; i < song.size(); ++i) {
        normalised[i] = (song[i] - mean) / std;
    }
    stems::Blender blender(length, segment);
    const auto pieces = stems::planPieces(length, segment);
    std::vector<float> input;
    std::vector<float> output;
    std::vector<float> parts;
    std::vector<float> whole(static_cast<std::size_t>(stems::kStemCount) * 2 * length);
    std::int64_t done = 0;
    for (std::size_t k = 0; k < pieces.size(); ++k) {
        blender.fillInput(pieces[k], normalised, &input);
        fakeModel(input, segment, &output);
        blender.add(pieces[k], output.data());
        const std::int64_t upTo = k + 1 < pieces.size() ? pieces[k + 1].offset : length;
        const std::int64_t count = blender.flush(upTo, mean, std, &parts);
        for (int part = 0; part < stems::kStemCount * 2; ++part) {
            std::copy(parts.begin() + part * count,
                    parts.begin() + (part + 1) * count,
                    whole.begin() + part * length + done);
        }
        done += count;
    }
    ASSERT_EQ(length, done);
    for (int channel = 0; channel < 2; ++channel) {
        for (std::int64_t i = 0; i < length; ++i) {
            double sum = 0.0;
            for (int stem = 0; stem < stems::kStemCount; ++stem) {
                sum += whole[(stem * 2 + channel) * length + i];
            }
            // Each part carries the mean, so four parts carry it four times.
            ASSERT_NEAR(song[channel * length + i] + 3 * mean, sum, 1e-4) << i;
        }
    }
}

TEST(StemMathTest, PlanarAndInterleaved) {
    const float interleaved[6] = {1, 2, 3, 4, 5, 6};
    const auto planar = stems::toPlanar(interleaved, 3);
    EXPECT_EQ((std::vector<float>{1, 3, 5, 2, 4, 6}), planar);
    float back[6] = {};
    stems::toInterleaved(planar.data(), 3, back);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(interleaved[i], back[i]);
    }
}

TEST(StemMathTest, ResamplingKeepsTheTone) {
    // A 1 kHz tone at 48000 Hz becomes a 1 kHz tone at 44100 Hz.
    const int from = 48000;
    const int to = 44100;
    const std::int64_t frames = from; // one second
    std::vector<float> tone(frames);
    for (std::int64_t i = 0; i < frames; ++i) {
        tone[i] = static_cast<float>(0.8 * std::sin(2 * kPi * 1000.0 * i / from));
    }
    const auto out = stems::resample(tone.data(), frames, from, to);
    ASSERT_EQ(stems::resampledLength(frames, from, to), static_cast<std::int64_t>(out.size()));
    EXPECT_EQ(44100, static_cast<int>(out.size()));
    double worst = 0.0;
    for (std::int64_t n = 1000; n < 43000; ++n) { // away from the edges
        const double expected = 0.8 * std::sin(2 * kPi * 1000.0 * n / to);
        worst = std::max(worst, std::abs(out[n] - expected));
    }
    EXPECT_LT(worst, 0.002);
    // Same rate: unchanged.
    const auto same = stems::resample(tone.data(), frames, from, from);
    EXPECT_EQ(tone, same);
}

TEST(StemMathTest, ResamplingRemovesTooHighTones) {
    // 23 kHz fits at 48000 Hz but not at 44100 Hz: it must not come back
    // as a false lower tone.
    const int from = 48000;
    const std::int64_t frames = from;
    std::vector<float> tone(frames);
    for (std::int64_t i = 0; i < frames; ++i) {
        tone[i] = static_cast<float>(0.8 * std::sin(2 * kPi * 23000.0 * i / from));
    }
    const auto out = stems::resample(tone.data(), frames, from, 44100);
    double peak = 0.0;
    for (std::int64_t n = 1000; n < 43000; ++n) {
        peak = std::max(peak, static_cast<double>(std::abs(out[n])));
    }
    EXPECT_LT(peak, 0.001);
}

TEST(StemMathTest, ManifestIsAddedToTheMoovBox) {
    const QByteArray manifest = stems::stemManifest();
    const auto json = QJsonDocument::fromJson(manifest).object();
    EXPECT_EQ(1, json.value("version").toInt());
    EXPECT_EQ(4, json.value("stems").toArray().size());

    // moov with mvhd only: a udta box is added.
    const QByteArray mvhd = box("mvhd", QByteArray(20, 'x'));
    const QByteArray moov = box("moov", mvhd);
    const QByteArray patched = stems::moovWithManifest(moov, manifest);
    ASSERT_FALSE(patched.isEmpty());
    EXPECT_EQ(patched.size(), static_cast<int>(qFromBigEndian<quint32>(patched.constData())));
    EXPECT_EQ(box("moov", mvhd + box("udta", box("stem", manifest))), patched);

    // moov with a udta box: the stem box goes inside it.
    const QByteArray meta = box("meta", QByteArray(4, 'm'));
    const QByteArray moov2 = box("moov", mvhd + box("udta", meta) + box("trak", "t"));
    EXPECT_EQ(box("moov", mvhd + box("udta", meta + box("stem", manifest)) + box("trak", "t")),
            stems::moovWithManifest(moov2, manifest));

    // Not a moov box: nothing.
    EXPECT_TRUE(stems::moovWithManifest(box("free", "x"), manifest).isEmpty());
}

TEST(StemMathTest, ResamplingPieceByPieceIsTheSame) {
    // The splitter converts the parts back in pieces as they are finished.
    std::vector<float> sound(100000);
    for (std::size_t i = 0; i < sound.size(); ++i) {
        sound[i] = static_cast<float>(std::sin(i * 0.05) * 0.5 + std::sin(i * 0.31) * 0.2);
    }
    for (const auto& rates : {std::pair<int, int>{44100, 48000}, {48000, 44100}}) {
        const auto whole = stems::resample(sound.data(), sound.size(), rates.first, rates.second);
        stems::Resampler resampler(rates.first, rates.second);
        std::vector<float> pieces;
        std::size_t at = 0;
        std::size_t step = 1;
        while (at < sound.size()) {
            const std::size_t count = std::min(step, sound.size() - at);
            resampler.push(sound.data() + at, count, &pieces);
            at += count;
            step = step * 3 + 7; // uneven piece sizes
        }
        resampler.finish(&pieces);
        ASSERT_EQ(whole.size(), pieces.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            ASSERT_FLOAT_EQ(whole[i], pieces[i]) << i;
        }
    }
}
