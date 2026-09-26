#include "library/autodj/genrescanner.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include "defs_urls.h"
#include "moc_genrescanner.cpp"
#include "util/logger.h"
#include "util/versionstore.h"

namespace {

const mixxx::Logger kLogger("GenreScanner");

// MusicBrainz allows one request per second on average (per IP address).
// See <https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting>.
constexpr int kRequestGapMs = 1100;
constexpr int kRetryGapMs = 5000;
constexpr int kMaxRetries = 2;

const QString kBaseUrl = QStringLiteral("https://musicbrainz.org/ws/2/");

// The same identification Mixxx's own MusicBrainz lookups use.
QByteArray userAgent() {
    return (VersionStore::applicationName() + QStringLiteral("/") + VersionStore::version() +
            QStringLiteral(" ( ") + MIXXX_WEBSITE_URL + QStringLiteral(" )"))
            .toLatin1();
}

} // namespace

GenreScanner::GenreScanner(QObject* pParent)
        : QObject(pParent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &GenreScanner::sendRequest);
}

QString GenreScanner::itemName(int index) const {
    if (index < 0 || index >= m_items.size()) {
        return QString();
    }
    const Item& item = m_items[index];
    return item.artist.isEmpty() ? item.title : item.artist + QStringLiteral(" - ") + item.title;
}

void GenreScanner::start(const QList<Item>& items) {
    if (m_running) {
        return;
    }
    m_items = items;
    m_results.clear();
    m_index = 0;
    m_step = Step::Search;
    m_match = genrescan::Match();
    m_recordingGenres.clear();
    m_retries = 0;
    m_cancelled = false;
    m_running = true;
    kLogger.info() << "Genre scan of" << m_items.size() << "songs";
    if (m_items.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            done(false);
        });
        return;
    }
    emit progress(0, static_cast<int>(m_items.size()), itemName(0));
    m_timer.start(0);
}

void GenreScanner::cancel() {
    if (!m_running) {
        return;
    }
    m_cancelled = true;
    m_timer.stop();
    if (m_pReply) {
        m_pReply->abort(); // onReply() finishes the scan
    } else {
        done(true);
    }
}

void GenreScanner::sendRequest() {
    if (m_cancelled || m_index >= m_items.size()) {
        return;
    }
    const Item& item = m_items[m_index];
    QUrl url;
    QUrlQuery query;
    switch (m_step) {
    case Step::Search: {
        QString text = genrescan::recordingQuery(item.artist, item.title);
        if (text.isEmpty()) {
            finishItem(tr("no artist or title tag"));
            return;
        }
        text.replace(QChar('+'), QChar(' ')); // '+' would read as a space
        url = QUrl(kBaseUrl + QStringLiteral("recording/"));
        query.addQueryItem(QStringLiteral("query"), text);
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
        break;
    }
    case Step::Recording:
        url = QUrl(kBaseUrl + QStringLiteral("recording/") + m_match.recordingId);
        query.addQueryItem(QStringLiteral("inc"), QStringLiteral("genres"));
        break;
    case Step::Artist:
        url = QUrl(kBaseUrl + QStringLiteral("artist/") + m_match.artistId);
        query.addQueryItem(QStringLiteral("inc"), QStringLiteral("genres"));
        break;
    }
    query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent());
    request.setRawHeader("Accept", "application/json");
    m_pReply = m_network.get(request);
    connect(m_pReply, &QNetworkReply::finished, this, &GenreScanner::onReply);
}

void GenreScanner::onReply() {
    QNetworkReply* pReply = m_pReply;
    m_pReply = nullptr;
    if (!pReply) {
        return;
    }
    pReply->deleteLater();
    if (m_cancelled) {
        done(true);
        return;
    }
    const int status = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool busy = status == 503 || status == 429 ||
            (pReply->error() != QNetworkReply::NoError && status == 0);
    if (busy) {
        if (m_retries < kMaxRetries) {
            ++m_retries;
            kLogger.info() << "MusicBrainz busy or unreachable, retrying" << pReply->errorString();
            m_timer.start(kRetryGapMs);
            return;
        }
        finishItem(tr("MusicBrainz did not answer (%1)").arg(pReply->errorString()));
        return;
    }
    m_retries = 0;
    if (pReply->error() != QNetworkReply::NoError) {
        finishItem(tr("MusicBrainz error %1").arg(status));
        return;
    }
    const QByteArray body = pReply->readAll();
    const Item& item = m_items[m_index];
    switch (m_step) {
    case Step::Search: {
        const auto match = genrescan::pickRecording(body, item.artist, item.title);
        if (!match) {
            finishItem(tr("song not found on MusicBrainz"));
            return;
        }
        m_match = *match;
        m_step = Step::Recording;
        m_timer.start(kRequestGapMs);
        return;
    }
    case Step::Recording:
        m_recordingGenres = genrescan::parseGenres(body);
        if (!m_recordingGenres.isEmpty() || m_match.artistId.isEmpty() ||
                m_artistGenres.contains(m_match.artistId)) {
            finishItem(QString());
            return;
        }
        m_step = Step::Artist;
        m_timer.start(kRequestGapMs);
        return;
    case Step::Artist:
        m_artistGenres.insert(m_match.artistId, genrescan::parseGenres(body));
        finishItem(QString());
        return;
    }
}

void GenreScanner::finishItem(const QString& note) {
    Result result;
    result.item = m_items[m_index];
    if (note.isEmpty()) {
        const auto suggestion = genrescan::pickGenre(
                m_recordingGenres, m_artistGenres.value(m_match.artistId));
        if (suggestion) {
            result.suggested = suggestion->genre;
            result.source = suggestion->source;
            result.votes = suggestion->votes;
        } else {
            result.note = tr("no genre on MusicBrainz yet");
        }
    } else {
        result.note = note;
    }
    m_results.append(result);
    ++m_index;
    m_step = Step::Search;
    m_match = genrescan::Match();
    m_recordingGenres.clear();
    m_retries = 0;
    const int total = static_cast<int>(m_items.size());
    emit progress(m_index, total, itemName(m_index));
    if (m_index >= total) {
        done(false);
        return;
    }
    m_timer.start(kRequestGapMs);
}

void GenreScanner::done(bool cancelled) {
    if (!m_running) {
        return;
    }
    m_running = false;
    m_timer.stop();
    kLogger.info() << "Genre scan" << (cancelled ? "stopped" : "finished") << "after"
                   << m_results.size() << "songs";
    emit finished(m_results, cancelled);
}
