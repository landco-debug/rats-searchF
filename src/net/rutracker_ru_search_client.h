#ifndef RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H
#define RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H

#include "domain/torrent.h"

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace rats::net {

class CloudflareClearance;

// Authenticated asynchronous RuTracker exact-source client.
//
// Current RuTracker access is mirror- and Cloudflare-sensitive. The client tries
// official mirrors using the real login endpoint, keeps the successful mirror
// for search/topic requests, and on macOS can bootstrap a managed Cloudflare
// challenge through system WebKit before retrying the normal Qt network path.
class RuTrackerRuSearchClient : public QObject {
    Q_OBJECT

public:
    explicit RuTrackerRuSearchClient(QObject* parent = nullptr);
    ~RuTrackerRuSearchClient() override;

    void setCredentials(const QString& username, const QString& password);
    bool isConfigured() const;

    void search(const QString& query, int limit = 50,
        const QString& sortKey = QStringLiteral("seeders_desc"),
        const QString& contentType = QString());
    void cancel();

signals:
    void resultReady(const QString& query, const rats::domain::Torrent& torrent);
    void searchFinished(
        const QString& query, int accepted, int rejected, const QString& error);

private:
    enum class ClearancePurpose {
        None,
        Login,
        Search
    };

    struct DetailJob {
        domain::Torrent torrent;
        QUrl url;
    };

    void authenticate(int generation);
    void authenticateCurrentMirror(int generation);
    void tryNextMirror(int generation, const QString& reason);
    void requestCloudflareClearance(
        const QUrl& url, int generation, ClearancePurpose purpose);
    void resetCookieJar();
    void fetchSearchPage(int generation);
    void processQueue(int generation);
    void fetchDetail(DetailJob job, int generation);
    void finishIfIdle(int generation);
    void finishNow(int generation, const QString& error = QString());

    QNetworkAccessManager* networkManager_ = nullptr;
    CloudflareClearance* clearance_ = nullptr;
    QSet<QNetworkReply*> replies_;
    QQueue<DetailJob> detailQueue_;

    int generation_ = 0;
    int clearanceGeneration_ = -1;
    int mirrorIndex_ = 0;
    int activeDetails_ = 0;
    int requestedLimit_ = 50;
    int accepted_ = 0;
    int rejected_ = 0;
    bool authenticated_ = false;
    bool authRetried_ = false;
    bool clearanceRetriedForMirror_ = false;
    bool searchClearanceRetried_ = false;
    bool searchPageResolved_ = false;
    bool finishedEmitted_ = true;
    ClearancePurpose clearancePurpose_ = ClearancePurpose::None;
    QString username_;
    QString password_;
    QString currentQuery_;
    QString currentSortKey_;
    QString currentContentType_;
    QString userAgent_;
    QStringList mirrorErrors_;
    QUrl currentBaseUrl_;

    static constexpr int kTimeoutMs = 20000;
    static constexpr int kMaxConcurrentDetails = 2;
};

} // namespace rats::net

#endif // RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H
