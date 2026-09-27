#ifndef RATS_NET_SOURCE_PARSE_UTILS_H
#define RATS_NET_SOURCE_PARSE_UTILS_H

#include "domain/torrent.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace rats::net::sourceparse {

// The Russian public trackers integrated by the exact-source adapters still
// serve a mixture of UTF-8 and Windows-1251. Decode losslessly enough for titles
// and technical labels without adding Qt5Compat/QTextCodec to the application.
QString decodeTrackerText(const QByteArray& bytes);

// Percent/form encoding using Windows-1251 bytes. Needed by legacy tracker
// search endpoints whose declared page encoding is Windows-1251.
QByteArray percentEncodeWindows1251(const QString& text);
QByteArray formEncodeWindows1251(const QString& text);

QString decodeEntities(QString text);
QString stripHtml(QString html);
QString htmlToText(QString html);
QUrl resolveUrl(const QUrl& base, const QString& href);
qint64 parseSize(QString text);

// Coarse source-native category mapping. Exact .torrent file classification is
// still the fallback when a site does not expose a useful category.
domain::ContentType contentTypeFromCategoryText(QString category);

// Fast, non-authoritative prioritization for typed searches. It only changes
// which exact candidates are verified first; final admission still depends on
// source page/.torrent proof. An explicit source-category mismatch is the one
// case that may be rejected before expensive detail/.torrent requests.
int contentTypeHintScore(
    const domain::Torrent& torrent, const QString& expectedType);

// Populate the shared Torrent Info fields used by TorrentDetailsPanel:
// quality, video, audioTracks and subtitles. The full exact-page description
// must already be stored in torrent.info["description"].
void populateTechnicalInfo(domain::Torrent& torrent);

} // namespace rats::net::sourceparse

#endif // RATS_NET_SOURCE_PARSE_UTILS_H
