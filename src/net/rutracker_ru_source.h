#ifndef RATS_NET_RUTRACKER_RU_SOURCE_H
#define RATS_NET_RUTRACKER_RU_SOURCE_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QUrl>
#include <QVector>

namespace rats::net {

// Authenticated RuTracker exact-source parser.
//
// The current tracker listing identifies a concrete topic and direct download,
// but it does not expose a trustworthy info-hash in the row. Identity is
// completed on the exact topic page: its magnet supplies the btih hash and the
// same topic id must match the search candidate before any release text is used.
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
