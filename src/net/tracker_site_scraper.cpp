#include "net/tracker_site_scraper.h"
#include "net/media_metadata_utils.h"

#include <QDebug>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace rats::net {

namespace {

// User-Agent shared by every request; trackers gate some content on it.
constexpr const char* kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

} // namespace

// The authoritative strategy list. Its size — not a hardcoded constant — is
// what every scrape uses as its pending-result count.
const QVector<TrackerSiteScraper::Strategy> TrackerSiteScraper::kStrategies = {
    &TrackerSiteScraper::scrapeRutracker,
    &TrackerSiteScraper::scrapeNyaa,
    &TrackerSiteScraper::scrape1337x,
    &TrackerSiteScraper::scrapeRutor,
};

// ============================================================================
// Construction
// ============================================================================

TrackerSiteScraper::TrackerSiteScraper(QObject* parent)
    : QObject(parent), networkManager_(new QNetworkAccessManager(this))
{
    queueTimer_ = new QTimer(this);
    queueTimer_->setInterval(kQueuePollIntervalMs);
    connect(queueTimer_, &QTimer::timeout, this, &TrackerSiteScraper::processQueue);
}

TrackerSiteScraper::~TrackerSiteScraper()
{
    stop();
}

// ============================================================================
// Public entry point
// ============================================================================

void TrackerSiteScraper::stop()
{
    stopping_.store(true);

    // Stop draining and drop anything still queued — those scrapes never started,
    // so there is no reply to abort and no slot to free for them.
    if (queueTimer_) {
        queueTimer_->stop();
    }
    {
        QMutexLocker locker(&queueMutex_);
        pendingQueue_.clear();
    }

    // Abort every outstanding reply so none lingers up to the 20 s transfer
    // timeout during shutdown. Each reply's finished() handler still runs (with
    // an error), which cleans up its pending-scrape bookkeeping. Replies are
    // parented to the manager, so findChildren locates them all.
    const QList<QNetworkReply*> replies = networkManager_->findChildren<QNetworkReply*>();
    for (QNetworkReply* reply : replies) {
        reply->abort();
    }
}

void TrackerSiteScraper::scrape(const QString& infoHash, const QString& name)
{
    if (stopping_.load()) {
        emit scrapeFinished(infoHash, false);
        return; // shutting down — accept no new work
    }

    if (infoHash.length() != kInfoHashHexLength) {
        emit scrapeFinished(infoHash, false);
        return;
    }

    // Per-hash cooldown: skip anything scraped inside the cooldown window.
    {
        QMutexLocker locker(&recentChecksMutex_);
        if (recentChecks_.contains(infoHash)) {
            const QDateTime lastCheck = recentChecks_[infoHash];
            if (lastCheck.secsTo(QDateTime::currentDateTime()) < kCooldownSecs) {
                qDebug() << "TrackerSiteScraper: Hash" << infoHash.left(8) << "checked recently, skipping";
                emit scrapeFinished(infoHash, false);
                return;
            }
        }
        recentChecks_[infoHash] = QDateTime::currentDateTime();
    }

    // Enforce the concurrency cap: start now if a slot is free, otherwise queue
    // for the drain timer. The hash is already marked in recentChecks_ above, so
    // it cannot be enqueued twice within the cooldown window.
    {
        QMutexLocker locker(&queueMutex_);
        if (activeRequests_ >= kMaxConcurrent) {
            pendingQueue_.enqueue({ infoHash, name });
            qDebug() << "TrackerSiteScraper: queued" << infoHash.left(8) << "- active:" << activeRequests_
                     << "queued:" << pendingQueue_.size();
            if (queueTimer_ && !queueTimer_->isActive()) {
                queueTimer_->start();
            }
            return;
        }
        activeRequests_++;
    }

    startScrape(infoHash, name);
}

void TrackerSiteScraper::startScrape(const QString& infoHash, const QString& name)
{
    // Register the pending scrape. The number of results we wait on is derived
    // from the strategy list, not a hardcoded STRATEGY_COUNT.
    {
        QMutexLocker locker(&pendingMutex_);
        PendingScrape pending;
        pending.name = name;
        pending.pendingCount = static_cast<int>(kStrategies.size());
        pendingScrapes_[infoHash] = pending;
    }

    qInfo() << "TrackerSiteScraper: Scraping tracker info for" << infoHash.left(16) << name.left(48);

    // Launch every strategy in parallel.
    for (const Strategy strategy : kStrategies) {
        (this->*strategy)(infoHash);
    }
}

