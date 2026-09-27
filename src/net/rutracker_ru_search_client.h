#ifndef RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H
#define RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H

#include "domain/torrent.h"

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace rats::net {

// Async transport for the public RuTracker.RU source-first parser.
// No result is emitted before its concrete topic page re-proves the exact
// info-hash and passes RuTrackerRuSource::isStrictComplete().
class RuTrackerRuSearchClient : public QObject {
    Q_OBJECT

public:
    explicit RuTrackerRuSearchClient(QObject* parent = nullptr);
    ~RuTrackerRuSearchClient() override;

    void search(const QString& query, int limit = 50,
        const QString& sortKey = QStringLiteral("seeders_desc"),
        const QString& contentType = QString());
    void cancel();

signals:
    void resultReady(const QString& query, const rats::domain::Torrent& torrent);
    void searchFinished(
        const QString& query, int accepted, int rejected, const QString& error);

private:
    struct DetailJob {
        domain::Torrent torrent;
        QUrl url;
    };

    void fetchSearchPage(int generation);
    void processQueue(int generation);
    void fetchDetail(DetailJob job, int generation);
    void finishIfIdle(int generation);
    void finishNow(int generation, const QString& error = QString());

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
    QString currentContentType_;

    static constexpr int kTimeoutMs = 15000;
    static constexpr int kMaxConcurrentDetails = 4;
};

} // namespace rats::net

#endif // RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H
