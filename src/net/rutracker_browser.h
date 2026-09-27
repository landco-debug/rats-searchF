#ifndef RATS_NET_RUTRACKER_BROWSER_H
#define RATS_NET_RUTRACKER_BROWSER_H

#ifdef __APPLE__
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>

namespace rats::net {

// One persistent WebKit transport owns the complete RuTracker browser session on
// macOS: login, Cloudflare clearance, search and exact topic pages all use the
// same WKWebsiteDataStore and network fingerprint.
class RuTrackerBrowser {
public:
    using Completion = std::function<void(const QByteArray&, const QUrl&, const QString&)>;

    RuTrackerBrowser();
    ~RuTrackerBrowser();

    // Fetch a search/topic URL through the persistent authenticated browser.
    void get(const QUrl& url, Completion completion);

    // Open the embedded login page. Completion succeeds once RuTracker exposes
    // its authenticated DOM marker in this same persistent WebKit session.
    void authorize(const QUrl& loginUrl, Completion completion);

    // Remove only RuTracker website data from the app's persistent WebKit store.
    // Used by the explicit "Authorize / Re-login" UI action.
    void clearSession(std::function<void()> completion);

    void cancel();

private:
    void* bridge_ = nullptr;
};

} // namespace rats::net
#endif

#endif
