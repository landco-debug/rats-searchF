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

QNetworkRequest requestFor(
    const QUrl& url, bool torrent = false, const QUrl& referer = {})
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/154 Safari/537.36 RatsSearch/2"));
    request.setRawHeader("Accept", torrent
        ? "application/x-bittorrent,application/octet-stream,*/*;q=0.5"
        : "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setRawHeader("Referer",
        (referer.isValid()
                ? referer
                : QUrl(QStringLiteral("https://megapeer.vip/browse.php")))
            .toEncoded());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    return request;
}

bool looksLikeBrowserChallenge(const QByteArray& body)
{
    if (body.isEmpty())
        return false;
    const QString text = QString::fromUtf8(body.left(256 * 1024)).toLower();
    return text.contains(QStringLiteral("<title>just a moment"))
        || text.contains(QStringLiteral("cf-browser-verification"))
        || text.contains(QStringLiteral("cf_chl_"))
        || text.contains(QStringLiteral("cf-chl-"))
        || text.contains(QStringLiteral("attention required"))
        || (text.contains(QStringLiteral("cloudflare"))
            && text.contains(QStringLiteral("challenge")));
}

bool looksLikeHtml(const QByteArray& body)
{
    const QByteArray head = body.left(4096).trimmed().toLower();
    return head.startsWith("<!doctype html")
        || head.startsWith("<html")
        || head.contains("<head")
        || head.contains("<body");
}

QString networkFailureText(
    const QString& context, int status, const QString& error)
{
    QString detail = error.trimmed();
    if (status > 0)
        detail = QStringLiteral("HTTP %1%2")
                     .arg(status)
                     .arg(detail.isEmpty()
                             ? QString()
                             : QStringLiteral(" · ") + detail);
    if (detail.isEmpty())
        detail = QStringLiteral("request failed");
    return context + QStringLiteral(": ") + detail;
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
    searchResolved_ = false;
    networkIssues_.clear();

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
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body = reply->readAll();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;
            if (error != QNetworkReply::NoError) {
                finishNow(generation,
                    tr("MegaPeer search request failed: %1")
                        .arg(networkFailureText(
                            QStringLiteral("search"), status, errorText)));
                return;
            }
            if (looksLikeBrowserChallenge(body)) {
                finishNow(generation,
                    tr("MegaPeer search was blocked by a browser/Cloudflare challenge."));
                return;
            }

            QVector<domain::Torrent> candidates
                = MegaPeerSource::parseSearchPage(body, finalUrl,
                    qMin(80, qMax(requestedLimit_, requestedLimit_ * 2)));
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
                if (job.detailUrl.isValid())
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
        fetchDetail(std::move(job), generation);
    }
    if (accepted_ >= requestedLimit_)
        queue_.clear();
    finishIfIdle(generation);
}

void MegaPeerSearchClient::fetchDetail(Job job, int generation)
{
    QNetworkReply* reply = networkManager_->get(
        requestFor(job.detailUrl, false,
            QUrl(QStringLiteral("https://megapeer.vip/browse.php"))));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const auto error = reply->error();
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errorText = reply->errorString();
            const QUrl finalUrl = reply->url();
            const QByteArray body = reply->readAll();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                recordNetworkIssue(networkFailureText(
                    QStringLiteral("detail"), status, errorText));
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }
            if (looksLikeBrowserChallenge(body)) {
                recordNetworkIssue(
                    tr("detail: browser/Cloudflare challenge"));
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }

            job.detailBody = body;
            job.detailFinalUrl = finalUrl;

            const bool applied = MegaPeerSource::applyDetailPage(
                job.torrent, job.detailBody, job.detailFinalUrl);
            if (!applied) {
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }

            job.torrentUrl = QUrl(job.torrent.info
                .value(QStringLiteral("sourceTorrentUrl")).toString());

            const bool strict
                = MegaPeerSource::isStrictComplete(job.torrent);
            const bool needsIdentity
                = !job.torrent.info
                       .value(QStringLiteral("sourceVerified")).toBool()
                || !job.torrent.isValid();
            const bool needsClassification
                = job.torrent.contentType == domain::ContentType::Unknown;

            if (!strict
                && (needsIdentity || needsClassification)
                && job.torrentUrl.isValid()) {
                // Only exceptional candidates pay for .torrent metadata:
                // no magnet on the exact page, or classification cannot be
                // proven from the page itself.
                fetchTorrentFallback(std::move(job), generation);
                return;
            }

            finishCandidate(std::move(job), generation, true);
        });
}