void TrackerSiteScraper::processQueue()
{
    if (stopping_.load()) {
        return;
    }

    QMutexLocker locker(&queueMutex_);

    // Drain as many queued scrapes as the concurrency cap allows.
    while (!pendingQueue_.isEmpty() && activeRequests_ < kMaxConcurrent) {
        PendingRequest req = pendingQueue_.dequeue();
        activeRequests_++;

        // Release the lock while kicking off the scrape (it launches network I/O).
        locker.unlock();
        startScrape(req.infoHash, req.name);
        locker.relock();
    }

    if (pendingQueue_.isEmpty() && queueTimer_) {
        queueTimer_->stop();
    }
}

QString TrackerSiteScraper::pendingNameForHash(const QString& hash) const
{
    QMutexLocker locker(&pendingMutex_);
    const auto it = pendingScrapes_.constFind(hash);
    return it == pendingScrapes_.constEnd() ? QString() : it->name;
}

// ============================================================================
// RuTracker strategy
// ============================================================================

void TrackerSiteScraper::scrapeRutracker(const QString& hash)
{
    // RuTracker allows searching by info hash via the ?h= parameter.
    QUrl url(QString("https://rutracker.org/forum/viewtopic.php?h=%1").arg(hash));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();

        TrackerSiteInfo info;
        info.trackerName = "rutracker";

        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray rawData = reply->readAll();
            info = parseRutrackerHtml(rawData);
        } else {
            qDebug() << "TrackerSiteScraper: RuTracker request failed:" << reply->errorString();
        }

        onStrategyComplete(hash, info);
    });
}

TrackerSiteInfo TrackerSiteScraper::parseRutrackerHtml(const QByteArray& rawData)
{
    TrackerSiteInfo info;
    info.trackerName = "rutracker";

    if (rawData.isEmpty()) {
        return info;
    }

    // RuTracker pages are typically windows-1251 encoded. Sniff the head of the
    // document for the charset before deciding how to decode.
    QString html;
    const QString rawPreview = QString::fromLatin1(rawData.left(kEncodingSniffLength));
    if (rawPreview.contains("windows-1251", Qt::CaseInsensitive)
        || rawPreview.contains("charset=windows-1251", Qt::CaseInsensitive)) {
        qDebug() << "TrackerSiteScraper: Windows-1251 detected, decoding";
        html = decodeWindows1251(rawData);
    } else {
        html = QString::fromUtf8(rawData);
    }

    if (html.isEmpty()) {
        return info;
    }

    // Pre-process: add newlines before post-br and post-b (like legacy).
    html.replace(QRegularExpression("<span class=\"post-br\">"), "\n<span class=\"post-br\">");
    html.replace(QRegularExpression("><span class=\"post-b\">"), ">\n<span class=\"post-b\">");

    // Extract topic title: <a id="topic-title" ...>TITLE</a>
    // Also handles: <a ... id="topic-title" ...>TITLE</a>
    {
        QRegularExpression re(
            R"(<a[^>]*id\s*=\s*"topic-title"[^>]*>(.*?)</a>)", QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.name = stripHtml(match.captured(1)).trimmed();
        }
    }

    // If no topic title found, the page didn't match — return empty.
    if (info.name.isEmpty()) {
        return info;
    }

    // Extract poster image:
    // <var class="postImgAligned" title="URL"> or <var class="postImg"
    // title="URL"> Also: <img class="postImgAligned" title="URL"> or src="URL"
    {
        QRegularExpression re(R"re(<(?:var|img)[^>]*class\s*=\s*"postImg(?:Aligned)?"[^>]*title\s*=\s*"([^"]+)")re");
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.poster = match.captured(1).trimmed();
        }
    }

    // Extract description: first <div class="post_body"> ... </div>.
    // Simplified approach — grab the first post_body and clamp its length.
    {
        QRegularExpression re(R"(<div[^>]*class\s*=\s*"post_body"[^>]*>(.*?)</div\s*>\s*</td>)",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            const QString desc = stripHtml(match.captured(1));
            info.description = truncateDescription(desc).trimmed();
        }
    }

    // Extract thread ID from magnet link:
    // <a ... class="magnet-link" ... data-topic_id="12345">
    // or <a ... class="magnet-link-1" ... data-topic_id="12345">
    {
        QRegularExpression re(R"re(<a[^>]*class\s*=\s*"magnet-link(?:-1)?"[^>]*data-topic_id\s*=\s*"(\d+)")re");
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.threadId = match.captured(1).toInt();
        }
    }

    // Extract content category from navigation breadcrumb:
    // <td class="vBottom"> ... <div class="nav"> ... </div>
    {
        QRegularExpression re(
            R"(<td[^>]*class\s*=\s*"vBottom"[^>]*>.*?<[^>]*class\s*=\s*"nav"[^>]*>(.*?)</(?:div|td)>)",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            QString cat = stripHtml(match.captured(1));
            cat = cat.replace(QRegularExpression("\\s+"), " ").trimmed();
            if (!cat.isEmpty()) {
                info.contentCategory = cat;
            }
        }
    }

    info.success = true;
    qInfo() << "TrackerSiteScraper: RuTracker found:" << info.name.left(60) << "threadId:" << info.threadId;

    return info;
}

