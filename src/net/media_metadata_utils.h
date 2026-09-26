#ifndef RATS_NET_MEDIA_METADATA_UTILS_H
#define RATS_NET_MEDIA_METADATA_UTILS_H

#include <QJsonObject>
#include <QString>

namespace rats::net::metadata {

// Pull a stable movie/series title out of release-style torrent names.
// Example: "Se7en (1995) [2160p] [BluRay] [5.1]" -> "Se7en".
QString cleanMediaTitle(const QString& torrentName);

// Returns a four-digit release year found in the name, or 0.
int extractYear(const QString& text);

// Extract user-facing technical release facts from tracker descriptions,
// filenames and stream descriptions. The returned object is deliberately
// source-agnostic so data from several sources can be merged.
QJsonObject extractTechnicalInfo(const QString& text);

// Merge two technical-info objects without throwing away arrays discovered by
// an earlier source. Scalar values keep the more informative (longer) value.
QJsonObject mergeTechnicalInfo(const QJsonObject& base, const QJsonObject& incoming);

// Score a tracker description by how useful it is to a human. This is used to
// choose the richest description instead of whichever HTTP request finishes
// first.
int descriptionRichness(const QString& text);

} // namespace rats::net::metadata

#endif // RATS_NET_MEDIA_METADATA_UTILS_H
