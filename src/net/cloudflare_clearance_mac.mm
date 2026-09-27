#include "net/cloudflare_clearance.h"

#ifdef Q_OS_MACOS

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

namespace rats::net {
namespace {

QString toQString(NSString* value)
{
    if (!value)
        return {};
    const char* utf8 = [value UTF8String];
    return utf8 ? QString::fromUtf8(utf8) : QString();
}

@interface RatsWebNavigationDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic, copy) void (^onFinish)(void);
@property(nonatomic, copy) void (^onFail)(NSString*);
@end

@implementation RatsWebNavigationDelegate

- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation
{
    Q_UNUSED(webView);
    Q_UNUSED(navigation);
    if (self.onFinish)
        self.onFinish();
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
    withError:(NSError*)error
{
    Q_UNUSED(webView);
    Q_UNUSED(navigation);
    if (self.onFail)
        self.onFail(error.localizedDescription);
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
    withError:(NSError*)error
{
    Q_UNUSED(webView);
    Q_UNUSED(navigation);
    if (self.onFail)
        self.onFail(error.localizedDescription);
}

@end

} // namespace

struct CloudflareClearance::Impl {
    CloudflareClearance* q = nullptr;
    WKWebView* webView = nil;
    NSWindow* window = nil;
    RatsWebNavigationDelegate* delegate = nil;
    QUrl requestedUrl;
    QElapsedTimer elapsed;
    int timeoutMs = 65000;
    bool busy = false;
    bool collectingCookies = false;

    explicit Impl(CloudflareClearance* owner)
        : q(owner)
    {
    }

    void cleanup()
    {
        collectingCookies = false;
        busy = false;

        if (webView) {
            [webView stopLoading];
            [webView setNavigationDelegate:nil];
            [webView removeFromSuperview];
        }
        if (window)
            [window orderOut:nil];

        delegate.onFinish = nil;
        delegate.onFail = nil;
        delegate = nil;
        webView = nil;
        window = nil;
    }

    void fail(const QString& error)
    {
        if (!busy)
            return;
        const QUrl url = requestedUrl;
        cleanup();
        emit q->failed(url, error);
    }

    void scheduleInspect(int delayMs = 350)
    {
        if (!busy)
            return;
        QTimer::singleShot(delayMs, q, [this]() { inspectPage(); });
    }

    void inspectPage()
    {
        if (!busy || !webView || collectingCookies)
            return;
        if (elapsed.elapsed() >= timeoutMs) {
            fail(q->tr("Cloudflare browser clearance timed out."));
            return;
        }

        NSString* script = @"(() => ({"
                           "title: document.title || '',"
                           "html: document.documentElement ? document.documentElement.outerHTML : '',"
                           "ua: navigator.userAgent || '',"
                           "href: location.href || ''"
                           "}))()";

        [webView evaluateJavaScript:script
                 completionHandler:^(id result, NSError* error) {
            if (!busy)
                return;
            if (error || ![result isKindOfClass:[NSDictionary class]]) {
                scheduleInspect(500);
                return;
            }

            NSDictionary* values = (NSDictionary*)result;
            const QString title = toQString(values[@"title"]);
            const QString html = toQString(values[@"html"]);
            const QString ua = toQString(values[@"ua"]);
            const QUrl finalUrl(toQString(values[@"href"]));

            const bool challenge
                = looksLikeCloudflareChallenge(200, html.toUtf8())
                || title.contains(QStringLiteral("Just a moment"),
                    Qt::CaseInsensitive)
                || title.contains(QStringLiteral("Attention Required"),
                    Qt::CaseInsensitive);

            if (challenge || html.size() < 200) {
                scheduleInspect(700);
                return;
            }

            if (finalUrl.isValid() && !requestedUrl.host().isEmpty()
                && finalUrl.host().compare(
                       requestedUrl.host(), Qt::CaseInsensitive)
                    != 0) {
                // Managed challenge redirects should end on the requested host.
                // Give WebKit a little more time before treating a foreign
                // redirect as a failed clearance.
                scheduleInspect(700);
                return;
            }

            collectingCookies = true;
            WKHTTPCookieStore* cookieStore
                = webView.configuration.websiteDataStore.httpCookieStore;
            [cookieStore getAllCookies:^(NSArray<NSHTTPCookie*>* cookies) {
                if (!busy)
                    return;

                QList<QNetworkCookie> qtCookies;
                const QString requestedHost = requestedUrl.host().toLower();
                for (NSHTTPCookie* cookie in cookies) {
                    QString domain = toQString(cookie.domain).toLower();
                    QString compareDomain = domain;
                    if (compareDomain.startsWith(QLatin1Char('.')))
                        compareDomain.remove(0, 1);

                    if (requestedHost != compareDomain
                        && !requestedHost.endsWith(
                            QStringLiteral(".") + compareDomain)) {
                        continue;
                    }

                    QNetworkCookie qtCookie(
                        toQString(cookie.name).toUtf8(),
                        toQString(cookie.value).toUtf8());
                    qtCookie.setDomain(domain);
                    qtCookie.setPath(toQString(cookie.path));
                    qtCookie.setSecure(cookie.secure);
                    qtCookie.setHttpOnly(cookie.HTTPOnly);
                    if (cookie.expiresDate) {
                        const qint64 msecs = static_cast<qint64>(
                            [cookie.expiresDate timeIntervalSince1970] * 1000.0);
                        qtCookie.setExpirationDate(
                            QDateTime::fromMSecsSinceEpoch(msecs, Qt::UTC));
                    }
                    qtCookies.append(qtCookie);
                }

                const QUrl url = requestedUrl;
                const QString finalUa = ua;
                cleanup();
                emit q->solved(url, finalUa, qtCookies);
            }];
        }];
    }

    void start(const QUrl& url, int requestedTimeoutMs)
    {
        cleanup();

        requestedUrl = url;
        timeoutMs = qBound(15000, requestedTimeoutMs, 120000);
        busy = true;
        elapsed.restart();

        WKWebViewConfiguration* configuration
            = [[WKWebViewConfiguration alloc] init];
        configuration.websiteDataStore = [WKWebsiteDataStore defaultDataStore];

        webView = [[WKWebView alloc]
            initWithFrame:NSMakeRect(0, 0, 1280, 800)
            configuration:configuration];

        delegate = [[RatsWebNavigationDelegate alloc] init];
        Impl* self = this;
        delegate.onFinish = ^{
            self->scheduleInspect(250);
        };
        delegate.onFail = ^(NSString* message) {
            self->fail(
                q->tr("System WebKit could not open the protected tracker: %1")
                    .arg(toQString(message)));
        };
        webView.navigationDelegate = delegate;

        // Keep a real, rendered WebKit view alive off-screen. This avoids the
        // reduced/timer-throttled behavior of an unattached view while never
        // flashing a browser window in front of the user.
        window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(-10000, -10000, 1280, 800)
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        window.contentView = webView;
        window.alphaValue = 0.01;
        window.opaque = NO;
        [window orderFrontRegardless];

        NSURL* nsUrl = [NSURL URLWithString:
            [NSString stringWithUTF8String:url.toEncoded().constData()]];
        if (!nsUrl) {
            fail(q->tr("Invalid protected tracker URL."));
            return;
        }

        NSMutableURLRequest* request = [NSMutableURLRequest
            requestWithURL:nsUrl
               cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
           timeoutInterval:static_cast<NSTimeInterval>(timeoutMs) / 1000.0];
        [webView loadRequest:request];

        // Some challenge transitions do not trigger a second didFinish callback.
        scheduleInspect(1000);
    }
};

CloudflareClearance::CloudflareClearance(QObject* parent)
    : QObject(parent)
    , d_(std::make_unique<Impl>(this))
{
}

CloudflareClearance::~CloudflareClearance()
{
    d_->cleanup();
}

bool CloudflareClearance::isSupported() const
{
    return true;
}

bool CloudflareClearance::isBusy() const
{
    return d_->busy;
}

void CloudflareClearance::solve(const QUrl& url, int timeoutMs)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
        emit failed(url, tr("Protected tracker URL is invalid."));
        return;
    }
    d_->start(url, timeoutMs);
}

void CloudflareClearance::cancel()
{
    d_->cleanup();
}

} // namespace rats::net

#endif // Q_OS_MACOS
