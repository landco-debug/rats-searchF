#include "net/cloudflare_clearance.h"

#include <QtGlobal>

namespace rats::net {

bool looksLikeCloudflareChallenge(int httpStatus, const QByteArray& body)
{
    const QByteArray lower = body.toLower();
    const bool marker
        = lower.contains("just a moment")
        || lower.contains("challenge-platform")
        || lower.contains("cf-chl-")
        || lower.contains("/cdn-cgi/challenge-platform/")
        || lower.contains("attention required")
        || (lower.contains("cloudflare") && lower.contains("challenge"));

    if (marker)
        return true;

    return (httpStatus == 403 || httpStatus == 429 || httpStatus == 503)
        && lower.contains("cloudflare");
}

#ifndef Q_OS_MACOS

struct CloudflareClearance::Impl {
    bool busy = false;
};

CloudflareClearance::CloudflareClearance(QObject* parent)
    : QObject(parent)
    , d_(std::make_unique<Impl>())
{
}

CloudflareClearance::~CloudflareClearance() = default;

bool CloudflareClearance::isSupported() const
{
    return false;
}

bool CloudflareClearance::isBusy() const
{
    return d_->busy;
}

void CloudflareClearance::solve(const QUrl& url, int)
{
    d_->busy = false;
    emit failed(url, tr("System WebKit clearance is available only on macOS."));
}

void CloudflareClearance::cancel()
{
    d_->busy = false;
}

#endif

} // namespace rats::net
