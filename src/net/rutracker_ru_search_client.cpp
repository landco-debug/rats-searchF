#include "net/rutracker_ru_search_client.h"

#include "net/rutracker_ru_source.h"
#include "net/source_parse_utils.h"

#include <algorithm>
#include <QDateTime>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>

namespace rats::net {
namespace {

class SnapshotCookieJar final : public QNetworkCookieJar {
public:
    explicit SnapshotCookieJar(QObject* parent = nullptr)
        : QNetworkCookieJar(parent)
    {
    }

    QList<QNetworkCookie> snapshot() const
    {
        return allCookies();
    }
};

QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/154.0.0.0 Safari/537.36"));
    request.setRawHeader("Accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setRawHeader("DNT", "1");
    request.setRawHeader("Upgrade-Insecure-Requests", "1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    return request;
}

bool hasLoginForm(const QString& html)
{
    return html.contains(QStringLiteral("login_username"), Qt::CaseInsensitive)
        && html.contains(QStringLiteral("login_password"), Qt::CaseInsensitive);
}

bool looksLikeChallenge(const QString& html)
{
    return html.contains(QStringLiteral("cf-chl-"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("/cdn-cgi/challenge-platform"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("Just a moment"), Qt::CaseInsensitive)
        || html.contains(QStringLiteral("captcha"), Qt::CaseInsensitive);
}

} // namespace

RuTrackerRuSearchClient::RuTrackerRuSearchClient(QObject* parent)
    : QObject(parent)
{
    resetMirrorCycle();
#ifdef __APPLE__
    // macOS uses one persistent WebKit transport for login, Cloudflare,
    // searches and exact topic pages. Remove the obsolete plaintext credential
    // preferences and old Qt-cookie snapshots left by Stages 25-32.
    browser_ = std::make_unique<RuTrackerBrowser>();
    browserMode_ = true;
    QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
    settings.remove(QStringLiteral("rutracker/username"));
    settings.remove(QStringLiteral("rutracker/password"));
    settings.remove(QStringLiteral("rutracker/sessions"));
    settings.sync();
#else
    networkManager_ = new QNetworkAccessManager(this);
    resetCookieJar(false);
#endif
}

RuTrackerRuSearchClient::~RuTrackerRuSearchClient()
{
    cancel();
}

QString RuTrackerRuSearchClient::sessionSettingsGroup(const QString& host) const
{
    return QStringLiteral("rutracker/sessions/%1").arg(host.toLower());
}

void RuTrackerRuSearchClient::resetCookieJar(bool restorePersisted)
{
    if (!networkManager_)
        return;

    if (QNetworkCookieJar* old = networkManager_->cookieJar())
        old->deleteLater();

    networkManager_->setCookieJar(new SnapshotCookieJar(networkManager_));
    authenticated_ = false;

    if (restorePersisted && isConfigured())
        authenticated_ = restorePersistedSession();
}

bool RuTrackerRuSearchClient::restorePersistedSession()
{
    if (!networkManager_ || !isConfigured())
        return false;

    const QUrl base(activeMirrorBaseUrl_);
    const QString host = base.host().toLower();
    if (host.isEmpty())
        return false;

    QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
    settings.beginGroup(sessionSettingsGroup(host));

    const QString owner = settings.value(QStringLiteral("owner")).toString();
    const QStringList encodedCookies
        = settings.value(QStringLiteral("cookies")).toStringList();
    settings.endGroup();

    if (owner != username_ || encodedCookies.isEmpty())
        return false;

    QList<QNetworkCookie> restored;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QString& encoded : encodedCookies) {
        const QByteArray raw = QByteArray::fromBase64(encoded.toLatin1());
        const QList<QNetworkCookie> parsed = QNetworkCookie::parseCookies(raw);
        for (const QNetworkCookie& cookie : parsed) {
            if (!cookie.isSessionCookie()
                && cookie.expirationDate().isValid()
                && cookie.expirationDate().toUTC() <= now) {
                continue;
            }
            restored.append(cookie);
        }
    }

    if (restored.isEmpty()) {
        clearPersistedSessionForActiveMirror();
        return false;
    }

    const QUrl scope = urlOnActiveMirror(QStringLiteral("/forum/index.php"));
    if (!networkManager_->cookieJar()->setCookiesFromUrl(restored, scope)) {
        clearPersistedSessionForActiveMirror();
        return false;
    }

    qInfo() << "[RuTrackerRuSearchClient] restored persisted session for"
            << host << "cookies" << restored.size();
    return true;
}

void RuTrackerRuSearchClient::persistActiveSession()
{
    if (!networkManager_ || !isConfigured())
        return;

    const QUrl base(activeMirrorBaseUrl_);
    const QString host = base.host().toLower();
    if (host.isEmpty())
        return;

    auto* jar = dynamic_cast<SnapshotCookieJar*>(networkManager_->cookieJar());
    if (!jar)
        return;

    QStringList encodedCookies;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QNetworkCookie& cookie : jar->snapshot()) {
        if (!cookie.isSessionCookie()
            && cookie.expirationDate().isValid()
            && cookie.expirationDate().toUTC() <= now) {
            continue;
        }
        const QByteArray raw = cookie.toRawForm(QNetworkCookie::Full);
        if (!raw.isEmpty())
            encodedCookies.append(QString::fromLatin1(raw.toBase64()));
    }

    if (encodedCookies.isEmpty())
        return;

    QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
    settings.beginGroup(sessionSettingsGroup(host));
    settings.setValue(QStringLiteral("owner"), username_);
    settings.setValue(QStringLiteral("cookies"), encodedCookies);
    settings.setValue(QStringLiteral("savedAt"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    settings.endGroup();
    settings.sync();

    qInfo() << "[RuTrackerRuSearchClient] persisted session for"
            << host << "cookies" << encodedCookies.size();
}

void RuTrackerRuSearchClient::clearPersistedSessionForActiveMirror()
{
    const QString host = QUrl(activeMirrorBaseUrl_).host().toLower();
    if (host.isEmpty())
        return;

    QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
    settings.beginGroup(sessionSettingsGroup(host));
    settings.remove(QString());
    settings.endGroup();
    settings.sync();
}

void RuTrackerRuSearchClient::clearAllPersistedSessions()
{
    QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
    settings.beginGroup(QStringLiteral("rutracker/sessions"));
    settings.remove(QString());
    settings.endGroup();
    settings.sync();
}

void RuTrackerRuSearchClient::resetMirrorCycle()
{
    mirrorIndex_ = 0;
    activeMirrorBaseUrl_ = mirrorBaseUrls_.isEmpty()
        ? QStringLiteral("https://rutracker.net")
        : mirrorBaseUrls_.first();
    lastMirrorError_.clear();
}

QUrl RuTrackerRuSearchClient::urlOnActiveMirror(const QString& path) const
{
    QUrl url(activeMirrorBaseUrl_);
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

bool RuTrackerRuSearchClient::tryNextMirror(
    int generation, const QString& reason)
{
    if (generation != generation_ || finishedEmitted_)
        return false;

    lastMirrorError_ = reason;
    const int next = mirrorIndex_ + 1;
    if (next >= mirrorBaseUrls_.size()) {
#ifdef __APPLE__
        // Keep all subsequent requests in WebKit. Moving only cookies back to
        // QNetworkAccessManager would lose the browser's network fingerprint.
        browserMode_ = true;
        if (!browser_)
            browser_ = std::make_unique<RuTrackerBrowser>();
        fetchSearchPage(generation);
        return true;
#else
        finishNow(generation,
            tr("RuTracker failed on all official mirrors. Last error: %1")
                .arg(reason));
        return false;
#endif
    }

    mirrorIndex_ = next;
    activeMirrorBaseUrl_ = mirrorBaseUrls_.at(mirrorIndex_);
    authRetried_ = false;

    qInfo() << "[RuTrackerRuSearchClient] switching mirror to"
            << activeMirrorBaseUrl_ << "after" << reason;

#ifdef __APPLE__
    if (browserMode_) {
        // Keep mirror failover inside the same persistent WebKit transport.
        fetchSearchPage(generation);
        return true;
    }
#endif

    resetCookieJar(true);
    if (authenticated_)
        fetchSearchPage(generation);
    else
        authenticate(generation);
    return true;
}

void RuTrackerRuSearchClient::setCredentials(
    const QString& username, const QString& password)
{
#ifdef __APPLE__
    // Kept only for API compatibility with shared application code. macOS no
    // longer stores or uses tracker credentials outside WebKit.
    Q_UNUSED(username);
    Q_UNUSED(password);
    return;
#else
    const QString cleanUser = username.trimmed();
    if (cleanUser == username_ && password == password_)
        return;

    const bool hadCredentials = !username_.isEmpty() || !password_.isEmpty();
    const bool credentialsChanged = hadCredentials
        && (cleanUser != username_ || password != password_);

    username_ = cleanUser;
    password_ = password;

    if (credentialsChanged)
        clearAllPersistedSessions();

    resetMirrorCycle();
    resetCookieJar(true);
#endif
}

bool RuTrackerRuSearchClient::isConfigured() const
{
#ifdef __APPLE__
    return true;
#else
    return !username_.isEmpty() && !password_.isEmpty();
#endif
}

#ifdef __APPLE__
void RuTrackerRuSearchClient::reloginInBrowser()
{
    cancel();
    resetMirrorCycle();
    browserMode_ = true;
    if (!browser_)
        browser_ = std::make_unique<RuTrackerBrowser>();

    emit browserAuthorizationChanged(false, tr("Clearing RuTracker browser session…"));
    browser_->clearSession([this]() {
        const QUrl loginUrl = urlOnActiveMirror(QStringLiteral("/forum/login.php"));
        emit browserAuthorizationChanged(false, tr("Complete authorization in the RuTracker window."));
        browser_->authorize(loginUrl,
            [this](const QByteArray&, const QUrl&, const QString& error) {
                if (!error.isEmpty()) {
                    emit browserAuthorizationChanged(false,
                        tr("RuTracker authorization failed: %1").arg(error));
                    return;
                }
                emit browserAuthorizationChanged(true,
                    tr("RuTracker browser session is authorized."));
            });
    });
}
#endif

void RuTrackerRuSearchClient::cancel()
{
    ++generation_;
    finishedEmitted_ = true;
    currentQuery_.clear();
    currentSortKey_.clear();
    currentContentType_.clear();
    detailQueue_.clear();
    activeDetails_ = 0;
    accepted_ = 0;
    rejected_ = 0;
    authRetried_ = false;
    searchPageResolved_ = false;

#ifdef __APPLE__
    if (browser_)
        browser_->cancel();
#endif
    const QList<QNetworkReply*> outstanding = replies_.values();
    replies_.clear();
    for (QNetworkReply* reply : outstanding) {
        if (reply)
            reply->abort();
    }
}

void RuTrackerRuSearchClient::search(
    const QString& query, int limit, const QString& sortKey,
    const QString& contentType)
{
    cancel();

    currentQuery_ = query.trimmed();
    currentSortKey_ = sortKey;
    currentContentType_ = contentType.trimmed().toLower();
    requestedLimit_ = qBound(1, limit, 50);
    accepted_ = 0;
    rejected_ = 0;
    activeDetails_ = 0;
    authRetried_ = false;
    searchPageResolved_ = false;
    finishedEmitted_ = false;

    const int generation = generation_;
    if (currentQuery_.isEmpty()) {
        finishNow(generation, tr("Empty RuTracker search query."));
        return;
    }

    qInfo() << "[RuTrackerRuSearchClient] search"
            << currentQuery_.left(80) << "limit" << requestedLimit_;

#ifdef __APPLE__
    // macOS is browser-only. Never fall back to the old credential/QNetwork
    // path, because it cannot share WebKit's Cloudflare/network fingerprint.
    browserMode_ = true;
    if (!browser_)
        browser_ = std::make_unique<RuTrackerBrowser>();
    resetMirrorCycle();
    fetchSearchPage(generation);
    return;
#else
    if (!isConfigured()) {
        finishNow(generation,
            tr("RuTracker account is not configured in Settings > Indexer."));
        return;
    }
#endif
    if (authenticated_) {
        fetchSearchPage(generation);
    } else {
        resetMirrorCycle();
        resetCookieJar(true);
        if (authenticated_)
            fetchSearchPage(generation);
        else
            authenticate(generation);
    }
}

void RuTrackerRuSearchClient::authenticate(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!isConfigured()) {
        finishNow(generation, tr("RuTracker credentials are missing."));
        return;
    }

    const QUrl loginUrl = urlOnActiveMirror(QStringLiteral("/forum/login.php"));
    QNetworkRequest request = requestFor(loginUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Referer", loginUrl.toEncoded());

    QByteArray body;
    body += "login_username=";
    body += sourceparse::formEncodeWindows1251(username_);
    body += "&login_password=";
    body += sourceparse::formEncodeWindows1251(password_);
    body += "&login=Login&redirect=index.php";

    QNetworkReply* reply = networkManager_->post(request, body);
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, loginUrl]() {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray raw = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                authenticated_ = false;
                tryNextMirror(generation,
                    tr("%1 login request failed: %2")
                        .arg(activeMirrorBaseUrl_, errorText));
                return;
            }

            bool hasSessionCookie = false;
            const QList<QNetworkCookie> cookies
                = networkManager_->cookieJar()->cookiesForUrl(loginUrl);
            for (const QNetworkCookie& cookie : cookies) {
                if (cookie.name() == QByteArrayLiteral("bb_session")
                    && !cookie.value().isEmpty()) {
                    hasSessionCookie = true;
                    break;
                }
            }

            const QString html = sourceparse::decodeTrackerText(raw);
            const bool loggedInMarker = html.contains(
                QStringLiteral(R"(id="logged-in-username")"),
                Qt::CaseInsensitive);

            if (!hasSessionCookie && !loggedInMarker) {
                authenticated_ = false;
                const QString cause = looksLikeChallenge(html)
                    ? tr("anti-bot/captcha challenge")
                    : tr("authentication was not accepted");
                tryNextMirror(generation,
                    tr("%1 %2 (final URL: %3)")
                        .arg(activeMirrorBaseUrl_, cause, finalUrl.toString()));
                return;
            }

            authenticated_ = true;
            persistActiveSession();
            qInfo() << "[RuTrackerRuSearchClient] authenticated on"
                    << activeMirrorBaseUrl_;
            fetchSearchPage(generation);
        });
}

void RuTrackerRuSearchClient::fetchSearchPage(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    QUrl url = RuTrackerRuSource::searchUrl(
        currentQuery_, currentSortKey_, currentContentType_);
    const QUrl base(activeMirrorBaseUrl_);
    url.setScheme(base.scheme());
    url.setHost(base.host());
    if (base.port() >= 0)
        url.setPort(base.port());

#ifdef __APPLE__
    if (browserMode_) {
        browser_->get(url, [this, generation](const QByteArray& body,
                                      const QUrl& finalUrl, const QString& error) {
            if (generation != generation_ || finishedEmitted_)
                return;
            if (!error.isEmpty()) {
                emit browserAuthorizationChanged(false,
                    tr("RuTracker browser request failed: %1").arg(error));
                tryNextMirror(generation,
                    tr("%1 browser request failed: %2")
                        .arg(activeMirrorBaseUrl_, error));
                return;
            }
            handleSearchPage(generation, body, finalUrl);
        });
        return;
    }
#endif
    QNetworkReply* reply = networkManager_->get(requestFor(url));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation]() {
            replies_.remove(reply);
            const QNetworkReply::NetworkError error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();
            if (generation != generation_ || finishedEmitted_)
                return;
            if (error != QNetworkReply::NoError) {
                authenticated_ = false;
                tryNextMirror(generation,
                    tr("%1 search request failed: %2")
                        .arg(activeMirrorBaseUrl_, errorText));
                return;
            }
            handleSearchPage(generation, body, finalUrl);
        });
}

