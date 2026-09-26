#ifndef RATS_NET_RICH_METADATA_RESOLVER_H
#define RATS_NET_RICH_METADATA_RESOLVER_H

#include "domain/content.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace rats::net {

// Enriches a torrent with human-facing movie/series and exact-release metadata.
//
// Sources are deliberately complementary:
//   * YTS: exact info-hash match -> structured quality/video/audio fields.
//   * Cinemeta: high-confidence title/year match -> synopsis/poster/cast/genres.
//   * Torrentio: exact info-hash match inside the resolved IMDb title -> release
//     string containing source, resolution, codecs, audio/languages, etc.
//
// No media is streamed or downloaded. Only small JSON metadata responses are
// fetched. Every exact-release source is accepted only after the 40-char info
// hash matches the selected torrent.
class RichMetadataResolver : public QObject {
    Q_OBJECT

public:
    explicit RichMetadataResolver(QObject* parent = nullptr);

    void resolve(const QString& infoHash, const QString& torrentName, rats::domain::ContentCategory category);

signals:
    // Incremental patch. Callers should merge rather than replace existing info:
    // later sources often add different fields to the same torrent.
    void metadataFound(const QString& infoHash, const QJsonObject& patch);

private:
    void requestYts(const QString& hash, const QString& torrentName);
    void requestCinemeta(const QString& hash, const QString& torrentName, rats::domain::ContentCategory category);
    void requestCinemetaCatalog(
        const QString& hash, const QString& torrentName, const QString& type, bool tryOtherTypeOnFailure);
    void requestCinemetaMeta(
        const QString& hash, const QString& torrentName, const QString& type, const QString& imdbId);
    void requestTorrentio(
        const QString& hash, const QString& torrentName, const QString& type, const QString& imdbId);

    static QString normalizedTitle(const QString& value);
    static int candidateScore(const QString& torrentName, const QJsonObject& candidate);
    static QString streamInfoHash(const QJsonObject& stream);
    static QJsonArray sourceArray(const QString& source);

    QNetworkAccessManager* networkManager_ = nullptr;
};

} // namespace rats::net

#endif // RATS_NET_RICH_METADATA_RESOLVER_H
