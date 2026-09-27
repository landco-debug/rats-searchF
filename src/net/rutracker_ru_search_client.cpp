#include "net/rutracker_ru_search_client.h"

#include "net/cloudflare_clearance.h"
#include "net/rutracker_ru_source.h"
#include "net/source_parse_utils.h"

#include <algorithm>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace rats::net {
namespace {

const QList<QUrl>& officialMirrors()
{
    // The user's current working browser session is on rutracker.net, and the
    // current qBittorrent plugin lists .org + .net as official mirrors. Prefer
    // .net first because .org has current 403/Cloudflare reports; retain .org
    // and .nl as fallbacks for region-dependent reachability.
    static const QList<QUrl> mirrors {
        QUrl(QStringLiteral("https://rutracker.net")),
        QUrl(QStringLiteral("https://rutracker.org")),
        QUrl(QStringLiteral("https://rutracker.nl"))
    };
    return mirrors;
}

QNetworkRequest requestFor(const QUrl& url, const QString& userAgent)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        userAgent.isEmpty()
            ? QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                             "AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Chrome/154 Safari/537.36")
            : userAgent);
    request.setRawHeader("Accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
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

bool replyLooksCloudflare(
    QNetworkReply* reply, int status, const QByteArray& body)
{
    if (looksLikeCloudflareChallenge(status, body))
        return true;
    return reply && !reply->rawHeader("cf-ray").isEmpty()
        && (status == 403 || status == 429 || status == 503);
}

QString mirrorLabel(const QUrl& base)
{
    return base.host().isEmpty() ? base.toString() : base.host();
}

} // namespace

RuTrackerRuSearchClient::RuTrackerRuSearchClient(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
    , clearance_(new CloudflareClearance(this))
    , currentBaseUrl_(officialMirrors().first())
{
    resetCookieJar();

    connect(clearance_, &CloudflareClearance::solved, this,
        [this](const QUrl& url, const QString& userAgent,
            const QList<QNetworkCookie>& cookies) {
            if (clearanceGeneration_ != generation_ || finishedEmitted_)
                return;

            if (!userAgent.isEmpty())
                userAgent_ = userAgent;
            if (QNetworkCookieJar* jar = networkManager_->cookieJar())
                jar->setCookiesFromUrl(cookies, url);

            const int generation = clearanceGeneration_;
            const ClearancePurpose purpose = clearancePurpose_;
            clearanceGeneration_ = -1;
            clearancePurpose_ = ClearancePurpose::None;

            if (purpose == ClearancePurpose::Login)
                authenticateCurrentMirror(generation);
            else if (purpose == ClearancePurpose::Search)
                fetchSearchPage(generation);
        });

    connect(clearance_, &CloudflareClearance::failed, this,
        [this](const QUrl&, const QString& error) {
            if (clearanceGeneration_ != generation_ || finishedEmitted_)
                return;

            const int generation = clearanceGeneration_;
            const ClearancePurpose purpose = clearancePurpose_;
            clearanceGeneration_ = -1;
            clearancePurpose_ = ClearancePurpose::None;

            if (purpose == ClearancePurpose::Login) {
                tryNextMirror(generation,
                    tr("Cloudflare clearance failed: %1").arg(error));
            } else {
                finishNow(generation,
                    tr("RuTracker search is blocked by Cloudflare and the "
                       "system-browser clearance failed: %1").arg(error));
            }
        });
}

RuTrackerRuSearchClient::~RuTrackerRuSearchClient()
{
    cancel();
}

void RuTrackerRuSearchClient::resetCookieJar()
{
    if (!networkManager_)
        return;
    if (QNetworkCookieJar* old = networkManager_->cookieJar())
        old->deleteLater();
    networkManager_->setCookieJar(new QNetworkCookieJar(networkManager_));
}

void RuTrackerRuSearchClient::setCredentials(
    const QString& username, const QString& password)
{
    const QString cleanUser = username.trimmed();
    if (cleanUser == username_ && password == password_)
        return;

    username_ = cleanUser;
    password_ = password;
    authenticated_ = false;
    mirrorIndex_ = 0;
    currentBaseUrl_ = officialMirrors().first();
    userAgent_.clear();
    mirrorErrors_.clear();
    resetCookieJar();
}

bool RuTrackerRuSearchClient::isConfigured() const
{
    return !username_.isEmpty() && !password_.isEmpty();
}

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
    searchClearanceRetried_ = false;
    searchPageResolved_ = false;
    clearanceGeneration_ = -1;
    clearancePurpose_ = ClearancePurpose::None;
    if (clearance_)
        clearance_->cancel();

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
    searchClearanceRetried_ = false;
    searchPageResolved_ = false;
    finishedEmitted_ = false;

    const int generation = generation_;
    if (currentQuery_.isEmpty()) {
        finishNow(generation, tr("Empty RuTracker search query."));
        return;
    }
    if (!isConfigured()) {
        finishNow(generation,
            tr("RuTracker account is not configured in Settings > Indexer."));
        return;
    }

    qInfo() << "[RuTrackerRuSearchClient] search"
            << currentQuery_.left(80) << "limit" << requestedLimit_;

    if (authenticated_)
        fetchSearchPage(generation);
    else
        authenticate(generation);
}

