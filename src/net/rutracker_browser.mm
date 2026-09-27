#include "net/rutracker_browser.h"

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
    RuTrackerBrowser::Completion callback;
    BrowserPurpose purpose = BrowserPurpose::Fetch;
    unsigned long serial = 0;
};
}
}

@interface RatsRuTrackerWebBridge : NSObject <WKNavigationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) WKWebView* web;
- (void)get:(const QUrl&)url completion:(rats::net::RuTrackerBrowser::Completion)completion;
- (void)authorize:(const QUrl&)url completion:(rats::net::RuTrackerBrowser::Completion)completion;
- (void)clearSession:(std::function<void()>)completion;
- (void)cancel;
@end

@implementation RatsRuTrackerWebBridge {
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
        _window.title = @"RuTracker — авторизация";
        _window.delegate = self;

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        // This is deliberately persistent. Replacing Rats Search.app must not
        // destroy a successful RuTracker/Cloudflare browser session.
        config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        _web = [[WKWebView alloc] initWithFrame:frame configuration:config];
        _web.navigationDelegate = self;
        _window.contentView = _web;
        [_window center];
    }
    return self;
}

- (void)finishWithHtml:(NSString*)html url:(NSURL*)url error:(NSString*)error {
    if (!_pending) return;
    auto callback = std::move(_pending->callback);
    _pending.reset();
    if (!callback) return;

    QByteArray body = html ? QByteArray([html UTF8String]) : QByteArray();
    QUrl finalUrl = url ? QUrl(QString::fromUtf8(url.absoluteString.UTF8String)) : QUrl();
    callback(body, finalUrl, error ? QString::fromUtf8(error.UTF8String) : QString());
}

- (void)cancel {
    ++_serial;
    _pending.reset();
    [_web stopLoading];
    [_window orderOut:nil];
}

- (void)start:(const QUrl&)url
      purpose:(rats::net::BrowserPurpose)purpose
   completion:(rats::net::RuTrackerBrowser::Completion)completion {
    [self cancel];

    _pending = std::make_unique<rats::net::Pending>();
    _pending->target = url;
    _pending->callback = std::move(completion);
    _pending->purpose = purpose;
    _pending->serial = _serial;

    NSURL* nsurl = [NSURL URLWithString:
        QString::fromLatin1(url.toEncoded(QUrl::FullyEncoded)).toNSString()];
    if (!nsurl) {
        [self finishWithHtml:nil url:nil error:@"Invalid RuTracker URL"];
        return;
    }

    if (purpose == rats::net::BrowserPurpose::Authorize) {
        _window.title = @"RuTracker — авторизация";
        [_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    [_web loadRequest:[NSURLRequest requestWithURL:nsurl]];

    unsigned long serial = _serial;
    __weak RatsRuTrackerWebBridge* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        RatsRuTrackerWebBridge* strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_pending || strongSelf->_serial != serial)
            return;
        NSString* message = strongSelf->_pending->purpose == rats::net::BrowserPurpose::Authorize
            ? @"RuTracker browser authorization timed out (3 minutes)"
            : @"RuTracker browser request timed out (3 minutes)";
        [strongSelf finishWithHtml:nil url:strongSelf.web.URL error:message];
    });
}

- (void)get:(const QUrl&)url completion:(rats::net::RuTrackerBrowser::Completion)completion {
    [self start:url purpose:rats::net::BrowserPurpose::Fetch completion:std::move(completion)];
}

- (void)authorize:(const QUrl&)url completion:(rats::net::RuTrackerBrowser::Completion)completion {
    [self start:url purpose:rats::net::BrowserPurpose::Authorize completion:std::move(completion)];
}