// ============================================================================
// Nyaa strategy
// ============================================================================

void TrackerSiteScraper::scrapeNyaa(const QString& hash)
{
    // Nyaa allows searching by info hash via ?q=.
    QUrl url(QString("https://nyaa.si/?q=%1").arg(hash));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "TrackerSiteScraper: Nyaa request failed:" << reply->errorString();
            TrackerSiteInfo info;
            info.trackerName = "nyaa";
            onStrategyComplete(hash, info);
            return;
        }

        const QByteArray rawData = reply->readAll();

        // A single search result redirects straight to the /view/ page.
        if (reply->url().path().startsWith("/view/")) {
            onStrategyComplete(hash, parseNyaaViewHtml(rawData));
            return;
        }

        // Otherwise parse the search results to locate a view link.
        const TrackerSiteInfo searchInfo = parseNyaaSearchHtml(rawData);

        if (searchInfo.success && searchInfo.threadId > 0) {
            // Found a result — fetch the view page for full details.
            scrapeNyaaViewPage(hash, QString("https://nyaa.si/view/%1").arg(searchInfo.threadId));
        } else if (searchInfo.success) {
            // Got some info directly from the search page.
            onStrategyComplete(hash, searchInfo);
        } else {
            // No results on Nyaa.
            TrackerSiteInfo emptyInfo;
            emptyInfo.trackerName = "nyaa";
            onStrategyComplete(hash, emptyInfo);
        }
    });
}

void TrackerSiteScraper::scrapeNyaaViewPage(const QString& hash, const QString& viewUrl)
{
    if (stopping_.load()) {
        // Don't chain a fresh request during shutdown; close out the strategy.
        TrackerSiteInfo info;
        info.trackerName = "nyaa";
        onStrategyComplete(hash, info);
        return;
    }

    QUrl url(viewUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();

        TrackerSiteInfo info;
        info.trackerName = "nyaa";

        if (reply->error() == QNetworkReply::NoError) {
            info = parseNyaaViewHtml(reply->readAll());
        } else {
            qDebug() << "TrackerSiteScraper: Nyaa view page request failed:" << reply->errorString();
        }

        onStrategyComplete(hash, info);
    });
}

TrackerSiteInfo TrackerSiteScraper::parseNyaaSearchHtml(const QByteArray& rawData)
{
    TrackerSiteInfo info;
    info.trackerName = "nyaa";

    const QString html = QString::fromUtf8(rawData);
    if (html.isEmpty()) {
        return info;
    }

    // Look for a view link in the search results:
    // <td ...><a href="/view/1234567" ...>Title</a></td>
    {
        QRegularExpression re(R"re(<a[^>]*href\s*=\s*"/view/(\d+)"[^>]*>([^<]+)</a>)re");
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.threadId = match.captured(1).toInt();
            info.name = match.captured(2).trimmed();
            info.success = true;
        }
    }

    // Also try the panel-title (single result / detail view).
    if (!info.success) {
        QRegularExpression re(
            R"(<h3[^>]*class\s*=\s*"panel-title"[^>]*>(.*?)</h3>)", QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            QString title = stripHtml(match.captured(1)).trimmed();
            title.replace(QRegularExpression("[\\t\\n]+"), "");
            if (!title.isEmpty() && title != "Nyaa") {
                info.name = title;
                info.success = true;
            }
        }
    }

    // Grab the description if we happen to be on a detail page.
    {
        QRegularExpression re(R"(<div[^>]*id\s*=\s*"torrent-description"[^>]*>(.*?)</div>)",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.description = truncateDescription(stripHtml(match.captured(1)).trimmed());
        }
    }

    return info;
}

