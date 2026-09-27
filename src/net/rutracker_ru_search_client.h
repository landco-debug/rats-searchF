#ifndef RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H
#define RATS_NET_RUTRACKER_RU_SEARCH_CLIENT_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QObject>
#ifdef __APPLE__
#include <memory>
#include "net/rutracker_browser.h"
#endif
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
// On macOS one persistent WebKit session owns login, Cloudflare clearance,
// search and exact topic pages. Username/password are not replayed through the
// unrelated Qt HTTP stack. Other platforms retain the HTTP credential path.
// Every emitted result is still verified on its concrete topic page.
class RuTrackerRuSearchClient : public QObject {
    Q_OBJECT

public:
    explicit RuTrackerRuSearchClient(QObject* parent = nullptr);
    ~RuTrackerRuSearchClient() override;

    void setCredentials(const QString& username, const QString& password);
    bool isConfigured() const;

#ifdef __APPLE__
    // Opens the persistent embedded browser after clearing only RuTracker site
    // data. This is the deterministic "Authorize / Re-login" action.
    void reloginInBrowser();
#endif

    void search(const QString& query, int limit = 50,
        const QString& sortKey = QStringLiteral("seeders_desc"),
        const QString& contentType = QString());
    void cancel();

signals:
    void resultReady(const QString& query, const rats::domain::Torrent& torrent);
    void searchFinished(
        const QString& query, int accepted, int rejected, const QString& error);
#ifdef __APPLE__
    void browserAuthorizationChanged(bool authorized, const QString& message);
#endif

private:
    struct DetailJob {
        domain::Torrent torrent;
        QUrl url;
    };

    void authenticate(int generation);
    void resetCookieJar(bool restorePersisted = true);
    bool restorePersistedSession();
    void persistActiveSession();
    void clearPersistedSessionForActiveMirror();
    void clearAllPersistedSessions();
    QString sessionSettingsGroup(const QString& host) const;

    void resetMirrorCycle();
    bool tryNextMirror(int generation, const QString& reason);
    QUrl urlOnActiveMirror(const QString& path) const;
    void fetchSearchPage(int generation);
    void handleSearchPage(int generation, const QByteArray& body, const QUrl& finalUrl);
    void processQueue(int generation);
    void handleDetailPage(DetailJob job, int generation,
        const QByteArray& body, const QUrl& finalUrl);
    void fetchDetail(DetailJob job, int generation);
    void finishIfIdle(int generation);
    void finishNow(int generation, const QString& error = QString());

    QNetworkAccessManager* networkManager_ = nullptr;
#ifdef __APPLE__
    std::unique_ptr<RuTrackerBrowser> browser_;
    bool browserMode_ = false;
#endif
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
