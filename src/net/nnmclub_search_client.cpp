#include "net/nnmclub_search_client.h"

#include "domain/content_classifier.h"
#include "net/nnmclub_source.h"
#include "net/torrent_engine.h"
#include "net/source_parse_utils.h"

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
    request.setRawHeader("Referer", "https://nnmclub.to/forum/tracker.php");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    return request;
}

} // namespace

NnmClubSearchClient::NnmClubSearchClient(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
{
}

NnmClubSearchClient::~NnmClubSearchClient()
{
    cancel();
}

void NnmClubSearchClient::cancel()
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
    searchResolved_ = false;

    const QList<QNetworkReply*> outstanding = replies_.values();
    replies_.clear();
    for (QNetworkReply* reply : outstanding) {
        if (reply)
            reply->abort();
    }
}

void NnmClubSearchClient::search(
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
        finishNow(generation, tr("Empty NNM-Club search query."));
        return;
    }
    fetchSearchPage(generation);
}

void NnmClubSearchClient::fetchSearchPage(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    const QUrl url = NnmClubSource::searchUrl();
    QNetworkRequest request = requestFor(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/x-www-form-urlencoded"));
    const QByteArray body
        = NnmClubSource::searchBody(currentQuery_, currentSortKey_);

    QNetworkReply* reply = networkManager_->post(request, body);
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation]() {
            replies_.remove(reply);
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray response = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;
            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("NNM-Club search request failed: %1").arg(errorText));
                return;
            }

            QVector<domain::Torrent> candidates
                = NnmClubSource::parseSearchPage(response, finalUrl,
                    qMin(120, qMax(requestedLimit_, requestedLimit_ * 3)));

            // Typed searches used to verify every row in tracker order. For
            // Books in particular that could mean dozens of unrelated
            // .torrent+detail requests before an obvious PDF/FB2/EPUB release.
            // Reorder only; exact admission is unchanged.
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

void NnmClubSearchClient::processQueue(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    while (active_ < kMaxConcurrent
        && !queue_.isEmpty()
        && accepted_ < requestedLimit_) {
        Job job = queue_.dequeue();
        ++active_;
        fetchTorrent(std::move(job), generation);
    }
    if (accepted_ >= requestedLimit_)
        queue_.clear();
    finishIfIdle(generation);
}

void NnmClubSearchClient::fetchTorrent(Job job, int generation)
{
    QNetworkReply* reply = networkManager_->get(requestFor(job.torrentUrl, true));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const auto error = reply->error();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            bool parsed = false;
            constexpr int kMaxTorrentBytes = 32 * 1024 * 1024;
            if (error == QNetworkReply::NoError
                && !body.isEmpty() && body.size() <= kMaxTorrentBytes) {
                QTemporaryFile temp(
                    QDir::tempPath() + QStringLiteral("/rats-nnmclub-XXXXXX.torrent"));
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
                        // Prefer a recognized tracker-native forum category
                        // over extension voting (important for ISO/DVD images);
                        // otherwise the real .torrent file list is authoritative.
                        if (job.torrent.contentType == domain::ContentType::Unknown) {
                            job.torrent.contentType = c.type;
                            job.torrent.contentCategory = c.category;
                            if (c.type != domain::ContentType::Unknown) {
                                job.torrent.info[
                                    QStringLiteral("contentTypeEvidence")]
                                    = QStringLiteral("torrent-files");
                            }
                        } else if (job.torrent.contentCategory
                            == domain::ContentCategory::Unknown) {
                            job.torrent.contentCategory = c.category;
                        }
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
            fetchDetail(std::move(job), generation);
        });
}

void NnmClubSearchClient::fetchDetail(Job job, int generation)
{
    QNetworkReply* reply = networkManager_->get(requestFor(job.detailUrl));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const auto error = reply->error();
            const QUrl finalUrl = reply->url();
            const QByteArray body = error == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            bool accepted = false;
            if (error == QNetworkReply::NoError
                && NnmClubSource::applyDetailPage(
                    job.torrent, body, finalUrl)
                && NnmClubSource::isStrictComplete(job.torrent)) {
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
        });
}

void NnmClubSearchClient::finishIfIdle(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!searchResolved_ || active_ != 0 || !queue_.isEmpty())
        return;
    finishNow(generation);
}

void NnmClubSearchClient::finishNow(int generation, const QString& error)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    finishedEmitted_ = true;
    emit searchFinished(currentQuery_, accepted_, rejected_, error);
}

} // namespace rats::net
