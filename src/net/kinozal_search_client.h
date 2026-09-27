#ifndef RATS_NET_KINOZAL_SEARCH_CLIENT_H
#define RATS_NET_KINOZAL_SEARCH_CLIENT_H

#include "domain/torrent.h"

#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>

#ifdef __APPLE__
#include "net/kinozal_browser.h"
#endif

namespace rats::net {

class KinozalSearchClient : public QObject {
    Q_OBJECT

public:
    explicit KinozalSearchClient(QObject* parent = nullptr);
    ~KinozalSearchClient() override;

    bool isConfigured() const;

#ifdef __APPLE__
    void reloginInBrowser();
#endif

    void search(const QString& query, int limit = 50,
        const QString& sortKey = QStringLiteral("seeders_desc"),
        const QString& contentType = QString());
    void cancel();

signals:
    void resultReady(
        const QString& query, const rats::domain::Torrent& torrent);
    void searchFinished(
        const QString& query, int accepted, int rejected,
        const QString& error);
#ifdef __APPLE__
    void browserAuthorizationChanged(
        bool authorized, const QString& message);
#endif

private:
    struct Job {
        domain::Torrent torrent;
        QUrl detailUrl;
    };

    void fetchSearchPage(int generation);
    void processNext(int generation);
    void fetchDetail(Job job, int generation);
    void fetchServerDetails(Job job, int generation);
    bool tryNextMirror(int generation, const QString& reason);
    void finishIfIdle(int generation);
    void finishNow(int generation, const QString& error = QString());

#ifdef __APPLE__
    void startAuthorizationAtMirror(int mirror, int generation);
    std::unique_ptr<KinozalBrowser> browser_;
#endif

    QQueue<Job> queue_;
    int generation_ = 0;
    int requestedLimit_ = 50;
    int accepted_ = 0;
    int rejected_ = 0;
    int active_ = 0;
    int requestFailures_ = 0;
    int mirrorIndex_ = 0;
    int preferredMirrorIndex_ = 0;
    bool searchResolved_ = false;
    bool finishedEmitted_ = true;
    QString currentQuery_;
    QString currentSortKey_;
    QString currentContentType_;
    QString lastError_;
    QStringList mirrorBaseUrls_ {
        QStringLiteral("https://kinozal.me"),
        QStringLiteral("https://kinozal.guru")
    };
};

} // namespace rats::net

#endif // RATS_NET_KINOZAL_SEARCH_CLIENT_H
