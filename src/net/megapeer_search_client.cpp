#include "net/megapeer_search_client.h"

#include "domain/content_classifier.h"
#include "net/megapeer_source.h"
#include "net/source_parse_utils.h"
#include "net/torrent_engine.h"

#include <algorithm>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>

namespace rats::net {
namespace {

QNetworkRequest requestFor(const QUrl& url, bool torrent = false)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 RatsSearch/2"));
    request.setRawHeader("Accept", torrent
        ? "application/x-bittorrent,application/octet-stream,*/*;q=0.5"
        : "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setRawHeader("Referer", "https://megapeer.vip/browse.php");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    return request;
}

} // namespace

MegaPeerSearchClient::MegaPeerSearchClient(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
{
}

MegaPeerSearchClient::~MegaPeerSearchClient()
{
    cancel();
}

void MegaPeerSearchClient::cancel()
{
    ++generation_;
    finishedEmitted_ = true;
    currentQuery_.clear();
    currentSortKey_.clear();
    currentContentType_.clear();
    queue_.clear();
    active_ = 0;
    accepted_ = 0;
    rejected_ = 0;
    networkFailures_ = 0;
    lastNetworkError_.clear();
    searchResolved_ = false;

    const QList<QNetworkReply*> outstanding = replies_.values();
    replies_.clear();
    for (QNetworkReply* reply : outstanding) {
        if (reply)
            reply->abort();
    }
}

void MegaPeerSearchClient::search(
    const QString& query, int limit, const QString& sortKey,
    const QString& contentType)
{
    cancel();

    currentQuery_ = query.trimmed();
    currentSortKey_ = sortKey;
    currentContentType_ = contentType.trimmed().toLower();
    requestedLimit_ = qBound(1, limit, 50);
    finishedEmitted_ = false;

    const int generation = generation_;
    if (currentQuery_.isEmpty()) {
        finishNow(generation, tr("Empty MegaPeer search query."));
        return;
    }
    fetchSearchPage(generation);
}

void MegaPeerSearchClient::fetchSearchPage(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    const QUrl url = MegaPeerSource::searchUrl(currentQuery_, currentSortKey_);
    QNetworkReply* reply = networkManager_->get(requestFor(url));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation]() {
            replies_.remove(reply);
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;
            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("MegaPeer search request failed: %1").arg(errorText));
                return;
            }

            QVector<domain::Torrent> candidates
                = MegaPeerSource::parseSearchPage(body, finalUrl,
                    qMin(120, qMax(requestedLimit_, requestedLimit_ * 3)));
            std::stable_sort(candidates.begin(), candidates.end(),
                [this](const domain::Torrent& a, const domain::Torrent& b) {
                    return sourceparse::contentTypeHintScore(
                               a, currentContentType_)
                        > sourceparse::contentTypeHintScore(
                               b, currentContentType_);
                });

            searchResolved_ = true;
            for (const domain::Torrent& torrent : candidates) {
                Job job;
                job.torrent = torrent;
                job.detailUrl = QUrl(torrent.info
                    .value(QStringLiteral("sourceUrl")).toString());
                job.torrentUrl = QUrl(torrent.info
                    .value(QStringLiteral("sourceTorrentUrl")).toString());
                if (job.detailUrl.isValid() && job.torrentUrl.isValid())
                    queue_.enqueue(std::move(job));
                else
                    ++rejected_;
            }
            processQueue(generation);
        });
}

void MegaPeerSearchClient::processQueue(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    while (active_ < kMaxConcurrent
        && !queue_.isEmpty()
        && accepted_ < requestedLimit_) {
        Job job = queue_.dequeue();
        ++active_;
        // Detail first: current MegaPeer pages expose the exact magnet, so the
        // normal path is one page request instead of .torrent + page.
        fetchDetail(std::move(job), generation);
    }
    if (accepted_ >= requestedLimit_)
        queue_.clear();
    finishIfIdle(generation);
}