TrackerSiteInfo TrackerSiteScraper::parseNyaaViewHtml(const QByteArray& rawData)
{
    TrackerSiteInfo info;
    info.trackerName = "nyaa";

    const QString html = QString::fromUtf8(rawData);
    if (html.isEmpty()) {
        return info;
    }

    // Extract title from panel-title.
    {
        QRegularExpression re(
            R"(<h3[^>]*class\s*=\s*"panel-title"[^>]*>(.*?)</h3>)", QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            QString title = stripHtml(match.captured(1)).trimmed();
            title.replace(QRegularExpression("[\\t\\n]+"), "");
            if (!title.isEmpty() && title != "Nyaa") {
                info.name = title;
                info.success = true;
            }
        }
    }

    if (!info.success) {
        return info;
    }

    // Extract description.
    {
        QRegularExpression re(R"(<div[^>]*id\s*=\s*"torrent-description"[^>]*>(.*?)</div>)",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.description = truncateDescription(stripHtml(match.captured(1)).trimmed());
        }
    }

    // Extract view ID from a URL in the page (for thread linking).
    {
        QRegularExpression re(R"(/view/(\d+))");
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.threadId = match.captured(1).toInt();
        }
    }

    qInfo() << "TrackerSiteScraper: Nyaa found:" << info.name.left(60);

    return info;
}

// ============================================================================
// 1337x strategy (restored from the legacy Electron implementation)
// ============================================================================

void TrackerSiteScraper::scrape1337x(const QString& hash)
{
    // 1337x supports searching by the literal info hash. The search result is
    // still verified against the magnet on the detail page before any metadata
    // is accepted.
    const QUrl url(QStringLiteral("https://1337x.to/srch?search=%1")
                       .arg(QString::fromLatin1(QUrl::toPercentEncoding(hash))));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();

        TrackerSiteInfo empty;
        empty.trackerName = QStringLiteral("1337x");

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "TrackerSiteScraper: 1337x search failed:" << reply->errorString();
            onStrategyComplete(hash, empty);
            return;
        }

        const QString html = QString::fromUtf8(reply->readAll());
        QRegularExpression linkRe(
            QStringLiteral(R"re(href\s*=\s*["'](/torrent/(\d+)/[^"']*)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = linkRe.match(html);
        if (!match.hasMatch()) {
            onStrategyComplete(hash, empty);
            return;
        }

        const QString relative = match.captured(1);
        const QString href = QStringLiteral("https://1337x.to") + relative;

        QNetworkRequest detailRequest { QUrl(href) };
        detailRequest.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
        detailRequest.setRawHeader("Accept", "text/html,application/xhtml+xml");
        detailRequest.setTransferTimeout(kTimeoutMs);

        QNetworkReply* detailReply = networkManager_->get(detailRequest);
        connect(detailReply, &QNetworkReply::finished, this, [this, detailReply, hash, href]() {
            detailReply->deleteLater();

            TrackerSiteInfo info;
            info.trackerName = QStringLiteral("1337x");

            if (detailReply->error() == QNetworkReply::NoError) {
                const QByteArray raw = detailReply->readAll();
                const QString page = QString::fromUtf8(raw);
                const QRegularExpression hashRe(
                    QStringLiteral(R"(magnet:[^"'<>]*?xt=urn:btih:([A-Fa-f0-9]{40}))"),
                    QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch hashMatch = hashRe.match(page);
                if (hashMatch.hasMatch()
                    && hashMatch.captured(1).compare(hash, Qt::CaseInsensitive) == 0) {
                    info = parse1337xViewHtml(raw, href);
                }
            }

            onStrategyComplete(hash, info);
        });
    });
}

