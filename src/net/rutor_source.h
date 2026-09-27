#ifndef RATS_NET_RUTOR_SOURCE_H
#define RATS_NET_RUTOR_SOURCE_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QUrl>
#include <QVector>

namespace rats::net {

// Source-first Rutor parser.
//
// The key invariant is provenance: a result is born from one Rutor search row
// that contains BOTH the concrete /torrent/<id> URL and the magnet info-hash.
// The detail page is then accepted only if it repeats that exact info-hash.
// This class intentionally contains no UI logic.
class RutorSource {
public:
    // Build Rutor's title-search URL. sortKey uses the GUI naming convention
    // (seeders_desc, size_asc, added_desc, name_asc, ...).
    static QUrl searchUrl(const QString& query, const QString& sortKey = QStringLiteral("seeders_desc"));

    // Parse exact source rows. Returned torrents carry unverified provenance in
    // Torrent::info: sourceProvider, sourceTopicId, sourceUrl, sourceVerified.
    static QVector<domain::Torrent> parseSearchPage(
        const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates = 50);

    // Verify that the concrete detail page belongs to the same info-hash and
    // capture release-specific text/technical fields. Returns false on identity
    // mismatch or unusable HTML.
    static bool applyDetailPage(
        domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl);

    // Type-aware strict admission rule. Video keeps the rich
    // Quality+Video+Audio contract; non-video types use exact provenance plus
    // their relevant technical/release description.
    static bool isStrictComplete(const domain::Torrent& torrent);

private:
    static int sortCode(const QString& sortKey);
};

} // namespace rats::net

#endif // RATS_NET_RUTOR_SOURCE_H
