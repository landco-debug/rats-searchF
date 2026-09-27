#ifndef RATS_NET_MEGAPEER_SOURCE_H
#define RATS_NET_MEGAPEER_SOURCE_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QUrl>
#include <QVector>

namespace rats::net {

class MegaPeerSource {
public:
    static QUrl searchUrl(
        const QString& query,
        const QString& sortKey = QStringLiteral("seeders_desc"));

    // Search rows bind one concrete /torrent/<id>/... page to one concrete
    // /download/<id>/... .torrent URL. The info-hash is deliberately resolved
    // from that .torrent before the candidate can be emitted.
    static QVector<domain::Torrent> parseSearchPage(
        const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates = 50);

    static bool applyDetailPage(
        domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl);

    static bool isStrictComplete(const domain::Torrent& torrent);
};

} // namespace rats::net

#endif // RATS_NET_MEGAPEER_SOURCE_H
