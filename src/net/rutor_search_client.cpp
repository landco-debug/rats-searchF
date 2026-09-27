#include "net/rutor_search_client.h"

#include "net/rutor_source.h"
#include "domain/content_classifier.h"
#include "net/torrent_engine.h"

#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>

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

void RutorSearchClient::search(
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
        DetailJob job=detailQueue_.dequeue();
        ++activeDetails_;
        if(!currentContentType_.isEmpty()){
            const QUrl torrentUrl(job.torrent.info.value(QStringLiteral("sourceTorrentUrl")).toString());
            if(!torrentUrl.isValid()){ ++rejected_; --activeDetails_; continue; }
            fetchTypeProbe(std::move(job),generation,torrentUrl);
        } else {
            fetchDetail(std::move(job),generation);
        }
    }

    if (accepted_ >= requestedLimit_)
        detailQueue_.clear();

    finishIfIdle(generation);
}

void RutorSearchClient::fetchTypeProbe(DetailJob job, int generation, const QUrl& torrentUrl, bool mirrorRetried)
{
    QNetworkRequest request(torrentUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader,QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 RatsSearch/2"));
    request.setRawHeader("Accept","application/x-bittorrent,application/octet-stream,*/*;q=0.5");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply* reply=networkManager_->get(request); replies_.insert(reply);
    connect(reply,&QNetworkReply::finished,this,[this,reply,generation,job=std::move(job),torrentUrl,mirrorRetried]() mutable {
        replies_.remove(reply);
        const auto error=reply->error();
        const QByteArray body=error==QNetworkReply::NoError?reply->readAll():QByteArray();
        reply->deleteLater();
        if(generation!=generation_ || finishedEmitted_) return;
        if(error!=QNetworkReply::NoError && !mirrorRetried){ fetchTypeProbe(std::move(job),generation,alternateMirror(torrentUrl),true); return; }
        bool matched=false;
        constexpr int kMaxTorrentMetadataBytes=32*1024*1024;
        if(error==QNetworkReply::NoError && !body.isEmpty() && body.size()<=kMaxTorrentMetadataBytes){
            QTemporaryFile temp;
            if(temp.open() && temp.write(body)==body.size()){
                const QString path=temp.fileName(); temp.flush(); temp.close();
                TorrentEngine parser(nullptr);
                const TorrentMetadata metadata=parser.readTorrentFile(path);
                if(metadata.valid && metadata.hash.compare(job.torrent.hash,Qt::CaseInsensitive)==0){
                    QVector<domain::File> files; files.reserve(metadata.files.size());
                    for(const EngineFile& source:metadata.files) files.append(domain::File{source.path,source.size});
                    const domain::Classification c=domain::ContentClassifier::classify(job.torrent.name,files);
                    job.torrent.contentType=c.type; job.torrent.contentCategory=c.category;
                    job.torrent.fileList=files; job.torrent.files=files.size();
                    if(job.torrent.size<=0) job.torrent.size=metadata.totalSize;
                    matched=domain::toString(job.torrent.contentType).compare(currentContentType_,Qt::CaseInsensitive)==0;
                }
            }
        }
        if(matched){ fetchDetail(std::move(job),generation); return; }
        ++rejected_; --activeDetails_; processQueue(generation);
    });
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
