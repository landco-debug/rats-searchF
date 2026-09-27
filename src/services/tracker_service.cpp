#include "services/tracker_service.h"

#include "data/torrent_repository.h"
#include "net/swarm_scraper.h"
#include "net/tracker_site_scraper.h"

namespace rats::service {

TrackerService::TrackerService(net::SwarmScraper* swarmScraper, net::TrackerSiteScraper* siteScraper,
    data::TorrentRepository* repository, QObject* parent)
    : QObject(parent), swarmScraper_(swarmScraper), siteScraper_(siteScraper), repository_(repository)
{
    connect(swarmScraper_, &net::SwarmScraper::scraped, this, &TrackerService::onCountsScraped);
    connect(siteScraper_, &net::TrackerSiteScraper::scraped, this, &TrackerService::onInfoScraped);
    connect(siteScraper_, &net::TrackerSiteScraper::scrapeFinished, this, &TrackerService::infoCheckFinished);
}

void TrackerService::setCountScrapingEnabled(bool enabled)
{
    countEnabled_ = enabled;
}

void TrackerService::setInfoScrapingEnabled(bool enabled)
{
    infoEnabled_ = enabled;
}

void TrackerService::stop()
{
    // Stop forwarding first so a late torrentIndexed / API call issues nothing,
    // then drain the scrapers (blocking announces / in-flight HTTP).
    countEnabled_ = false;
    infoEnabled_ = false;
    swarmScraper_->stop();
    siteScraper_->stop();
}

void TrackerService::checkCounts(const QString& hash)
{
    if (countEnabled_)
        swarmScraper_->requestScrape(hash);
}

void TrackerService::checkInfo(const QString& hash, const QString& name)
{
    if (infoEnabled_) {
        siteScraper_->scrape(hash, name);
    } else {
        emit infoCheckFinished(hash, false);
    }
}

void TrackerService::onTorrentIndexed(const domain::Torrent& torrent)
{
    checkCounts(torrent.hash);

    // Source-first results already carry a concrete release page that was
    // verified against the exact info-hash. Do not immediately run the legacy
    // post-hoc website resolver and risk attaching unrelated metadata.
    const bool exactSource
        = torrent.info.value(QStringLiteral("sourceVerified")).toBool(false)
        && !torrent.info.value(QStringLiteral("sourceUrl")).toString().isEmpty();
    if (!exactSource)
        checkInfo(torrent.hash, torrent.name);
}

void TrackerService::onCountsScraped(const QString& hash, int seeders, int leechers, int completed)
{
    repository_->updateTrackerCounts(hash, seeders, leechers, completed);
}

void TrackerService::onInfoScraped(const QString& hash, const QJsonObject& info)
{
    // Emit regardless of persistence: a search hit can come from a peer and may
    // not have been cloned into our repository yet.
    emit infoAvailable(hash, info);
    repository_->mergeInfo(hash, info);
}

} // namespace rats::service
