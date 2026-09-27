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

    // A search row must identify one concrete /torrent/<id> page. A paired
    // direct .torrent URL is useful but no longer mandatory at this stage:
    // current detail pages normally expose a magnet, and the exact detail page
    // can also supply its own download link when a .torrent fallback is needed.
    static QVector<domain::Torrent> parseSearchPage(
        const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates = 50);

    // Parses and verifies one concrete detail page. Returns true when the page
    // itself belongs to the expected topic and its exact release information was
    // parsed. sourceVerified becomes true only after identity is established by
    // the detail-page magnet or by a hash pre-filled from that page's paired
    // .torrent fallback.
    static bool applyDetailPage(
        domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl);

    static bool isStrictComplete(const domain::Torrent& torrent);
};

} // namespace rats::net

#endif // RATS_NET_MEGAPEER_SOURCE_H
