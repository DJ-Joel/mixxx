#include <gtest/gtest.h>

#include "library/autodj/smart/genrescan.h"

using namespace genrescan;

namespace {
// Shaped like MusicBrainz /ws/2/recording?query=...&fmt=json.
const QByteArray kSearch = R"({
  "recordings": [
    {"id": "wrong-song", "score": 100, "title": "Temple of Love",
     "artist-credit": [{"name": "The Sisters of Mercy",
                        "artist": {"id": "a-som", "name": "The Sisters of Mercy"}}]},
    {"id": "cover", "score": 98, "title": "Lucretia My Reflection",
     "artist-credit": [{"name": "Destroid", "artist": {"id": "a-d", "name": "Destroid"}}]},
    {"id": "right", "score": 95, "title": "Lucretia My Reflection",
     "artist-credit": [{"name": "The Sisters of Mercy",
                        "artist": {"id": "a-som", "name": "The Sisters of Mercy"}}]},
    {"id": "low", "score": 40, "title": "Lucretia My Reflection",
     "artist-credit": [{"name": "Sisters of Mercy", "artist": {"id": "x", "name": "Sisters of Mercy"}}]}
  ]})";
} // namespace

TEST(GenreScanTest, SearchTitleDropsVersionTags) {
    EXPECT_EQ(QStringLiteral("Fade To Grey"), searchTitle(QStringLiteral("Fade To Grey (12\" Version)")));
    EXPECT_EQ(QStringLiteral("Blue Monday"), searchTitle(QStringLiteral("Blue Monday - 1988 Remix")));
    EXPECT_EQ(QStringLiteral("Back That Azz Up"),
            searchTitle(QStringLiteral("Back That Azz Up feat. Mannie Fresh")));
}

TEST(GenreScanTest, QueryIsQuotedAndNeedsArtistAndTitle) {
    EXPECT_EQ(QStringLiteral("recording:\"Say \\\"Yes\\\"\" AND artist:\"Band\""),
            recordingQuery(QStringLiteral("Band"), QStringLiteral("Say \"Yes\" (Extended)")));
    EXPECT_TRUE(recordingQuery(QString(), QStringLiteral("Song")).isEmpty());
    EXPECT_TRUE(recordingQuery(QStringLiteral("Band"), QString()).isEmpty());
}

TEST(GenreScanTest, PicksOnlyTheSameSongByTheSameArtist) {
    const auto m = pickRecording(kSearch,
            QStringLiteral("Sisters of Mercy"),
            QStringLiteral("Lucretia My Reflection (Extended)"));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(QStringLiteral("right"), m->recordingId);
    EXPECT_EQ(QStringLiteral("a-som"), m->artistId);
    // No match for a song that is not in the results.
    EXPECT_FALSE(pickRecording(kSearch, QStringLiteral("Sisters of Mercy"), QStringLiteral("Alice"))
                         .has_value());
    // Garbage in: no match, no crash.
    EXPECT_FALSE(pickRecording("not json", QStringLiteral("A"), QStringLiteral("B")).has_value());
}

TEST(GenreScanTest, SongGenreFirstThenArtist) {
    const QByteArray recording = R"({"id":"r","genres":[
        {"name":"post-punk","count":2},{"name":"gothic rock","count":5}]})";
    const QByteArray artist = R"({"id":"a","genres":[{"name":"darkwave","count":9}]})";
    const auto song = pickGenre(parseGenres(recording), parseGenres(artist));
    ASSERT_TRUE(song.has_value());
    EXPECT_EQ(QStringLiteral("Gothic Rock"), song->genre);
    EXPECT_EQ(QStringLiteral("song"), song->source);
    EXPECT_EQ(5, song->votes);
    const auto byArtist = pickGenre({}, parseGenres(artist));
    ASSERT_TRUE(byArtist.has_value());
    EXPECT_EQ(QStringLiteral("Darkwave"), byArtist->genre);
    EXPECT_EQ(QStringLiteral("artist"), byArtist->source);
    EXPECT_FALSE(pickGenre({}, {}).has_value());
}

TEST(GenreScanTest, DisplayAndEmptyGenres) {
    EXPECT_EQ(QStringLiteral("Synth-Pop"), displayGenre(QStringLiteral("synth-pop")));
    EXPECT_EQ(QStringLiteral("EBM"), displayGenre(QStringLiteral("ebm")));
    EXPECT_EQ(QStringLiteral("Electronic Body Music"), displayGenre(QStringLiteral("electronic body music")));
    EXPECT_TRUE(isEmptyGenre(QStringLiteral(" Other ")));
    EXPECT_TRUE(isEmptyGenre(QString()));
    EXPECT_FALSE(isEmptyGenre(QStringLiteral("Goth")));
}
