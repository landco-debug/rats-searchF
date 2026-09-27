#include "net/rutracker_ru_search_client.h"

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

QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/154 Safari/537.36"));
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

} // namespace

RuTrackerRuSearchClient::RuTrackerRuSearchClient(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
{
    resetCookieJar();
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
    searchPageResolved_ = false;

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

    const QUrl loginUrl(QStringLiteral("https://rutracker.org/forum/login.php"));
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
            const QByteArray raw = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                authenticated_ = false;
                finishNow(generation,
                    tr("RuTracker login request failed: %1").arg(errorText));
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
                QStringLiteral("id=\\\"logged-in-username\\\""),
                Qt::CaseInsensitive);

            if (!hasSessionCookie && !loggedInMarker) {
                authenticated_ = false;
                QString reason = tr("RuTracker authentication failed. Check the "
                                    "login/password; the site may also require a "
                                    "captcha or browser challenge.");
                finishNow(generation, reason);
                return;
            }

            authenticated_ = true;
            fetchSearchPage(generation);
        });
}

void RuTrackerRuSearchClient::fetchSearchPage(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    const QUrl url = RuTrackerRuSource::searchUrl(
        currentQuery_, currentSortKey_, currentContentType_);
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
                finishNow(generation,
                    tr("RuTracker search request failed: %1").arg(errorText));
                return;
            }

            const QString pageText = sourceparse::decodeTrackerText(body);
            if (hasLoginForm(pageText)
                && !pageText.contains(QStringLiteral("id=\\\"tor-tbl\\\""),
                    Qt::CaseInsensitive)) {
                authenticated_ = false;
                if (!authRetried_) {
                    authRetried_ = true;
                    authenticate(generation);
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
                    tr("RuTracker returned no exact torrent rows for this query."));
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

            bool accepted = false;
            if (error == QNetworkReply::NoError
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
