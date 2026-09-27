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

class CloudflareClearance;

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
    void requestCloudflareClearance(const QUrl& url, int generation);

    QNetworkAccessManager* networkManager_ = nullptr;
    CloudflareClearance* clearance_ = nullptr;
    QSet<QNetworkReply*> replies_;
    QQueue<Job> queue_;

    int generation_ = 0;
    int clearanceGeneration_ = -1;
    int active_ = 0;
    int requestedLimit_ = 50;
    int accepted_ = 0;
    int rejected_ = 0;
    int networkFailures_ = 0;
    bool searchResolved_ = false;
    bool finishedEmitted_ = true;
    bool clearanceRetried_ = false;
    QString currentQuery_;
    QString currentSortKey_;
    QString currentContentType_;
    QString lastNetworkError_;
    QString userAgent_;

    static constexpr int kTimeoutMs = 15000;
    static constexpr int kMaxConcurrent = 2;
};

} // namespace rats::net

#endif // RATS_NET_MEGAPEER_SEARCH_CLIENT_H