void RuTrackerRuSearchClient::authenticate(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!isConfigured()) {
        finishNow(generation, tr("RuTracker credentials are missing."));
        return;
    }

    const QList<QUrl>& mirrors = officialMirrors();
    if (mirrorIndex_ < 0 || mirrorIndex_ >= mirrors.size())
        mirrorIndex_ = 0;
    currentBaseUrl_ = mirrors.at(mirrorIndex_);
    clearanceRetriedForMirror_ = false;
    mirrorErrors_.clear();
    resetCookieJar();
    authenticateCurrentMirror(generation);
}

void RuTrackerRuSearchClient::requestCloudflareClearance(
    const QUrl& url, int generation, ClearancePurpose purpose)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!clearance_ || !clearance_->isSupported()) {
        if (purpose == ClearancePurpose::Login) {
            tryNextMirror(generation,
                tr("Cloudflare challenge; no system-browser clearance backend."));
        } else {
            finishNow(generation,
                tr("RuTracker returned a Cloudflare challenge and this platform "
                   "has no system-browser clearance backend."));
        }
        return;
    }

    clearanceGeneration_ = generation;
    clearancePurpose_ = purpose;
    clearance_->solve(url, 65000);
}

void RuTrackerRuSearchClient::tryNextMirror(
    int generation, const QString& reason)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    mirrorErrors_ << tr("%1: %2")
        .arg(mirrorLabel(currentBaseUrl_), reason);

    ++mirrorIndex_;
    const QList<QUrl>& mirrors = officialMirrors();
    if (mirrorIndex_ >= mirrors.size()) {
        authenticated_ = false;
        finishNow(generation,
            tr("RuTracker authentication failed on all current official "
               "mirrors. %1").arg(mirrorErrors_.join(QStringLiteral(" | "))));
        return;
    }

    currentBaseUrl_ = mirrors.at(mirrorIndex_);
    clearanceRetriedForMirror_ = false;
    userAgent_.clear();
    resetCookieJar();
    authenticateCurrentMirror(generation);
}

void RuTrackerRuSearchClient::authenticateCurrentMirror(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    QUrl loginUrl = currentBaseUrl_;
    loginUrl.setPath(QStringLiteral("/forum/login.php"));
    loginUrl.setQuery(QString());

    QNetworkRequest request = requestFor(loginUrl, userAgent_);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Referer", loginUrl.toEncoded());

    QByteArray body;
    body += "login_username=";
    body += sourceparse::formEncodeWindows1251(username_);
    body += "&login_password=";
    body += sourceparse::formEncodeWindows1251(password_);
    body += "&login=";
    body += sourceparse::formEncodeWindows1251(QString::fromUtf8("Вход"));
    body += "&redirect=index.php";

    QNetworkReply* reply = networkManager_->post(request, body);
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, loginUrl]() {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const QString errorText = reply->errorString();
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            const bool cloudflare
                = replyLooksCloudflare(reply, status, raw);
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (cloudflare) {
                if (!clearanceRetriedForMirror_) {
                    clearanceRetriedForMirror_ = true;
                    QUrl origin = currentBaseUrl_;
                    origin.setPath(QStringLiteral("/forum/login.php"));
                    requestCloudflareClearance(
                        origin, generation, ClearancePurpose::Login);
                    return;
                }
                tryNextMirror(generation,
                    tr("Cloudflare still blocks the login POST after clearance."));
                return;
            }

            if (error != QNetworkReply::NoError) {
                tryNextMirror(generation,
                    tr("login request failed: %1").arg(errorText));
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
                QString reason;
                if (html.contains(QStringLiteral("captcha"), Qt::CaseInsensitive)
                    || html.contains(QStringLiteral("cap_sid"), Qt::CaseInsensitive)) {
                    reason = tr("site requested a captcha");
                } else if (hasLoginForm(html)) {
                    reason = tr("login form remained after POST");
                } else {
                    reason = tr("no authenticated session cookie was returned");
                }
                tryNextMirror(generation, reason);
                return;
            }

            authenticated_ = true;
            mirrorErrors_.clear();
            qInfo() << "[RuTrackerRuSearchClient] authenticated via"
                    << currentBaseUrl_.host();
            fetchSearchPage(generation);
        });
}