- (void)clearSession:(std::function<void()>)completion {
    [self cancel];

    auto callback = std::make_shared<std::function<void()>>(std::move(completion));
    WKWebsiteDataStore* store = _web.configuration.websiteDataStore;
    NSSet<NSString*>* types = [WKWebsiteDataStore allWebsiteDataTypes];

    [store fetchDataRecordsOfTypes:types
        completionHandler:^(NSArray<WKWebsiteDataRecord*>* records) {
            NSMutableArray<WKWebsiteDataRecord*>* matching = [NSMutableArray array];
            for (WKWebsiteDataRecord* record in records) {
                NSString* name = record.displayName.lowercaseString;
                if ([name containsString:@"rutracker"])
                    [matching addObject:record];
            }

            if (matching.count == 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (*callback) (*callback)();
                });
                return;
            }

            [store removeDataOfTypes:types forDataRecords:matching
                completionHandler:^{
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (*callback) (*callback)();
                    });
                }];
        }];
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (!_pending) return;
    const unsigned long serial = _serial;

    [webView evaluateJavaScript:@"(() => {"
        "const table = document.querySelector('table#tor-tbl');"
        "const links = table ? Array.from(table.querySelectorAll('a.tLink')) : [];"
        "return {html: document.documentElement.outerHTML, table: !!table,"
        " rows: table ? table.querySelectorAll('tbody > tr').length : 0,"
        " links: links.length,"
        " ids: links.slice(0,3).map(a => (a.closest('tr')?.id || '') + '/' + (a.getAttribute('data-topic_id') || '')).join(', '),"
        " loggedIn: !!document.getElementById('logged-in-username'),"
        " loginForm: !!document.querySelector('input[name=\"login_password\"]'),"
        " magnet: !!document.querySelector('a[href*=\"xt=urn:btih:\"]')};"
        "})()"
        completionHandler:^(id value, NSError* error) {
            if (!self->_pending || self->_serial != serial) return;
            if (error || ![value isKindOfClass:[NSDictionary class]]) {
                [self.window orderOut:nil];
                [self finishWithHtml:nil url:webView.URL
                    error:error.localizedDescription ?: @"Cannot inspect RuTracker browser page"];
                return;
            }

            NSDictionary* snapshot = value;
            NSString* html = snapshot[@"html"] ?: @"";
            NSString* path = webView.URL.path ?: @"";
            const bool loggedIn = [snapshot[@"loggedIn"] boolValue];

            if (self->_pending->purpose == rats::net::BrowserPurpose::Authorize) {
                if (loggedIn) {
                    qInfo() << "[RuTrackerBrowser] browser authorization confirmed"
                            << QString::fromNSString(webView.URL.host ?: @"");
                    [self.window orderOut:nil];
                    [self finishWithHtml:html url:webView.URL error:nil];
                    return;
                }

                // Login/Cloudflare/captcha stays visible and interactive. WebKit
                // keeps handling every redirect until the logged-in marker
                // appears in this same browser session.
                [self.window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                return;
            }

            const bool search = self->_pending->target.path().endsWith("tracker.php");
            if (search) {
                qInfo() << "[RuTrackerBrowser] search DOM"
                        << "path" << QString::fromNSString(path)
                        << "table" << [snapshot[@"table"] boolValue]
                        << "rows" << [snapshot[@"rows"] intValue]
                        << "topicLinks" << [snapshot[@"links"] intValue]
                        << "row/topic IDs" << QString::fromNSString(snapshot[@"ids"]);
            }

            const bool ready = search
                ? ([path hasSuffix:@"/tracker.php"] && [snapshot[@"table"] boolValue])
                : ([path containsString:@"viewtopic.php"]
                    && [snapshot[@"magnet"] boolValue]);
            if (ready) {
                [self.window orderOut:nil];
                [self finishWithHtml:html url:webView.URL error:nil];
                return;
            }

            const bool onTarget =
                [webView.URL.host isEqualToString:self->_pending->target.host().toNSString()]
                && [path isEqualToString:self->_pending->target.path().toNSString()];

            if (loggedIn && !onTarget) {
                NSURL* destination = [NSURL URLWithString:
                    QString::fromLatin1(self->_pending->target.toEncoded(QUrl::FullyEncoded)).toNSString()];
                [webView loadRequest:[NSURLRequest requestWithURL:destination]];
                return;
            }

            const bool interactive =
                [snapshot[@"loginForm"] boolValue]
                || [html rangeOfString:@"cf-chl-" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"challenge-platform" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"captcha" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"Just a moment" options:NSCaseInsensitiveSearch].location != NSNotFound;

            if (!interactive) {
                [self.window orderOut:nil];
                NSString* problem = [NSString stringWithFormat:
                    @"RuTracker returned an empty or unexpected browser page (%@)",
                    webView.URL.absoluteString ?: @"unknown URL"];
                [self finishWithHtml:nil url:webView.URL error:problem];
                return;
            }

            // Search requests that encounter authentication or Cloudflare are
            // resolved by the user in the same persistent WebKit session.
            [self.window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }];
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    if (_pending) [self finishWithHtml:nil url:webView.URL error:error.localizedDescription];
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    if (_pending && error.code != NSURLErrorCancelled)
        [self finishWithHtml:nil url:webView.URL error:error.localizedDescription];
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    [sender orderOut:nil];
    if (_pending)
        [self finishWithHtml:nil url:_web.URL error:@"RuTracker browser authorization was cancelled"];
    return NO;
}
@end

namespace rats::net {

RuTrackerBrowser::RuTrackerBrowser()
    : bridge_((__bridge_retained void*)[[RatsRuTrackerWebBridge alloc] init])
{
}

RuTrackerBrowser::~RuTrackerBrowser()
{
    auto* bridge = (__bridge_transfer RatsRuTrackerWebBridge*)bridge_;
    [bridge cancel];
    bridge.web.navigationDelegate = nil;
    bridge.window.delegate = nil;
    [bridge.window close];
}

void RuTrackerBrowser::get(const QUrl& url, Completion completion)
{
    [(__bridge RatsRuTrackerWebBridge*)bridge_ get:url completion:std::move(completion)];
}

void RuTrackerBrowser::authorize(const QUrl& loginUrl, Completion completion)
{
    [(__bridge RatsRuTrackerWebBridge*)bridge_ authorize:loginUrl completion:std::move(completion)];
}

void RuTrackerBrowser::clearSession(std::function<void()> completion)
{
    [(__bridge RatsRuTrackerWebBridge*)bridge_ clearSession:std::move(completion)];
}

void RuTrackerBrowser::cancel()
{
    [(__bridge RatsRuTrackerWebBridge*)bridge_ cancel];
}

} // namespace rats::net
#endif