void RuTrackerRuSearchClient::handleSearchPage(
    int generation, const QByteArray& body, const QUrl& finalUrl)
{
    const QString pageText = sourceparse::decodeTrackerText(body);
    const bool hasTorrentTable = pageText.contains(
        QStringLiteral(R"(id="tor-tbl")"), Qt::CaseInsensitive)
        || pageText.contains(
            QStringLiteral(R"(id='tor-tbl')"), Qt::CaseInsensitive);

    if ((hasLoginForm(pageText) || finalUrl.path().contains(
                QStringLiteral("login.php"), Qt::CaseInsensitive))
        && !hasTorrentTable) {
        authenticated_ = false;
#ifdef __APPLE__
        if (browserMode_) {
            emit browserAuthorizationChanged(false,
                tr("RuTracker authorization is required."));
            const QUrl loginUrl = urlOnActiveMirror(QStringLiteral("/forum/login.php"));
            browser_->authorize(loginUrl,
                [this, generation](const QByteArray&, const QUrl&, const QString& error) {
                    if (generation != generation_ || finishedEmitted_)
                        return;
                    if (!error.isEmpty()) {
                        emit browserAuthorizationChanged(false,
                            tr("RuTracker authorization failed: %1").arg(error));
                        finishNow(generation,
                            tr("RuTracker browser authorization failed: %1").arg(error));
                        return;
                    }
                    emit browserAuthorizationChanged(true,
                        tr("RuTracker browser session is authorized."));
                    fetchSearchPage(generation);
                });
            return;
        }
#endif
        if (!authRetried_) {
            authRetried_ = true;
            clearPersistedSessionForActiveMirror();
            resetCookieJar(false);
            authenticate(generation);
            return;
        }
        tryNextMirror(generation,
            tr("%1 session returned to the login page")
                .arg(activeMirrorBaseUrl_));
        return;
    }

    if (!hasTorrentTable && looksLikeChallenge(pageText)) {
        authenticated_ = false;
#ifdef __APPLE__
        if (browserMode_) {
            finishNow(generation, tr("RuTracker browser challenge is still active."));
            return;
        }
#endif
        tryNextMirror(generation,
            tr("%1 returned an anti-bot/captcha challenge")
                .arg(activeMirrorBaseUrl_));
        return;
    }

#ifdef __APPLE__
    if (browserMode_ && hasTorrentTable) {
        emit browserAuthorizationChanged(true,
            tr("RuTracker browser session is authorized."));
    }
#endif

    const int candidateCap
        = qMin(100, qMax(requestedLimit_, requestedLimit_ * 2));
    QVector<domain::Torrent> candidates
        = RuTrackerRuSource::parseSearchPage(
            body, finalUrl, candidateCap);

    qInfo() << "[RuTrackerRuSearchClient] search page"
            << "path" << finalUrl.path()
            << "bytes" << body.size()
            << "table" << hasTorrentTable
            << "parsed candidates" << candidates.size();

    if (candidates.isEmpty()) {
        if (!hasTorrentTable) {
            authenticated_ = false;
#ifdef __APPLE__
            if (browserMode_) {
                finishNow(generation, tr("RuTracker browser returned no tracker table."));
                return;
            }
#endif
            tryNextMirror(generation,
                tr("%1 returned an unexpected non-tracker page")
                    .arg(activeMirrorBaseUrl_));
            return;
        }
#ifdef __APPLE__
        if (!browserMode_)
#endif
            persistActiveSession();
        searchPageResolved_ = true;
        finishNow(generation,
            tr("RuTracker returned no exact torrent rows for this query."));
        return;
    }

#ifdef __APPLE__
    if (!browserMode_)
#endif
        persistActiveSession();

    std::stable_sort(candidates.begin(), candidates.end(),
        [this](const domain::Torrent& a, const domain::Torrent& b) {
            return sourceparse::contentTypeHintScore(
                       a, currentContentType_)
                > sourceparse::contentTypeHintScore(
                       b, currentContentType_);
        });

    searchPageResolved_ = true;
    for (domain::Torrent& torrent : candidates) {
        DetailJob job;
        job.url = QUrl(torrent.info
            .value(QStringLiteral("sourceUrl")).toString());
        job.torrent = std::move(torrent);
        if (job.url.isValid())
            detailQueue_.enqueue(std::move(job));
        else
            ++rejected_;
    }

    processQueue(generation);
}