void MegaPeerSearchClient::fetchTorrentFallback(Job job, int generation)
{
    QNetworkReply* reply = networkManager_->get(
        requestFor(job.torrentUrl, true, job.detailUrl));
    replies_.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, job = std::move(job)]() mutable {
            replies_.remove(reply);
            const auto error = reply->error();
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errorText = reply->errorString();
            const QByteArray body = reply->readAll();
            reply->deleteLater();

            if (generation != generation_ || finishedEmitted_)
                return;

            if (error != QNetworkReply::NoError) {
                recordNetworkIssue(networkFailureText(
                    QStringLiteral("torrent fallback"), status, errorText));
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }
            if (looksLikeBrowserChallenge(body)) {
                recordNetworkIssue(
                    tr("torrent fallback: browser/Cloudflare challenge"));
                ++rejected_;
                --active_;
                processQueue(generation);
                return;
            }

            bool parsed = false;
            constexpr int kMaxTorrentBytes = 32 * 1024 * 1024;
            if (!body.isEmpty() && body.size() <= kMaxTorrentBytes) {
                QTemporaryFile temp(
                    QDir::tempPath()
                    + QStringLiteral("/rats-megapeer-XXXXXX.torrent"));
                if (temp.open() && temp.write(body) == body.size()) {
                    temp.flush();
                    temp.close();
                    TorrentEngine parser(nullptr);
                    const TorrentMetadata metadata
                        = parser.readTorrentFile(temp.fileName());
                    if (metadata.valid && !metadata.hash.isEmpty()) {
                        const QString metadataHash
                            = metadata.hash.toLower();
                        if (job.torrent.isValid()
                            && job.torrent.hash.compare(
                                   metadataHash, Qt::CaseInsensitive)
                                != 0) {
                            ++rejected_;
                            --active_;
                            processQueue(generation);
                            return;
                        }

                        job.torrent.hash = metadataHash;
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
                        if (c.type != domain::ContentType::Unknown) {
                            job.torrent.contentType = c.type;
                            job.torrent.contentCategory = c.category;
                            job.torrent.info[
                                QStringLiteral("contentTypeEvidence")]
                                = QStringLiteral("torrent-files");
                        }
                        parsed = job.torrent.isValid();
                    }
                }
            }

            if (!parsed) {
                if (looksLikeHtml(body)) {
                    recordNetworkIssue(
                        tr("torrent fallback: tracker returned HTML instead of .torrent metadata"));
                }
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

void MegaPeerSearchClient::recordNetworkIssue(const QString& issue)
{
    const QString clean = issue.trimmed();
    if (clean.isEmpty() || networkIssues_.contains(clean))
        return;
    if (networkIssues_.size() < 4)
        networkIssues_.append(clean);
}

void MegaPeerSearchClient::finishIfIdle(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!searchResolved_ || active_ != 0 || !queue_.isEmpty())
        return;

    if (!networkIssues_.isEmpty()) {
        const QString issues = networkIssues_.join(QStringLiteral(" · "));
        finishNow(generation,
            accepted_ > 0
                ? tr("MegaPeer returned partial results; source/network issues: %1")
                      .arg(issues)
                : tr("MegaPeer produced no verified results; source/network issues: %1")
                      .arg(issues));
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