TrackerSiteInfo TrackerSiteScraper::parse1337xViewHtml(const QByteArray& rawData, const QString& href)
{
    TrackerSiteInfo info;
    info.trackerName = QStringLiteral("1337x");
    info.href = href;

    const QString html = QString::fromUtf8(rawData);
    if (html.isEmpty())
        return info;

    {
        const QRegularExpression re(
            QStringLiteral(R"(<h1[^>]*>(.*?)</h1>)"), QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch())
            info.name = stripHtml(match.captured(1)).trimmed();
    }

    if (info.name.isEmpty())
        return info;

    {
        const QRegularExpression re(
            QStringLiteral(R"re(class\s*=\s*["'][^"']*torrent-image[^"']*["'][\s\S]{0,1200}?<img[^>]*(?:src|data-original)\s*=\s*["']([^"']+)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            info.poster = match.captured(1).trimmed();
            if (info.poster.startsWith(QStringLiteral("//")))
                info.poster.prepend(QStringLiteral("https:"));
            else if (info.poster.startsWith(QLatin1Char('/')))
                info.poster.prepend(QStringLiteral("https://1337x.to"));
        }
    }

    {
        const QRegularExpression re(
            QStringLiteral(R"re(<div[^>]*id\s*=\s*["']description["'][^>]*>(.*?)</div>)re"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch())
            info.description = truncateDescription(stripHtml(match.captured(1)).trimmed());
    }

    {
        const QRegularExpression re(
            QStringLiteral(R"re(class\s*=\s*["'][^"']*torrent-category-detail[^"']*["'][\s\S]{0,1800}?<span[^>]*>(.*?)</span>)re"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch())
            info.contentCategory = stripHtml(match.captured(1)).trimmed();
    }

    {
        const QRegularExpression re(QStringLiteral(R"(/torrent/(\d+)/)"));
        const QRegularExpressionMatch match = re.match(href);
        if (match.hasMatch())
            info.threadId = match.captured(1).toInt();
    }

    info.success = true;
    qInfo() << "TrackerSiteScraper: 1337x found:" << info.name.left(60);
    return info;
}

// ============================================================================
// Rutor strategy (restored, with exact-hash verification)
// ============================================================================

void TrackerSiteScraper::scrapeRutor(const QString& hash)
{
    const QString torrentName = pendingNameForHash(hash);
    QString query = metadata::cleanMediaTitle(torrentName);
    const int year = metadata::extractYear(torrentName);
    if (year > 0)
        query += QStringLiteral(" ") + QString::number(year);

    if (query.trimmed().size() < 3) {
        TrackerSiteInfo empty;
        empty.trackerName = QStringLiteral("rutor");
        onStrategyComplete(hash, empty);
        return;
    }

    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(query.trimmed()));
    const QUrl url(QStringLiteral("https://rutor.info/search/%1").arg(encoded));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();

        TrackerSiteInfo empty;
        empty.trackerName = QStringLiteral("rutor");

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "TrackerSiteScraper: Rutor search failed:" << reply->errorString();
            onStrategyComplete(hash, empty);
            return;
        }

        const QByteArray raw = reply->readAll();
        QString html;
        const QString preview = QString::fromLatin1(raw.left(kEncodingSniffLength));
        if (preview.contains(QStringLiteral("windows-1251"), Qt::CaseInsensitive))
            html = decodeWindows1251(raw);
        else
            html = QString::fromUtf8(raw);

        QStringList candidates;
        QSet<QString> seen;
        QRegularExpression linkRe(
            QStringLiteral(R"re(href\s*=\s*["'](/torrent/(\d+)(?:/[^"']*)?)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator it = linkRe.globalMatch(html);
        while (it.hasNext() && candidates.size() < 8) {
            const QString relative = it.next().captured(1);
            const QString absolute = QStringLiteral("https://rutor.info") + relative;
            if (!seen.contains(absolute)) {
                seen.insert(absolute);
                candidates.append(absolute);
            }
        }

        if (candidates.isEmpty()) {
            onStrategyComplete(hash, empty);
            return;
        }

        scrapeRutorCandidate(hash, candidates, 0);
    });
}

