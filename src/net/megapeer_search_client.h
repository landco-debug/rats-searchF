#ifndef RATS_NET_MEGAPEER_SEARCH_CLIENT_H
#define RATS_NET_MEGAPEER_SEARCH_CLIENT_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace rats::net {

class MegaPeerSearchClient : public QObject {
    Q_OBJECT
public:
    explicit MegaPeerSearchClient(QObject* parent = nullptr);
    ~MegaPeerSearchClient() override;

    void search(const QString& query, int limit = 50,
        const QString& sortKey = QStringLiteral("seeders_desc"),
        const QString& contentType = QString());
    void cancel();

signals:
    void resultReady(const QString& query, const rats::domain::Torrent& torrent);
    void searchFinished(
        const QString& query, int accepted, int rejected, const QString& error);

private:
    struct Job {
        domain::Torrent torrent;
        QUrl detailUrl;
        QUrl torrentUrl;
        QByteArray detailBody;
        QUrl detailFinalUrl;
    };

    void fetchSearchPage(int generation);
    void processQueue(int generation);
    void fetchDetail(Job job, int generation);
    void fetchTorrentFallback(Job job, int generation);
    void finishCandidate(Job job, int generation, bool detailApplied);
    void finishIfIdle(int generation);
    void finishNow(int generation, const QString& error = QString());
    void recordNetworkFailure(const QString& context, const QString& error);

    QNetworkAccessManager* networkManager_ = nullptr;
    QSet<QNetworkReply*> replies_;
    QQueue<Job> queue_;

    int generation_ = 0;
    int active_ = 0;
    int requestedLimit_ = 50;
    int accepted_ = 0;
    int rejected_ = 0;
    int networkFailures_ = 0;
    bool searchResolved_ = false;
    bool finishedEmitted_ = true;
    QString currentQuery_;
    QString currentSortKey_;
    QString currentContentType_;
    QString lastNetworkError_;

    static constexpr int kTimeoutMs = 15000;
    // Interactive search should not fan out four detail/.torrent requests per
    // row against a public tracker. Two detail requests at a time are enough to
    // keep results streaming without recreating crawler-like request pressure.
    static constexpr int kMaxConcurrent = 2;
};

} // namespace rats::net

#endif // RATS_NET_MEGAPEER_SEARCH_CLIENT_H
