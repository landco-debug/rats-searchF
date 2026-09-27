#include "net/rutor_search_client.h"

#include "net/rutor_source.h"
#include "domain/content_classifier.h"
#include "net/torrent_engine.h"
#include "net/source_parse_utils.h"

#include <algorithm>
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
    queuedSearchHashes_.clear();
    currentCategory_ = 0;
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
    // Rutor's search form exposes three coarse buckets: Movies, TV and Other.
    // Music/games/software/books live under Other. Video stays on All because it
    // spans both Movies and TV.
    currentCategory_ = (!currentContentType_.isEmpty()
                           && currentContentType_ != QStringLiteral("video"))
        ? 3
        : 0;
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
        RutorSource::searchUrl(
            currentQuery_, currentSortKey_, 0, currentCategory_),
        generation, false, 0);
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
    const QUrl& url, int generation, bool mirrorRetried, int page)
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
        [this, reply, generation, mirrorRetried, url, page]() {
            replies_.remove(reply);

            const QNetworkReply::NetworkError error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body
                = error == QNetworkReply::NoError ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            const int candidateCap = 200;
            QVector<domain::Torrent> candidates;
            if (error == QNetworkReply::NoError)
                candidates = RutorSource::parseSearchPage(body, finalUrl, candidateCap);

            if ((error != QNetworkReply::NoError || candidates.isEmpty())
                && !mirrorRetried) {
                qInfo() << "[RutorSearchClient] primary search failed/empty;"
                           " retrying mirror";
                fetchSearchPage(
                    alternateMirror(url), generation, true, page);
                return;
            }

            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("Rutor search request failed: %1").arg(errorText));
                return;
            }

            if (candidates.isEmpty()) {
                searchPageResolved_ = true;
                if (page == 0 && detailQueue_.isEmpty() && activeDetails_ == 0) {
                    finishNow(generation,
                        tr("Rutor returned no exact torrent rows for this query."));
                    return;
                }
                processQueue(generation);
                return;
            }

            std::stable_sort(candidates.begin(), candidates.end(),
                [this](const domain::Torrent& a, const domain::Torrent& b) {
                    return sourceparse::contentTypeHintScore(
                               a, currentContentType_)
                        > sourceparse::contentTypeHintScore(
                               b, currentContentType_);
                });

            for (domain::Torrent& torrent : candidates) {
                if (sourceparse::hasAuthoritativeTypeMismatch(
                        torrent, currentContentType_)) {
                    ++rejected_;
                    continue;
                }
                if (queuedSearchHashes_.contains(torrent.hash))
                    continue;
                queuedSearchHashes_.insert(torrent.hash);

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

            if (page + 1 < kMaxSearchPages
                && accepted_ < requestedLimit_) {
                fetchSearchPage(
                    RutorSource::searchUrl(currentQuery_, currentSortKey_,
                        page + 1, currentCategory_),
                    generation, false, page + 1);
                return;
            }

            searchPageResolved_ = true;
            finishIfIdle(generation);
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
                    job.torrent.contentType = c.type;
                    job.torrent.contentCategory = c.category;
                    job.torrent.fileList = files;
                    job.torrent.files = files.size();
                    if (job.torrent.size <= 0)
                        job.torrent.size = metadata.totalSize;
                    if (job.torrent.contentType != domain::ContentType::Unknown)
                        job.torrent.info[QStringLiteral("contentTypeEvidence")]
                            = QStringLiteral("torrent-files");
                    matched = domain::toString(job.torrent.contentType)
                                  .compare(currentContentType_,
                                      Qt::CaseInsensitive)
                        == 0;
                }
            }
        }

        if (matched
            && RutorSource::isStrictComplete(job.torrent)
            && accepted_ < requestedLimit_) {
            ++accepted_;
            emit resultReady(currentQuery_, job.torrent);
        } else {
            ++rejected_;
        }

        --activeDetails_;
        processQueue(generation);
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

                if (currentContentType_.isEmpty()) {
                    accepted = true;
                } else if (job.torrent.contentType != domain::ContentType::Unknown) {
                    accepted = domain::toString(job.torrent.contentType)
                                   .compare(currentContentType_,
                                       Qt::CaseInsensitive)
                        == 0;
                } else {
                    // The exact page did not expose a category we recognise.
                    // Fall back to the exact .torrent's real file list instead
                    // of guessing from the release title.
                    const QUrl torrentUrl(job.torrent.info
                        .value(QStringLiteral("sourceTorrentUrl")).toString());
                    if (torrentUrl.isValid()) {
                        fetchTypeProbe(
                            std::move(job), generation, torrentUrl);
                        return; // same concurrency slot remains active
                    }
                }
            }

            if (accepted) {
                ++accepted_;
                emit resultReady(currentQuery_, job.torrent);
            } else {
                ++rejected_;
            }

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
