#include "net/kinozal_browser.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <QDebug>
#include <memory>
#include <utility>

namespace rats::net {
namespace {

enum class BrowserPurpose {
    Fetch,
    Authorize
};

struct Pending {
    QUrl target;
    KinozalBrowser::Completion callback;
    BrowserPurpose purpose = BrowserPurpose::Fetch;
    unsigned long serial = 0;
};

} // namespace
} // namespace rats::net

@interface RatsKinozalWebBridge : NSObject <WKNavigationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) WKWebView* web;
- (void)get:(const QUrl&)url
    completion:(rats::net::KinozalBrowser::Completion)completion;
- (void)fetchText:(const QUrl&)url
    completion:(rats::net::KinozalBrowser::Completion)completion;
- (void)authorize:(const QUrl&)url
    completion:(rats::net::KinozalBrowser::Completion)completion;
- (void)clearSession:(std::function<void()>)completion;
- (void)cancel;
@end

@implementation RatsKinozalWebBridge {
    std::unique_ptr<rats::net::Pending> _pending;
    unsigned long _serial;
}

- (instancetype)init {
    if ((self = [super init])) {
        NSRect frame = NSMakeRect(0, 0, 980, 720);
        _window = [[NSWindow alloc] initWithContentRect:frame
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered defer:NO];
        _window.releasedWhenClosed = NO;
        _window.title = @"Kinozal — авторизация";
        _window.delegate = self;

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        _web = [[WKWebView alloc] initWithFrame:frame configuration:config];
        _web.navigationDelegate = self;
        _window.contentView = _web;
        [_window center];
    }
    return self;
}

- (void)finishWithHtml:(NSString*)html
                   url:(NSURL*)url
                 error:(NSString*)error {
    if (!_pending)
        return;

    auto callback = std::move(_pending->callback);
    _pending.reset();
    if (!callback)
        return;

    const QByteArray body
        = html ? QByteArray([html UTF8String]) : QByteArray();
    const QUrl finalUrl = url
        ? QUrl(QString::fromUtf8(url.absoluteString.UTF8String))
        : QUrl();
    callback(body, finalUrl,
        error ? QString::fromUtf8(error.UTF8String) : QString());
}

- (void)cancel {
    ++_serial;
    _pending.reset();
    [_web stopLoading];
    [_window orderOut:nil];
}

- (void)start:(const QUrl&)url
      purpose:(rats::net::BrowserPurpose)purpose
   completion:(rats::net::KinozalBrowser::Completion)completion {
    [self cancel];

    _pending = std::make_unique<rats::net::Pending>();
    _pending->target = url;
    _pending->callback = std::move(completion);
    _pending->purpose = purpose;
    _pending->serial = _serial;

    NSURL* nsurl = [NSURL URLWithString:
        QString::fromLatin1(
            url.toEncoded(QUrl::FullyEncoded)).toNSString()];
    if (!nsurl) {
        [self finishWithHtml:nil url:nil error:@"Invalid Kinozal URL"];
        return;
    }

    if (purpose == rats::net::BrowserPurpose::Authorize) {
        _window.title = @"Kinozal — авторизация";
        [_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    [_web loadRequest:[NSURLRequest requestWithURL:nsurl]];

    const unsigned long serial = _serial;
    __weak RatsKinozalWebBridge* weakSelf = self;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC),
        dispatch_get_main_queue(), ^{
            RatsKinozalWebBridge* strongSelf = weakSelf;
            if (!strongSelf || !strongSelf->_pending
                || strongSelf->_serial != serial) {
                return;
            }
            NSString* message =
                strongSelf->_pending->purpose
                    == rats::net::BrowserPurpose::Authorize
                ? @"Kinozal browser authorization timed out (3 minutes)"
                : @"Kinozal browser request timed out (3 minutes)";
            [strongSelf finishWithHtml:nil
                                  url:strongSelf.web.URL
                                error:message];
        });
}

- (void)get:(const QUrl&)url
    completion:(rats::net::KinozalBrowser::Completion)completion {
    [self start:url purpose:rats::net::BrowserPurpose::Fetch
        completion:std::move(completion)];
}