void TrackerSiteScraper::scrapeRutorCandidate(const QString& hash, const QStringList& candidateUrls, int index)
{
    if (stopping_.load() || index >= candidateUrls.size()) {
        TrackerSiteInfo empty;
        empty.trackerName = QStringLiteral("rutor");
        onStrategyComplete(hash, empty);
        return;
    }

    const QString href = candidateUrls.at(index);
    QNetworkRequest request { QUrl(href) };
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash, candidateUrls, index, href]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray raw = reply->readAll();
            QString html;
            const QString preview = QString::fromLatin1(raw.left(kEncodingSniffLength));
            if (preview.contains(QStringLiteral("windows-1251"), Qt::CaseInsensitive))
                html = decodeWindows1251(raw);
            else
                html = QString::fromUtf8(raw);

            const QRegularExpression hashRe(
                QStringLiteral(R"(magnet:[^"'<>]*?xt=urn:btih:([A-Fa-f0-9]{40}))"),
                QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch match = hashRe.match(html);
            if (match.hasMatch() && match.captured(1).compare(hash, Qt::CaseInsensitive) == 0) {
                onStrategyComplete(hash, parseRutorHtml(raw, href));
                return;
            }
        }

        // The title search can contain many versions of one movie. Never borrow
        // the description from a different release: walk a small candidate set
        // until the magnet hash proves identity.
        scrapeRutorCandidate(hash, candidateUrls, index + 1);
    });
}

