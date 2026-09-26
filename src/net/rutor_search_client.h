#ifndef RATS_NET_RUTOR_SEARCH_CLIENT_H
#define RATS_NET_RUTOR_SEARCH_CLIENT_H

#include "domain/torrent.h"

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace rats::net {

// Small asynchronous transport around the pure RutorSource parser.
//
// It searches Rutor first, keeps the concrete detail URL attached to every
// candidate, fetches that exact page, and emits a result only after
// RutorSource::applyDetailPage() has re-proved the info-hash and
// RutorSource::isStrictComplete() accepts the release information.
//
// This class deliberately knows nothing about Application, repositories or UI.
class RutorSearchClient : public QObject {
    Q_OBJECT

public:
    explicit RutorSearchClient(QObject* parent = nullptr);
    ~RutorSearchClient() override;

    void search(const QString& query, int limit = 50,
        const QString& sortKey = QStringLiteral("seeders_desc"));
    void cancel();

signals:
    void resultReady(const QString& query, const rats::domain::Torrent& torrent);
    void searchFinished(
        const QString& query, int accepted, int rejected, const QString& error);

private:
    struct DetailJob {
        domain::Torrent torrent;
        QUrl url;
        bool mirrorRetried = false;
    };

    void fetchSearchPage(const QUrl& url, int generation, bool mirrorRetried);
    void processQueue(int generation);
    void fetchDetail(DetailJob job, int generation);
    void finishIfIdle(int generation);
    void finishNow(int generation, const QString& error = QString());

    static QUrl alternateMirror(const QUrl& url);

    QNetworkAccessManager* networkManager_ = nullptr;
    QSet<QNetworkReply*> replies_;
    QQueue<DetailJob> detailQueue_;

    int generation_ = 0;
    int activeDetails_ = 0;
    int requestedLimit_ = 50;
    int accepted_ = 0;
    int rejected_ = 0;
    bool searchPageResolved_ = false;
    bool finishedEmitted_ = true;
    QString currentQuery_;
    QString currentSortKey_;

    static constexpr int kTimeoutMs = 15000;
    static constexpr int kMaxConcurrentDetails = 4;
};

} // namespace rats::net

#endif // RATS_NET_RUTOR_SEARCH_CLIENT_H