- (void)fetchText:(const QUrl&)url
    completion:(rats::net::KinozalBrowser::Completion)completion {
    if (!completion)
        return;

    NSString* absolute = QString::fromLatin1(
        url.toEncoded(QUrl::FullyEncoded)).toNSString();
    if (!absolute || absolute.length == 0) {
        completion(QByteArray(), QUrl(),
            QStringLiteral("Invalid Kinozal fetch URL"));
        return;
    }

    const unsigned long serial = _serial;
    NSString* script =
        @"const response = await fetch(url, {"
         " credentials: 'same-origin',"
         " headers: {'X-Requested-With': 'XMLHttpRequest'}"
         "});"
         "const bytes = new Uint8Array(await response.arrayBuffer());"
         "let binary = '';"
         "for (let i = 0; i < bytes.length; i += 0x8000) {"
         " binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));"
         "}"
         "return {ok: response.ok, status: response.status,"
         " url: response.url, bodyBase64: btoa(binary)};";

    [_web callAsyncJavaScript:script
        arguments:@{@"url": absolute}
        inFrame:nil
        inContentWorld:[WKContentWorld pageWorld]
        completionHandler:^(id value, NSError* error) {
            if (self->_serial != serial)
                return;

            if (error || ![value isKindOfClass:[NSDictionary class]]) {
                completion(QByteArray(), QUrl(),
                    error
                        ? QString::fromUtf8(
                              error.localizedDescription.UTF8String)
                        : QStringLiteral(
                              "Kinozal in-page fetch returned no response"));
                return;
            }

            NSDictionary* response = value;
            const bool ok = [response[@"ok"] boolValue];
            const int status = [response[@"status"] intValue];
            NSString* bodyBase64 = response[@"bodyBase64"] ?: @"";
            NSString* finalUrl = response[@"url"] ?: absolute;
            const QUrl resultUrl(
                QString::fromUtf8(finalUrl.UTF8String));

            if (!ok) {
                completion(QByteArray(), resultUrl,
                    QStringLiteral("Kinozal AJAX request returned HTTP %1")
                        .arg(status));
                return;
            }

            NSData* body = [[NSData alloc]
                initWithBase64EncodedString:bodyBase64
                options:0];
            if (!body && bodyBase64.length > 0) {
                completion(QByteArray(), resultUrl,
                    QStringLiteral("Kinozal AJAX response could not be decoded"));
                return;
            }

            const QByteArray bytes(
                body.length > 0
                    ? reinterpret_cast<const char*>(body.bytes)
                    : "",
                static_cast<qsizetype>(body.length));
            completion(bytes, resultUrl, QString());
        }];
}

- (void)authorize:(const QUrl&)url
    completion:(rats::net::KinozalBrowser::Completion)completion {
    [self start:url purpose:rats::net::BrowserPurpose::Authorize
        completion:std::move(completion)];
}

- (void)clearSession:(std::function<void()>)completion {
    [self cancel];

    auto callback
        = std::make_shared<std::function<void()>>(std::move(completion));
    WKWebsiteDataStore* store = _web.configuration.websiteDataStore;
    NSSet<NSString*>* types = [WKWebsiteDataStore allWebsiteDataTypes];

    [store fetchDataRecordsOfTypes:types
        completionHandler:^(NSArray<WKWebsiteDataRecord*>* records) {
            NSMutableArray<WKWebsiteDataRecord*>* matching
                = [NSMutableArray array];
            for (WKWebsiteDataRecord* record in records) {
                NSString* name = record.displayName.lowercaseString;
                if ([name containsString:@"kinozal"])
                    [matching addObject:record];
            }

            if (matching.count == 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (*callback)
                        (*callback)();
                });
                return;
            }

            [store removeDataOfTypes:types
                forDataRecords:matching
                completionHandler:^{
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (*callback)
                            (*callback)();
                    });
                }];
        }];
}

- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation {
    (void)navigation;
    if (!_pending)
        return;

    const unsigned long serial = _serial;
    [webView evaluateJavaScript:@"(() => {"
        "const html = document.documentElement.outerHTML;"
        "const text = document.body ? document.body.innerText : '';"
        "return {"
        " html,"
        " path: location.pathname,"
        " title: document.title || '',"
        " loggedIn: text.includes('Выход') || !!document.querySelector('a[href*=\"logout.php\"]'),"
        " details: document.querySelectorAll('a[href*=\"details.php?id=\"]').length,"
        " emptyResults: /Найдено\\s*0\\s+раздач/i.test(text),"
        " loginForm: !!document.querySelector('input[name=\"password\"], input[name=\"login_password\"]'),"
        " cf: html.includes('cf_chl_opt') || html.includes('challenge-platform') ||"
        "     html.includes('orchestrate/chl_page') || (document.title || '').includes('Just a moment')"
        "};"
        "})()"
        completionHandler:^(id value, NSError* error) {
            if (!self->_pending || self->_serial != serial)
                return;
            if (error || ![value isKindOfClass:[NSDictionary class]]) {
                [self.window orderOut:nil];
                [self finishWithHtml:nil url:webView.URL
                    error:error.localizedDescription
                        ?: @"Cannot inspect Kinozal browser page"];
                return;
            }

            NSDictionary* snapshot = value;
            NSString* html = snapshot[@"html"] ?: @"";
            NSString* path = webView.URL.path ?: @"";
            NSString* host = webView.URL.host.lowercaseString ?: @"";
            NSString* title = snapshot[@"title"] ?: @"";
            const bool officialHost =
                [host isEqualToString:@"kinozal.me"]
                || [host isEqualToString:@"www.kinozal.me"]
                || [host isEqualToString:@"kinozal.guru"]
                || [host isEqualToString:@"www.kinozal.guru"];
            const bool loggedIn = [snapshot[@"loggedIn"] boolValue];
            const int details = [snapshot[@"details"] intValue];
            const bool emptyResults = [snapshot[@"emptyResults"] boolValue];
            const bool interactive =
                [snapshot[@"loginForm"] boolValue]
                || [snapshot[@"cf"] boolValue];

            if (self->_pending->purpose
                == rats::net::BrowserPurpose::Authorize) {
                if (loggedIn && officialHost) {
                    qInfo() << "[KinozalBrowser] browser authorization confirmed"
                            << QString::fromNSString(host);
                    [self.window orderOut:nil];
                    [self finishWithHtml:html url:webView.URL error:nil];
                    return;
                }

                [self.window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                return;
            }

            const QString expectedPath
                = self->_pending->target.path();
            const bool samePath
                = [path isEqualToString:expectedPath.toNSString()];

            bool ready = false;
            if (officialHost && samePath && !interactive) {
                const bool branded
                    = [title rangeOfString:@"Кинозал."
                                   options:NSCaseInsensitiveSearch]
                          .location != NSNotFound;
                if (expectedPath == QStringLiteral("/browse.php")) {
                    // A branded guest/error shell is not a successful empty
                    // search. Prove a real listing/session (or an explicit zero).
                    ready = branded
                        && (loggedIn || details > 0 || emptyResults);
                } else if (expectedPath
                    == QStringLiteral("/details.php")) {
                    ready = branded;
                }
            }

            if (ready) {
                [self.window orderOut:nil];
                [self finishWithHtml:html url:webView.URL error:nil];
                return;
            }

            if (loggedIn && officialHost && !samePath) {
                NSURL* destination = [NSURL URLWithString:
                    QString::fromLatin1(
                        self->_pending->target.toEncoded(
                            QUrl::FullyEncoded)).toNSString()];
                [self.window orderOut:nil];
                [webView loadRequest:
                    [NSURLRequest requestWithURL:destination]];
                return;
            }

            if (interactive) {
                [self.window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                return;
            }

            [self.window orderOut:nil];
            NSString* problem = [NSString stringWithFormat:
                @"Kinozal returned an unexpected browser page (%@)",
                webView.URL.absoluteString ?: @"unknown URL"];
            [self finishWithHtml:nil url:webView.URL error:problem];
        }];
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
    withError:(NSError*)error {
    (void)navigation;
    if (_pending) {
        [self finishWithHtml:nil url:webView.URL
            error:error.localizedDescription];
    }
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
    withError:(NSError*)error {
    (void)navigation;
    if (_pending && error.code != NSURLErrorCancelled) {
        [self finishWithHtml:nil url:webView.URL
            error:error.localizedDescription];
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    [sender orderOut:nil];
    if (_pending) {
        [self finishWithHtml:nil url:_web.URL
            error:@"Kinozal browser authorization was cancelled"];
    }
    return NO;
}
@end

namespace rats::net {

KinozalBrowser::KinozalBrowser()
    : bridge_((__bridge_retained void*)[
          [RatsKinozalWebBridge alloc] init])
{
}

KinozalBrowser::~KinozalBrowser()
{
    auto* bridge
        = (__bridge_transfer RatsKinozalWebBridge*)bridge_;
    [bridge cancel];
    bridge.web.navigationDelegate = nil;
    bridge.window.delegate = nil;
    [bridge.window close];
}

void KinozalBrowser::get(const QUrl& url, Completion completion)
{
    [(__bridge RatsKinozalWebBridge*)bridge_
        get:url completion:std::move(completion)];
}

void KinozalBrowser::fetchText(
    const QUrl& url, Completion completion)
{
    [(__bridge RatsKinozalWebBridge*)bridge_
        fetchText:url completion:std::move(completion)];
}

void KinozalBrowser::authorize(
    const QUrl& loginUrl, Completion completion)
{
    [(__bridge RatsKinozalWebBridge*)bridge_
        authorize:loginUrl completion:std::move(completion)];
}

void KinozalBrowser::clearSession(std::function<void()> completion)
{
    [(__bridge RatsKinozalWebBridge*)bridge_
        clearSession:std::move(completion)];
}

void KinozalBrowser::cancel()
{
    [(__bridge RatsKinozalWebBridge*)bridge_ cancel];
}

} // namespace rats::net
#endif
