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

// Authenticated asynchronous RuTracker exact-source client.
//
// RuTracker search and topic metadata are account-gated. The client logs in on
// an official mirror, keeps that mirror's cookies in its QNetworkAccessManager,
// then verifies every result on its concrete topic page before emitting it.
// Official mirrors are tried automatically because reachability differs by ISP.
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
    struct DetailJob {
        domain::Torrent torrent;
        QUrl url;
    };

    void authenticate(int generation);
    void resetCookieJar();
    void resetMirrorCycle();
    bool tryNextMirror(int generation, const QString& reason);
    QUrl urlOnActiveMirror(const QString& path) const;
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
    bool authenticated_ = false;
    bool authRetried_ = false;
    bool searchPageResolved_ = false;
    bool finishedEmitted_ = true;
    QString username_;
    QString password_;
    QString currentQuery_;
    QString currentSortKey_;
    QString currentContentType_;
    QStringList mirrorBaseUrls_ {
        QStringLiteral("https://rutracker.net"),
        QStringLiteral("https://rutracker.org")
    };
    int mirrorIndex_ = 0;
    QString activeMirrorBaseUrl_ = QStringLiteral("https://rutracker.net");
    QString lastMirrorError_;

    static constexpr int kTimeoutMs = 20000;
    static constexpr int kMaxConcurrentDetails = 2;
};

} // namespace rats::net

#endif // RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H
