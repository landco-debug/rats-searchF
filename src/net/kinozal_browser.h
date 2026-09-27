#ifndef RATS_NET_KINOZAL_BROWSER_H
#define RATS_NET_KINOZAL_BROWSER_H

#ifdef __APPLE__

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>

namespace rats::net {

class KinozalBrowser {
public:
    using Completion = std::function<void(
        const QByteArray&, const QUrl&, const QString&)>;

    KinozalBrowser();
    ~KinozalBrowser();

    void get(const QUrl& url, Completion completion);
    // Run a same-origin fetch() inside the currently loaded page. Kinozal's
    // get_srv_details endpoint is normally called this way by its own UI, so
    // this preserves the exact browser session, Referer and XHR semantics.
    void fetchText(const QUrl& url, Completion completion);
    void authorize(const QUrl& loginUrl, Completion completion);
    void clearSession(std::function<void()> completion);
    void cancel();

private:
    void* bridge_ = nullptr;
};

} // namespace rats::net
#endif

#endif // RATS_NET_KINOZAL_BROWSER_H