void RuTrackerRuSearchClient::fetchSearchPage(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    const QUrl url = RuTrackerRuSource::searchUrl(
        currentQuery_, currentSortKey_, currentContentType_, currentBaseUrl_);
    QNetworkReply* reply
        = networkManager_->get(requestFor(url, userAgent_));
    replies_.insert(reply);

    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, url]() {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const QString errorText = reply->errorString();
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QUrl finalUrl = reply->url();
            const QByteArray body = reply->readAll();
            const bool cloudflare
                = replyLooksCloudflare(reply, status, body);
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (cloudflare) {
                if (!searchClearanceRetried_) {
                    searchClearanceRetried_ = true;
                    QUrl origin = currentBaseUrl_;
                    origin.setPath(QStringLiteral("/forum/tracker.php"));
                    requestCloudflareClearance(
                        origin, generation, ClearancePurpose::Search);
                    return;
                }
                finishNow(generation,
                    tr("RuTracker still returns a Cloudflare challenge after "
                       "system-browser clearance on %1.")
                        .arg(currentBaseUrl_.host()));
                return;
            }

            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("RuTracker search request failed on %1: %2")
                        .arg(currentBaseUrl_.host(), errorText));
                return;
            }

            const QString pageText = sourceparse::decodeTrackerText(body);
            if (hasLoginForm(pageText)
                && !pageText.contains(QStringLiteral(R"(id="tor-tbl")"),
                    Qt::CaseInsensitive)) {
                authenticated_ = false;
                if (!authRetried_) {
                    authRetried_ = true;
                    resetCookieJar();
                    clearanceRetriedForMirror_ = false;
                    authenticateCurrentMirror(generation);
                    return;
                }
                finishNow(generation,
                    tr("RuTracker session expired and re-authentication failed."));
                return;
            }

            const int candidateCap
                = qMin(100, qMax(requestedLimit_, requestedLimit_ * 2));
            QVector<domain::Torrent> candidates
                = RuTrackerRuSource::parseSearchPage(
                    body, finalUrl, candidateCap);

            if (candidates.isEmpty()) {
                searchPageResolved_ = true;
                finishNow(generation,
                    tr("RuTracker returned no exact torrent rows for this query "
                       "on %1.").arg(currentBaseUrl_.host()));
                return;
            }

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
        });
}

void RuTrackerRuSearchClient::processQueue(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    while (activeDetails_ < kMaxConcurrentDetails
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
    QNetworkReply* reply
        = networkManager_->get(requestFor(job.url, userAgent_));
    replies_.insert(reply);

    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QUrl finalUrl = reply->url();
            const QByteArray body = reply->readAll();
            const bool cloudflare
                = replyLooksCloudflare(reply, status, body);
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            bool accepted = false;
            if (!cloudflare
                && error == QNetworkReply::NoError
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
        });
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
    finishNow(generation);
}

void RuTrackerRuSearchClient::finishNow(
    int generation, const QString& error)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    finishedEmitted_ = true;
    qInfo() << "[RuTrackerRuSearchClient] finished"
            << "accepted" << accepted_
            << "rejected" << rejected_
            << (error.isEmpty() ? QString() : error);
    emit searchFinished(
        currentQuery_, accepted_, rejected_, error);
}

} // namespace rats::net
