#ifndef RATS_NET_RUTRACKER_RU_SOURCE_H
#define RATS_NET_RUTRACKER_RU_SOURCE_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QUrl>
#include <QVector>

namespace rats::net {

// Public RuTracker.RU source-first parser.
//
// Search rows already contain BOTH an exact viewtopic.php?t=<id> link and a
// magnet/info-hash. The detail page must repeat that same info-hash before its
// release text is trusted.
class RuTrackerRuSource {
public:
    static QUrl searchUrl(
        const QString& query,
        const QString& sortKey = QStringLiteral("seeders_desc"),
        const QString& contentType = QString());

    static QVector<domain::Torrent> parseSearchPage(
        const QByteArray& rawData,
        const QUrl& pageUrl,
        int maxCandidates = 50);

    static bool applyDetailPage(
        domain::Torrent& torrent,
        const QByteArray& rawData,
        const QUrl& finalUrl);

    static bool isStrictComplete(const domain::Torrent& torrent);

private:
    static int sortColumn(const QString& sortKey);
    static int sortDirection(const QString& sortKey);
};

} // namespace rats::net

#endif // RATS_NET_RUTRACKER_RU_SOURCE_H
