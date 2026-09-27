#include "net/kinozal_search_client.h"

#include "net/kinozal_source.h"
#include "net/source_parse_utils.h"

#include <algorithm>
#include <QDebug>
#include <QUrlQuery>

namespace rats::net {

KinozalSearchClient::KinozalSearchClient(QObject* parent)
    : QObject(parent)
{
#ifdef __APPLE__
    browser_ = std::make_unique<KinozalBrowser>();
#endif
}

KinozalSearchClient::~KinozalSearchClient()
{
    cancel();
}

bool KinozalSearchClient::isConfigured() const
{
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

void KinozalSearchClient::cancel()
{
    ++generation_;
    finishedEmitted_ = true;
    currentQuery_.clear();
    currentSortKey_.clear();
    currentContentType_.clear();
    queue_.clear();
    accepted_ = 0;
    rejected_ = 0;
    active_ = 0;
    requestFailures_ = 0;
    mirrorIndex_ = 0;
    searchResolved_ = false;
    lastError_.clear();
#ifdef __APPLE__
    if (browser_)
        browser_->cancel();
#endif
}

#ifdef __APPLE__
void KinozalSearchClient::startAuthorizationAtMirror(
    int mirror, int generation)
{
    if (generation != generation_ || mirror < 0
        || mirror >= mirrorBaseUrls_.size()) {
        return;
    }

    QUrl loginUrl(mirrorBaseUrls_.at(mirror));
    loginUrl.setPath(QStringLiteral("/login.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("m"), QStringLiteral("5"));
    loginUrl.setQuery(query);

    emit browserAuthorizationChanged(false,
        tr("Complete authorization in the Kinozal window (%1).")
            .arg(loginUrl.host()));

    browser_->authorize(loginUrl,
        [this, mirror, generation](
            const QByteArray&, const QUrl&, const QString& error) {
            if (generation != generation_)
                return;

            if (!error.isEmpty()) {
                const int next = mirror + 1;
                if (next < mirrorBaseUrls_.size()) {
                    startAuthorizationAtMirror(next, generation);
                    return;
                }
                emit browserAuthorizationChanged(false,
                    tr("Kinozal authorization failed on all mirrors: %1")
                        .arg(error));
                return;
            }

            mirrorIndex_ = mirror;
            emit browserAuthorizationChanged(true,
                tr("Kinozal browser session is authorized on %1.")
                    .arg(QUrl(mirrorBaseUrls_.at(mirror)).host()));
        });
}

void KinozalSearchClient::reloginInBrowser()
{
    cancel();
    if (!browser_)
        browser_ = std::make_unique<KinozalBrowser>();

    const int generation = generation_;
    emit browserAuthorizationChanged(false,
        tr("Clearing Kinozal browser session…"));
    browser_->clearSession([this, generation]() {
        if (generation != generation_)
            return;
        startAuthorizationAtMirror(0, generation);
    });
}
#endif

void KinozalSearchClient::search(
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
    active_ = 0;
    requestFailures_ = 0;
    mirrorIndex_ = 0;
    searchResolved_ = false;
    finishedEmitted_ = false;

    const int generation = generation_;
    if (currentQuery_.isEmpty()) {
        finishNow(generation, tr("Empty Kinozal search query."));
        return;
    }

#ifndef __APPLE__
    finishNow(generation,
        tr("Kinozal exact-source browser integration is available on macOS."));
    return;
#else
    if (!browser_)
        browser_ = std::make_unique<KinozalBrowser>();
    fetchSearchPage(generation);
#endif
}

void KinozalSearchClient::fetchSearchPage(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

#ifndef __APPLE__
    Q_UNUSED(generation);
#else
    const QUrl base(mirrorBaseUrls_.at(mirrorIndex_));
    const QUrl url
        = KinozalSource::searchUrl(base, currentQuery_, currentSortKey_);
    if (!url.isValid()) {
        finishNow(generation, tr("Cannot build Kinozal search URL."));
        return;
    }

    browser_->get(url,
        [this, generation](
            const QByteArray& body, const QUrl& finalUrl,
            const QString& error) {
            if (generation != generation_ || finishedEmitted_)
                return;

            if (!error.isEmpty()) {
                tryNextMirror(generation,
                    tr("%1 browser search failed: %2")
                        .arg(QUrl(mirrorBaseUrls_.at(mirrorIndex_)).host(),
                            error));
                return;
            }

            QVector<domain::Torrent> candidates
                = KinozalSource::parseSearchPage(
                    body, finalUrl,
                    qMin(120, qMax(requestedLimit_,
                                  requestedLimit_ * 3)));

            std::stable_sort(
                candidates.begin(), candidates.end(),
                [this](const domain::Torrent& a,
                    const domain::Torrent& b) {
                    return sourceparse::contentTypeHintScore(
                               a, currentContentType_)
                        > sourceparse::contentTypeHintScore(
                               b, currentContentType_);
                });

            searchResolved_ = true;
            emit browserAuthorizationChanged(true,
                tr("Kinozal browser session is active on %1.")
                    .arg(finalUrl.host()));

            for (const domain::Torrent& torrent : candidates) {
                Job job;
                job.torrent = torrent;
                job.detailUrl = QUrl(torrent.info
                    .value(QStringLiteral("sourceUrl")).toString());
                if (job.detailUrl.isValid())
                    queue_.enqueue(std::move(job));
                else
                    ++rejected_;
            }

            processNext(generation);
        });
#endif
}

void KinozalSearchClient::processNext(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    if (accepted_ >= requestedLimit_)
        queue_.clear();

    if (active_ == 0 && !queue_.isEmpty()) {
        Job job = queue_.dequeue();
        active_ = 1;
        fetchDetail(std::move(job), generation);
        return;
    }

    finishIfIdle(generation);
}

void KinozalSearchClient::fetchDetail(
    Job job, int generation)
{
#ifndef __APPLE__
    Q_UNUSED(job);
    Q_UNUSED(generation);
#else
    browser_->get(job.detailUrl,
        [this, job = std::move(job), generation](
            const QByteArray& body, const QUrl& finalUrl,
            const QString& error) mutable {
            if (generation != generation_ || finishedEmitted_)
                return;

            if (!error.isEmpty()
                || !KinozalSource::applyDetailPage(
                    job.torrent, body, finalUrl)) {
                ++rejected_;
                if (!error.isEmpty()) {
                    ++requestFailures_;
                    lastError_ = tr("detail %1: %2")
                                     .arg(job.detailUrl.toString(), error);
                }
                active_ = 0;
                processNext(generation);
                return;
            }

            job.detailUrl = QUrl(job.torrent.info
                .value(QStringLiteral("sourceUrl")).toString());
            fetchServerDetails(std::move(job), generation);
        });
#endif
}

void KinozalSearchClient::fetchServerDetails(
    Job job, int generation)
{
#ifndef __APPLE__
    Q_UNUSED(job);
    Q_UNUSED(generation);
#else
    const int id = job.torrent.info
                       .value(QStringLiteral("sourceTopicId")).toInt();
    QUrl url = job.detailUrl;
    url.setPath(QStringLiteral("/get_srv_details.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), QString::number(id));
    query.addQueryItem(QStringLiteral("action"), QStringLiteral("2"));
    url.setQuery(query);

    browser_->get(url,
        [this, job = std::move(job), generation](
            const QByteArray& body, const QUrl&,
            const QString& error) mutable {
            if (generation != generation_ || finishedEmitted_)
                return;

            bool accepted = false;
            if (error.isEmpty()
                && KinozalSource::applyServerDetails(
                    job.torrent, body)
                && KinozalSource::isStrictComplete(job.torrent)) {
                const bool typeMatches
                    = currentContentType_.isEmpty()
                    || domain::toString(job.torrent.contentType)
                           .compare(currentContentType_,
                               Qt::CaseInsensitive)
                        == 0;
                if (typeMatches
                    && accepted_ < requestedLimit_) {
                    accepted = true;
                    ++accepted_;
                    emit resultReady(currentQuery_, job.torrent);
                }
            }

            if (!accepted)
                ++rejected_;
            if (!error.isEmpty()) {
                ++requestFailures_;
                lastError_ = tr("get_srv_details: %1").arg(error);
            }

            active_ = 0;
            processNext(generation);
        });
#endif
}

bool KinozalSearchClient::tryNextMirror(
    int generation, const QString& reason)
{
    if (generation != generation_ || finishedEmitted_)
        return false;

    lastError_ = reason;
    const int next = mirrorIndex_ + 1;
    if (next >= mirrorBaseUrls_.size()) {
        finishNow(generation,
            tr("Kinozal failed on all current mirrors. Last error: %1")
                .arg(reason));
        return false;
    }

    mirrorIndex_ = next;
    qInfo() << "[KinozalSearchClient] switching mirror to"
            << mirrorBaseUrls_.at(mirrorIndex_)
            << "after" << reason;
    fetchSearchPage(generation);
    return true;
}

void KinozalSearchClient::finishIfIdle(int generation)
{
    if (generation != generation_ || finishedEmitted_)
        return;
    if (!searchResolved_ || active_ != 0 || !queue_.isEmpty())
        return;

    if (accepted_ == 0 && requestFailures_ > 0) {
        finishNow(generation,
            tr("Kinozal produced no verified results; %1 browser request(s) "
               "failed. Last error: %2")
                .arg(requestFailures_)
                .arg(lastError_));
        return;
    }

    finishNow(generation);
}

void KinozalSearchClient::finishNow(
    int generation, const QString& error)
{
    if (generation != generation_ || finishedEmitted_)
        return;

    finishedEmitted_ = true;
    emit searchFinished(
        currentQuery_, accepted_, rejected_, error);
}

} // namespace rats::net
