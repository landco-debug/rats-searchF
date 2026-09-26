#include "net/rutor_search_client.h"

#include "net/rutor_source.h"

#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace rats::net {
RutorSearchClient::RutorSearchClient(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
{
}

RutorSearchClient::~RutorSearchClient()
{
    cancel();
}

void RutorSearchClient::cancel()
{
    ++generation_;
    finishedEmitted_ = true;
    currentQuery_.clear();
    currentSortKey_.clear();
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

void RutorSearchClient::search(
    const QString& query, int limit, const QString& sortKey)
{
    cancel();

    currentQuery_ = query.trimmed();
    currentSortKey_ = sortKey;
    requestedLimit_ = qBound(1, limit, 50);
    accepted_ = 0;
    rejected_ = 0;
    activeDetails_ = 0;
    searchPageResolved_ = false;
    finishedEmitted_ = false;

    const int generation = generation_;
    if (currentQuery_.isEmpty()) {
        finishNow(generation, tr("Empty Rutor search query."));
        return;
    }

    qInfo() << "[RutorSearchClient] search" << currentQuery_.left(80)
            << "limit" << requestedLimit_;
    fetchSearchPage(
        RutorSource::searchUrl(currentQuery_, currentSortKey_),
        generation, false);
}

QUrl RutorSearchClient::alternateMirror(const QUrl& url)
{
    QUrl mirror(url);
    if (url.host().compare(QStringLiteral("rutor.info"), Qt::CaseInsensitive) == 0)
        mirror.setHost(QStringLiteral("rutor.is"));
    else
        mirror.setHost(QStringLiteral("rutor.info"));
    return mirror;
}

void RutorSearchClient::fetchSearchPage(
    const QUrl& url, int generation, bool mirrorRetried)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 RatsSearch/2"));
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);
    replies_.insert(reply);

    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, mirrorRetried, url]() {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body
                = error == QNetworkReply::NoError ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            const int candidateCap = qMin(100, qMax(requestedLimit_, requestedLimit_ * 3));
            QVector<domain::Torrent> candidates;
            if (error == QNetworkReply::NoError)
                candidates = RutorSource::parseSearchPage(body, finalUrl, candidateCap);

            if ((error != QNetworkReply::NoError || candidates.isEmpty())
                && !mirrorRetried) {
                qInfo() << "[RutorSearchClient] primary search failed/empty;"
                           " retrying mirror";
                fetchSearchPage(alternateMirror(url), generation, true);
                return;
            }

            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("Rutor search request failed: %1").arg(errorText));
                return;
            }

            if (candidates.isEmpty()) {
                searchPageResolved_ = true;
                finishNow(generation,
                    tr("Rutor returned no exact torrent rows for this query."));
                return;
            }

            searchPageResolved_ = true;
            for (domain::Torrent& torrent : candidates) {
                DetailJob job;
                job.url = QUrl(
                    torrent.info.value(QStringLiteral("sourceUrl")).toString());
                job.torrent = std::move(torrent);
                if (job.url.isValid())
                    detailQueue_.enqueue(std::move(job));
                else
                    ++rejected_;
            }

            processQueue(generation);
        });
}

void RutorSearchClient::processQueue(int generation)
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

void RutorSearchClient::fetchDetail(DetailJob job, int generation)
{
    QNetworkRequest request(job.url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 RatsSearch/2"));
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);
    replies_.insert(reply);

    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const QUrl finalUrl = reply->url();
            const QByteArray body
                = error == QNetworkReply::NoError ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError && !job.mirrorRetried) {
                job.url = alternateMirror(job.url);
                job.mirrorRetried = true;
                --activeDetails_;
                detailQueue_.prepend(std::move(job));
                processQueue(generation);
                return;
            }

            bool accepted = false;
            if (error == QNetworkReply::NoError
                && RutorSource::applyDetailPage(job.torrent, body, finalUrl)
                && RutorSource::isStrictComplete(job.torrent)
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

void RutorSearchClient::finishIfIdle(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!searchPageResolved_ || activeDetails_ != 0 || !detailQueue_.isEmpty())
        return;
    finishNow(generation);
}

void RutorSearchClient::finishNow(int generation, const QString& error)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    finishedEmitted_ = true;
    qInfo() << "[RutorSearchClient] finished"
            << "accepted" << accepted_ << "rejected" << rejected_
            << (error.isEmpty() ? QString() : error);

    emit searchFinished(currentQuery_, accepted_, rejected_, error);
}

} // namespace rats::net