TrackerSiteInfo TrackerSiteScraper::parseRutorHtml(const QByteArray& rawData, const QString& href)
{
    TrackerSiteInfo info;
    info.trackerName = QStringLiteral("rutor");
    info.href = href;

    QString html;
    const QString preview = QString::fromLatin1(rawData.left(kEncodingSniffLength));
    if (preview.contains(QStringLiteral("windows-1251"), Qt::CaseInsensitive))
        html = decodeWindows1251(rawData);
    else
        html = QString::fromUtf8(rawData);
    if (html.isEmpty())
        return info;

    {
        const QRegularExpression re(
            QStringLiteral(R"(<h1[^>]*>(.*?)</h1>)"), QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch())
            info.name = stripHtml(match.captured(1)).trimmed();
    }
    if (info.name.isEmpty())
        return info;

    QString detailsHtml;
    {
        const QRegularExpression re(
            QStringLiteral(R"re(<table[^>]*id\s*=\s*["']details["'][^>]*>(.*?)</table>)re"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch()) {
            detailsHtml = match.captured(1);
            info.description = truncateDescription(stripHtml(detailsHtml).trimmed());
        }
    }

    {
        const QRegularExpression re(
            QStringLiteral(R"re(<img[^>]*src\s*=\s*["']([^"']+)["'][^>]*>)re"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(detailsHtml);
        if (match.hasMatch()) {
            info.poster = match.captured(1).trimmed();
            if (info.poster.startsWith(QStringLiteral("//")))
                info.poster.prepend(QStringLiteral("https:"));
            else if (info.poster.startsWith(QLatin1Char('/')))
                info.poster.prepend(QStringLiteral("https://rutor.info"));
        }
    }

    {
        const QRegularExpression re(
            QStringLiteral(R"re(<td[^>]*class\s*=\s*["'][^"']*header[^"']*["'][^>]*>\s*Категория\s*</td>\s*<td[^>]*>(.*?)</td>)re"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(detailsHtml);
        if (match.hasMatch())
            info.contentCategory = stripHtml(match.captured(1)).trimmed();
    }

    {
        const QRegularExpression re(QStringLiteral(R"(/torrent/(\d+))"));
        const QRegularExpressionMatch match = re.match(href);
        if (match.hasMatch())
            info.threadId = match.captured(1).toInt();
    }

    info.success = true;
    qInfo() << "TrackerSiteScraper: Rutor found exact hash:" << info.name.left(60);
    return info;
}

// ============================================================================
// Result merging
// ============================================================================

void TrackerSiteScraper::onStrategyComplete(const QString& hash, const TrackerSiteInfo& info)
{
    {
        QMutexLocker locker(&pendingMutex_);
        auto it = pendingScrapes_.find(hash);
        if (it == pendingScrapes_.end()) {
            return;
        }

        if (info.success) {
            it->results.append(info);
        }
        it->pendingCount--;
    }

    checkAllComplete(hash);
}

void TrackerSiteScraper::checkAllComplete(const QString& hash)
{
    QMutexLocker locker(&pendingMutex_);
    auto it = pendingScrapes_.find(hash);
    if (it == pendingScrapes_.end())
        return;

    if (it->pendingCount > 0)
        return;

    QJsonObject info;
    QJsonArray sources;
    QJsonObject sourceDescriptions;
    QJsonObject technicalInfo;
    int bestDescriptionScore = -1;
    QString bestDescription;

    for (const TrackerSiteInfo& result : it->results) {
        bool listed = false;
        for (const QJsonValue& value : sources) {
            if (value.toString().compare(result.trackerName, Qt::CaseInsensitive) == 0) {
                listed = true;
                break;
            }
        }
        if (!listed)
            sources.append(result.trackerName);

        if (info.value(QStringLiteral("poster")).toString().isEmpty() && !result.poster.isEmpty())
            info[QStringLiteral("poster")] = result.poster;

        if (info.value(QStringLiteral("contentCategory")).toString().isEmpty() && !result.contentCategory.isEmpty())
            info[QStringLiteral("contentCategory")] = result.contentCategory;

        if (info.value(QStringLiteral("trackerName")).toString().isEmpty() && !result.name.isEmpty())
            info[QStringLiteral("trackerName")] = result.name;

        if (!result.description.isEmpty()) {
            sourceDescriptions[result.trackerName] = result.description;
            const int score = metadata::descriptionRichness(result.description);
            if (score > bestDescriptionScore) {
                bestDescriptionScore = score;
                bestDescription = result.description;
            }

            technicalInfo = metadata::mergeTechnicalInfo(
                technicalInfo, metadata::extractTechnicalInfo(result.name + QLatin1Char('\n') + result.description));
        } else if (!result.name.isEmpty()) {
            technicalInfo
                = metadata::mergeTechnicalInfo(technicalInfo, metadata::extractTechnicalInfo(result.name));
        }

        if (result.trackerName == QStringLiteral("rutracker")) {
            if (result.threadId > 0)
                info[QStringLiteral("rutrackerThreadId")] = result.threadId;
        } else if (result.trackerName == QStringLiteral("nyaa")) {
            if (result.threadId > 0)
                info[QStringLiteral("nyaaThreadId")] = result.threadId;
        } else if (result.trackerName == QStringLiteral("rutor")) {
            if (result.threadId > 0)
                info[QStringLiteral("rutorThreadId")] = result.threadId;
            if (!result.href.isEmpty())
                info[QStringLiteral("rutorUrl")] = result.href;
        } else if (result.trackerName == QStringLiteral("1337x")) {
            if (result.threadId > 0)
                info[QStringLiteral("x1337ThreadId")] = result.threadId;
            if (!result.href.isEmpty())
                info[QStringLiteral("x1337Url")] = result.href;
        }
    }

    if (!bestDescription.isEmpty())
        info[QStringLiteral("description")] = bestDescription;
    if (!sourceDescriptions.isEmpty())
        info[QStringLiteral("sourceDescriptions")] = sourceDescriptions;
    if (!technicalInfo.isEmpty())
        info[QStringLiteral("technicalInfo")] = technicalInfo;
    if (!sources.isEmpty()) {
        // Keep the old trackers[] key for backwards compatibility with peer
        // payloads, but metadataSources is what the native UI presents.
        info[QStringLiteral("trackers")] = sources;
        info[QStringLiteral("metadataSources")] = sources;
        info[QStringLiteral("metadataSource")] = QStringLiteral("Tracker sites");
    }

    const bool found = !it->results.isEmpty();
    pendingScrapes_.erase(it);
    locker.unlock();

    {
        QMutexLocker qlock(&queueMutex_);
        if (activeRequests_ > 0)
            activeRequests_--;
    }
    processQueue();

    // A failed lookup must be retryable immediately. The old cooldown recorded
    // failures too, which made the Retry button silently do nothing for an hour.
    if (!found) {
        QMutexLocker recentLocker(&recentChecksMutex_);
        recentChecks_.remove(hash);
    }

    if (found)
        emit scraped(hash, info);
    emit scrapeFinished(hash, found);
}

// ============================================================================
// HTML helpers
// ============================================================================

QString TrackerSiteScraper::truncateDescription(const QString& text)
{
    if (text.length() > kMaxDescriptionLength) {
        return text.left(kMaxDescriptionLength) + "...";
    }
    return text;
}

QString TrackerSiteScraper::stripHtml(const QString& html)
{
    QString text = html;

    // Replace <br>, <br/>, <br /> with newlines.
    text.replace(QRegularExpression("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption), "\n");

    // Replace block-level tags with newlines.
    text.replace(QRegularExpression("</(?:p|div|li|tr|h[1-6])>", QRegularExpression::CaseInsensitiveOption), "\n");

    // Remove all remaining HTML tags.
    text.replace(QRegularExpression("<[^>]*>"), "");

    // Decode common HTML entities.
    text.replace("&amp;", "&");
    text.replace("&lt;", "<");
    text.replace("&gt;", ">");
    text.replace("&quot;", "\"");
    text.replace("&apos;", "'");
    text.replace("&#39;", "'");
    text.replace("&nbsp;", " ");
    text.replace("&#160;", " ");

    // Decode numeric entities.
    QRegularExpression numEntityRe("&#(\\d+);");
    QRegularExpressionMatchIterator numIt = numEntityRe.globalMatch(text);
    while (numIt.hasNext()) {
        const QRegularExpressionMatch m = numIt.next();
        const int code = m.captured(1).toInt();
        if (code > 0 && code < 0x10FFFF) {
            text.replace(m.captured(0), QChar(code));
        }
    }

    // Collapse runs of blank lines to at most two.
    text.replace(QRegularExpression("\\n{3,}"), "\n\n");

    // Trim whitespace from each line.
    QStringList lines = text.split('\n');
    for (QString& line : lines) {
        line = line.trimmed();
    }
    text = lines.join('\n');

    return text.trimmed();
}

QString TrackerSiteScraper::decodeWindows1251(const QByteArray& data)
{
    // Windows-1251 to Unicode mapping for bytes 0x80-0xFF
    static const char16_t win1251table[128] = {
        // 0x80-0x8F
        0x0402,
        0x0403,
        0x201A,
        0x0453,
        0x201E,
        0x2026,
        0x2020,
        0x2021,
        0x20AC,
        0x2030,
        0x0409,
        0x2039,
        0x040A,
        0x040C,
        0x040B,
        0x040F,
        // 0x90-0x9F
        0x0452,
        0x2018,
        0x2019,
        0x201C,
        0x201D,
        0x2022,
        0x2013,
        0x2014,
        0x0098,
        0x2122,
        0x0459,
        0x203A,
        0x045A,
        0x045C,
        0x045B,
        0x045F,
        // 0xA0-0xAF
        0x00A0,
        0x040E,
        0x045E,
        0x0408,
        0x00A4,
        0x0490,
        0x00A6,
        0x00A7,
        0x0401,
        0x00A9,
        0x0404,
        0x00AB,
        0x00AC,
        0x00AD,
        0x00AE,
        0x0407,
        // 0xB0-0xBF
        0x00B0,
        0x00B1,
        0x0406,
        0x0456,
        0x0491,
        0x00B5,
        0x00B6,
        0x00B7,
        0x0451,
        0x2116,
        0x0454,
        0x00BB,
        0x0458,
        0x0405,
        0x0455,
        0x0457,
        // 0xC0-0xCF: А-П (U+0410-U+041F)
        0x0410,
        0x0411,
        0x0412,
        0x0413,
        0x0414,
        0x0415,
        0x0416,
        0x0417,
        0x0418,
        0x0419,
        0x041A,
        0x041B,
        0x041C,
        0x041D,
        0x041E,
        0x041F,
        // 0xD0-0xDF: Р-Я (U+0420-U+042F)
        0x0420,
        0x0421,
        0x0422,
        0x0423,
        0x0424,
        0x0425,
        0x0426,
        0x0427,
        0x0428,
        0x0429,
        0x042A,
        0x042B,
        0x042C,
        0x042D,
        0x042E,
        0x042F,
        // 0xE0-0xEF: а-п (U+0430-U+043F)
        0x0430,
        0x0431,
        0x0432,
        0x0433,
        0x0434,
        0x0435,
        0x0436,
        0x0437,
        0x0438,
        0x0439,
        0x043A,
        0x043B,
        0x043C,
        0x043D,
        0x043E,
        0x043F,
        // 0xF0-0xFF: р-я (U+0440-U+044F)
        0x0440,
        0x0441,
        0x0442,
        0x0443,
        0x0444,
        0x0445,
        0x0446,
        0x0447,
        0x0448,
        0x0449,
        0x044A,
        0x044B,
        0x044C,
        0x044D,
        0x044E,
        0x044F,
    };

    QString result;
    result.reserve(data.size());

    for (int i = 0; i < data.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        if (ch < 0x80) {
            result.append(QChar(ch));
        } else {
            result.append(QChar(win1251table[ch - 0x80]));
        }
    }

    return result;
}

} // namespace rats::net
