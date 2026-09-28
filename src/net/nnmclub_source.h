#ifndef RATS_NET_NNMCLUB_SOURCE_H
#define RATS_NET_NNMCLUB_SOURCE_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QUrl>
#include <QVector>

namespace rats::net {

class NnmClubSource {
public:
    static QUrl searchUrl();
    static QByteArray searchBody(
        const QString& query,
        const QString& sortKey = QStringLiteral("seeders_desc"));

    // Public mode intentionally asks NNM-Club only for rows that expose a
    // downloadable .torrent without an account (sds=4 in the tracker's public
    // search form). Every candidate therefore carries one exact topic URL and
    // one exact download.php?id=... URL from the same row.
    static QVector<domain::Torrent> parseSearchPage(
        const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates = 50);

    static bool applyDetailPage(
        domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl);

    static bool isStrictComplete(const domain::Torrent& torrent);
};

} // namespace rats::net

#endif // RATS_NET_NNMCLUB_SOURCE_H
