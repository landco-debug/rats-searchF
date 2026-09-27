#ifndef RATS_NET_CLOUDFLARE_CLEARANCE_H
#define RATS_NET_CLOUDFLARE_CLEARANCE_H

#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QUrl>

#include <memory>

namespace rats::net {

// Small platform adapter used only after a normal tracker request has actually
// returned a Cloudflare challenge. On macOS it uses the system WebKit engine to
// let the managed challenge complete, then hands the resulting cookies + exact
// WebKit User-Agent back to QNetworkAccessManager. No browser process is kept
// alive after the clearance hand-off.
class CloudflareClearance : public QObject {
    Q_OBJECT

public:
    explicit CloudflareClearance(QObject* parent = nullptr);
    ~CloudflareClearance() override;

    bool isSupported() const;
    bool isBusy() const;

    void solve(const QUrl& url, int timeoutMs = 65000);
    void cancel();

signals:
    void solved(const QUrl& url, const QString& userAgent,
        const QList<QNetworkCookie>& cookies);
    void failed(const QUrl& url, const QString& error);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Detect both error-status and HTTP-200 managed challenge pages. Kept in the
// cross-platform .cpp so unit tests never need to launch WebKit.
bool looksLikeCloudflareChallenge(int httpStatus, const QByteArray& body);

} // namespace rats::net

#endif // RATS_NET_CLOUDFLARE_CLEARANCE_H
