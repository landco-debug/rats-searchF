#ifndef RATS_NET_RUTRACKER_BROWSER_H
#define RATS_NET_RUTRACKER_BROWSER_H

#ifdef __APPLE__
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>

namespace rats::net {
// Uses one WebKit data store and one WebKit transport for both login and pages.
// No browser cookie is replayed through Qt's unrelated HTTP stack.
class RuTrackerBrowser {
public:
    using Completion = std::function<void(const QByteArray&, const QUrl&, const QString&)>;
    RuTrackerBrowser();
    ~RuTrackerBrowser();
    void get(const QUrl& url, Completion completion);
    void cancel();
private:
    void* bridge_ = nullptr;
};
}
#endif
#endif
