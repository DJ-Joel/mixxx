#pragma once

#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

#include "library/autodj/smart/genrescan.h"
#include "track/trackid.h"

class QNetworkReply;

/// Auto DJ 2.0 plus Video Mixing Genre Scan: looks up each song on MusicBrainz, one request
/// per second as MusicBrainz asks, and reports a suggested genre per song.
/// Nothing is changed here; the DJ reviews the results and applies them.
class GenreScanner : public QObject {
    Q_OBJECT
  public:
    struct Item {
        TrackId id;
        QString artist;
        QString title;
        QString currentGenre;
    };
    struct Result {
        Item item;
        QString suggested; ///< empty = nothing found (see note)
        QString source;    ///< "song" or "artist"
        int votes = 0;
        QString note; ///< why nothing was found
    };

    explicit GenreScanner(QObject* pParent = nullptr);

    void start(const QList<Item>& items);
    void cancel();
    bool isRunning() const {
        return m_running;
    }

  signals:
    void progress(int done, int total, const QString& current);
    void finished(const QList<GenreScanner::Result>& results, bool cancelled);

  private:
    enum class Step {
        Search,
        Recording,
        Artist,
    };
    void sendRequest();
    void onReply();
    void finishItem(const QString& note);
    void done(bool cancelled);
    QString itemName(int index) const;

    QNetworkAccessManager m_network;
    QTimer m_timer;
    QList<Item> m_items;
    QList<Result> m_results;
    int m_index = 0;
    Step m_step = Step::Search;
    genrescan::Match m_match;
    QList<genrescan::Genre> m_recordingGenres;
    QHash<QString, QList<genrescan::Genre>> m_artistGenres; // cache per artist id
    QPointer<QNetworkReply> m_pReply;
    int m_retries = 0;
    bool m_running = false;
    bool m_cancelled = false;
};