void RuTrackerRuSearchClient::processQueue(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    int concurrency = kMaxConcurrentDetails;
#ifdef __APPLE__
    if (browserMode_)
        concurrency = 1;
#endif
    while (activeDetails_ < concurrency
        && !detailQueue_.isEmpty()
        && accepted_ < requestedLimit_) {
        DetailJob job = detailQueue_.dequeue();
        ++activeDetails_;
        fetchDetail(std::move(job), generation);
    }

    if (accepted_ >= requestedLimit_)
        detailQueue_.clear();

    finishIfIdle(generation);
}

void RuTrackerRuSearchClient::fetchDetail(
    DetailJob job, int generation)
{
#ifdef __APPLE__
    if (browserMode_) {
        // Evaluate the URL before moving job into the callback. Function
        // arguments have no guaranteed left-to-right evaluation order.
        const QUrl detailUrl = job.url;
        browser_->get(detailUrl, [this, generation, job = std::move(job)](
                       const QByteArray& body, const QUrl& finalUrl,
                       const QString& error) mutable {
            if (generation != generation_ || finishedEmitted_)
                return;
            if (!error.isEmpty()) {
                ++rejected_;
                --activeDetails_;
                finishNow(generation, tr("RuTracker browser detail: %1").arg(error));
                return;
            }
            handleDetailPage(std::move(job), generation, body, finalUrl);
        });
        return;
    }
#endif
    QNetworkReply* reply = networkManager_->get(requestFor(job.url));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const QNetworkReply::NetworkError error = reply->error();
            const QUrl finalUrl = reply->url();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();
            if (generation != generation_ || finishedEmitted_)
                return;
            handleDetailPage(std::move(job), generation, body, finalUrl);
        });
}

