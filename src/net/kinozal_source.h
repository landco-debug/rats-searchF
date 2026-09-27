#ifndef RATS_NET_KINOZAL_SOURCE_H
#define RATS_NET_KINOZAL_SOURCE_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QUrl>
#include <QVector>

namespace rats::net {

class KinozalSource {
public:
    static QUrl searchUrl(const QUrl& baseUrl, const QString& query,
        const QString& sortKey = QStringLiteral("seeders_desc"));

    static QVector<domain::Torrent> parseSearchPage(
        const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates = 50);

    static bool applyDetailPage(
        domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl);

    static bool applyServerDetails(
        domain::Torrent& torrent, const QByteArray& rawData);

    static bool isExactDetailUrl(const QUrl& url, int expectedId = 0);
    static bool isStrictComplete(const domain::Torrent& torrent);
};

} // namespace rats::net

#endif // RATS_NET_KINOZAL_SOURCE_H
