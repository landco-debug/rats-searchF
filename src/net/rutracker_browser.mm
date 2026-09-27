#include "net/rutracker_browser.h"

#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <memory>
#include <utility>

namespace rats::net {
namespace {
struct Pending {
    QUrl target;
    RuTrackerBrowser::Completion callback;
    unsigned long serial = 0;
};
}
}

@interface RatsRuTrackerWebBridge : NSObject <WKNavigationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) WKWebView* web;
- (void)get:(const QUrl&)url completion:(rats::net::RuTrackerBrowser::Completion)completion;
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
        // The same window is reused for many searches. AppKit's default
        // releasedWhenClosed would leave our retained _window pointer stale
        // after the user presses the red close button.
        _window.releasedWhenClosed = NO;
        _window.title = @"RuTracker — войдите на сайт";
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

- (void)get:(const QUrl&)url completion:(rats::net::RuTrackerBrowser::Completion)completion {
    [self cancel];
    _pending = std::make_unique<rats::net::Pending>();
    _pending->target = url;
    _pending->callback = std::move(completion);
    _pending->serial = _serial;
    // WebKit's persistent store survives application replacement. Never copy
    // clearance into QNetworkCookieJar: its TLS/browser fingerprint differs.
    NSURL* nsurl = [NSURL URLWithString:
        QString::fromLatin1(url.toEncoded(QUrl::FullyEncoded)).toNSString()];
    if (!nsurl) {
        [self finishWithHtml:nil url:nil error:@"Invalid RuTracker URL"];
        return;
    }
    [_web loadRequest:[NSURLRequest requestWithURL:nsurl]];
    unsigned long serial = _serial;
    __weak RatsRuTrackerWebBridge* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        RatsRuTrackerWebBridge* strongSelf = weakSelf;
        if (strongSelf && strongSelf->_pending && strongSelf->_serial == serial)
            [strongSelf finishWithHtml:nil url:strongSelf.web.URL
                error:@"RuTracker browser login timed out (3 minutes)"];
    });
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (!_pending) return;
    const unsigned long serial = _serial;
    // DOM serialization is UTF-8 even when RuTracker's source is Windows-1251.
    [webView evaluateJavaScript:@"document.documentElement.outerHTML"
        completionHandler:^(id value, NSError* error) {
            if (!self->_pending || self->_serial != serial) return;
            NSString* html = [value isKindOfClass:[NSString class]] ? value : @"";
            NSString* path = webView.URL.path ?: @"";
            const bool search = self->_pending->target.path().endsWith("tracker.php");
            const bool ready = search
                ? [html containsString:@"tor-tbl"]
                : ([path containsString:@"viewtopic.php"]
                    && [html rangeOfString:@"xt=urn:btih:" options:NSCaseInsensitiveSearch].location != NSNotFound);
            if (ready) {
                [self.window orderOut:nil];
                [self finishWithHtml:html url:webView.URL error:nil];
                return;
            }
            const bool loggedIn = [html containsString:@"logged-in-username"];
            const bool onTarget = [webView.URL.host isEqualToString:self->_pending->target.host().toNSString()]
                && [path isEqualToString:self->_pending->target.path().toNSString()];
            if (loggedIn && !onTarget) {
                NSURL* destination = [NSURL URLWithString:
                    QString::fromLatin1(self->_pending->target.toEncoded(QUrl::FullyEncoded)).toNSString()];
                [webView loadRequest:[NSURLRequest requestWithURL:destination]];
                return;
            }
            const bool interactive =
                [html rangeOfString:@"login_password" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"cf-chl-" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"challenge-platform" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"captcha" options:NSCaseInsensitiveSearch].location != NSNotFound
                || [html rangeOfString:@"Just a moment" options:NSCaseInsensitiveSearch].location != NSNotFound;
            if (!interactive) {
                // A completed blank/unexpected page cannot become a torrent
                // listing by waiting. Give a provider error, not a white UI.
                [self.window orderOut:nil];
                NSString* problem = [NSString stringWithFormat:
                    @"RuTracker returned an empty or unexpected browser page (%@)",
                    webView.URL.absoluteString ?: @"unknown URL"];
                [self finishWithHtml:nil url:webView.URL error:problem];
                return;
            }
            // A real login/captcha/Cloudflare page remains interactive.
            [self.window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            (void)error;
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
    // Keep the reusable window and WKWebView alive. Closing the native window
    // while a later search still owns this bridge caused a stale window call.
    [sender orderOut:nil];
    if (_pending)
        [self finishWithHtml:nil url:_web.URL error:@"RuTracker browser login was cancelled"];
    return NO;
}
@end

namespace rats::net {
RuTrackerBrowser::RuTrackerBrowser()
    : bridge_((__bridge_retained void*)[[RatsRuTrackerWebBridge alloc] init]) {}
RuTrackerBrowser::~RuTrackerBrowser() {
    auto* bridge = (__bridge_transfer RatsRuTrackerWebBridge*)bridge_;
    [bridge cancel];
    bridge.web.navigationDelegate = nil;
    bridge.window.delegate = nil;
    [bridge.window close];
}
void RuTrackerBrowser::get(const QUrl& url, Completion completion) {
    [(__bridge RatsRuTrackerWebBridge*)bridge_ get:url completion:std::move(completion)];
}
void RuTrackerBrowser::cancel() {
    [(__bridge RatsRuTrackerWebBridge*)bridge_ cancel];
}
}
#endif