void RuTrackerRuSearchClient::handleDetailPage(
    DetailJob job, int generation, const QByteArray& body, const QUrl& finalUrl)
{
    bool accepted = false;
    if (!body.isEmpty()
        && RuTrackerRuSource::applyDetailPage(
            job.torrent, body, finalUrl)
        && RuTrackerRuSource::isStrictComplete(job.torrent)) {
        const bool typeMatches = currentContentType_.isEmpty()
            || domain::toString(job.torrent.contentType)
                   .compare(currentContentType_, Qt::CaseInsensitive)
                == 0;
        if (typeMatches && accepted_ < requestedLimit_) {
            accepted = true;
            ++accepted_;
            emit resultReady(currentQuery_, job.torrent);
        }
    }

    if (!accepted)
        ++rejected_;

    --activeDetails_;
    processQueue(generation);
}

void RuTrackerRuSearchClient::finishIfIdle(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!searchPageResolved_
        || activeDetails_ != 0
        || !detailQueue_.isEmpty()) {
        return;
    }
#ifdef __APPLE__
    if (browserMode_ && accepted_ == 0 && rejected_ > 0) {
        finishNow(generation,
            tr("RuTracker returned search rows, but none of %1 detail pages "
               "passed exact-release verification or the selected type filter.")
                .arg(rejected_));
        return;
    }
#endif
    finishNow(generation);
}

void RuTrackerRuSearchClient::finishNow(
    int generation, const QString& error)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    finishedEmitted_ = true;
    qInfo() << "[RuTrackerRuSearchClient] finished"
            << "mirror" << activeMirrorBaseUrl_
            << "accepted" << accepted_
            << "rejected" << rejected_
            << (error.isEmpty() ? QString() : error);
    emit searchFinished(
        currentQuery_, accepted_, rejected_, error);
}

} // namespace rats::net
