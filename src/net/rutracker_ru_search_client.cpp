#include "net/rutracker_ru_search_client.h"

#include "net/rutracker_ru_source.h"

#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace rats::net {
namespace {

QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 RatsSearch/2"));
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    return request;
}

} // namespace

RuTrackerRuSearchClient::RuTrackerRuSearchClient(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
{
}

RuTrackerRuSearchClient::~RuTrackerRuSearchClient()
{
    cancel();
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
    searchPageResolved_ = false;

    const QList<QNetworkReply*> outstanding = replies_.values();
    replies_.clear();
    for (QNetworkReply* reply : outstanding) {
        if (reply)
            reply->abort();
    }
}

void RuTrackerRuSearchClient::search(
    const QString& query, int limit, const QString& sortKey, const QString& contentType)
{
    cancel();

    currentQuery_ = query.trimmed();
    currentSortKey_ = sortKey;
    currentContentType_ = contentType.trimmed().toLower();
    requestedLimit_ = qBound(1, limit, 50);
    accepted_ = 0;
    rejected_ = 0;
    activeDetails_ = 0;
    searchPageResolved_ = false;
    finishedEmitted_ = false;

    const int generation = generation_;
    if (currentQuery_.isEmpty()) {
        finishNow(generation, tr("Empty RuTracker.RU search query."));
        return;
    }

    qInfo() << "[RuTrackerRuSearchClient] search"
            << currentQuery_.left(80) << "limit" << requestedLimit_;
    fetchSearchPage(generation);
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
            const QByteArray body
                = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("RuTracker.RU search request failed: %1")
                        .arg(errorText));
                return;
            }

            const int candidateCap
                = qMin(100, qMax(requestedLimit_, requestedLimit_ * 3));
            QVector<domain::Torrent> candidates
                = RuTrackerRuSource::parseSearchPage(
                    body, finalUrl, candidateCap);

            if (candidates.isEmpty()) {
                searchPageResolved_ = true;
                finishNow(generation,
                    tr("RuTracker.RU returned no exact torrent rows for this query."));
                return;
            }

            searchPageResolved_ = true;
            for (domain::Torrent& torrent : candidates) {
                if (!currentContentType_.isEmpty()
                    && domain::toString(torrent.contentType).compare(currentContentType_, Qt::CaseInsensitive) != 0) {
                    ++rejected_;
                    continue;
                }
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
            const QByteArray body
                = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            bool accepted = false;
            if (error == QNetworkReply::NoError
                && RuTrackerRuSource::applyDetailPage(
                    job.torrent, body, finalUrl)
                && RuTrackerRuSource::isStrictComplete(job.torrent)
                && accepted_ < requestedLimit_) {
                accepted = true;
                ++accepted_;
                emit resultReady(currentQuery_, job.torrent);
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