void MegaPeerSearchClient::fetchDetail(Job job, int generation)
{
    QNetworkReply* reply = networkManager_->get(requestFor(job.detailUrl));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                recordNetworkFailure(QStringLiteral("detail"), errorText);
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }

            job.detailBody = body;
            job.detailFinalUrl = finalUrl;

            if (MegaPeerSource::applyDetailPage(
                    job.torrent, job.detailBody, job.detailFinalUrl)) {
                finishCandidate(std::move(job), generation, true);
                return;
            }

            // The exact page may occasionally omit a magnet. Only then pay for
            // the paired .torrent; reuse the already-fetched detail body after
            // parsing it instead of issuing a second detail request.
            fetchTorrentFallback(std::move(job), generation);
        });
}

void MegaPeerSearchClient::fetchTorrentFallback(Job job, int generation)
{
    QNetworkReply* reply = networkManager_->get(requestFor(job.torrentUrl, true));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                recordNetworkFailure(QStringLiteral("torrent fallback"), errorText);
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }

            bool parsed = false;
            constexpr int kMaxTorrentBytes = 32 * 1024 * 1024;
            if (!body.isEmpty() && body.size() <= kMaxTorrentBytes) {
                QTemporaryFile temp(
                    QDir::tempPath() + QStringLiteral("/rats-megapeer-XXXXXX.torrent"));
                if (temp.open() && temp.write(body) == body.size()) {
                    temp.flush();
                    temp.close();
                    TorrentEngine parser(nullptr);
                    const TorrentMetadata metadata
                        = parser.readTorrentFile(temp.fileName());
                    if (metadata.valid && !metadata.hash.isEmpty()) {
                        job.torrent.hash = metadata.hash.toLower();
                        if (job.torrent.name.isEmpty())
                            job.torrent.name = metadata.name;
                        if (job.torrent.size <= 0)
                            job.torrent.size = metadata.totalSize;

                        QVector<domain::File> files;
                        files.reserve(metadata.files.size());
                        for (const EngineFile& f : metadata.files)
                            files.append(domain::File { f.path, f.size });
                        job.torrent.fileList = files;
                        job.torrent.files = files.size();

                        const domain::Classification c
                            = domain::ContentClassifier::classify(
                                job.torrent.name, files);
                        job.torrent.contentType = c.type;
                        job.torrent.contentCategory = c.category;
                        job.torrent.info[QStringLiteral("contentTypeEvidence")]
                            = QStringLiteral("torrent-files");
                        parsed = job.torrent.isValid();
                    }
                }
            }

            if (!parsed) {
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }

            const bool applied = MegaPeerSource::applyDetailPage(
                job.torrent, job.detailBody, job.detailFinalUrl);
            finishCandidate(std::move(job), generation, applied);
        });
}

void MegaPeerSearchClient::finishCandidate(
    Job job, int generation, bool detailApplied)
{
    bool accepted = false;
    if (detailApplied && MegaPeerSource::isStrictComplete(job.torrent)) {
        const bool typeMatches = currentContentType_.isEmpty()
            || domain::toString(job.torrent.contentType)
                   .compare(currentContentType_, Qt::CaseInsensitive) == 0;
        if (typeMatches && accepted_ < requestedLimit_) {
            accepted = true;
            ++accepted_;
            emit resultReady(currentQuery_, job.torrent);
        }
    }

    if (!accepted)
        ++rejected_;
    --active_;
    processQueue(generation);
}

void MegaPeerSearchClient::recordNetworkFailure(
    const QString& context, const QString& error)
{
    ++networkFailures_;
    lastNetworkError_ = context + QStringLiteral(": ") + error;
}

void MegaPeerSearchClient::finishIfIdle(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!searchResolved_ || active_ != 0 || !queue_.isEmpty())
        return;

    if (accepted_ == 0 && networkFailures_ > 0) {
        finishNow(generation,
            tr("MegaPeer produced no verified results; %1 network request(s) "
               "failed. Last error: %2")
                .arg(networkFailures_)
                .arg(lastNetworkError_));
        return;
    }
    finishNow(generation);
}

void MegaPeerSearchClient::finishNow(int generation, const QString& error)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    finishedEmitted_ = true;
    emit searchFinished(currentQuery_, accepted_, rejected_, error);
}

} // namespace rats::net
