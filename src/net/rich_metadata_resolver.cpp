#include "net/rich_metadata_resolver.h"

#include "net/media_metadata_utils.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace rats::net {
namespace {

constexpr int kMetadataTimeoutMs = 12000;

QNetworkRequest metadataRequest(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("RatsSearch/2 (metadata resolver; +https://github.com/librats/rats-search)"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(kMetadataTimeoutMs);
    return request;
}

QString jsonStringOrNumber(const QJsonValue& value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toInt());
    return {};
}

void appendIfPresent(QStringList& lines, const QString& label, const QString& value)
{
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty())
        lines << label + QStringLiteral(": ") + trimmed;
}

} // namespace

RichMetadataResolver::RichMetadataResolver(QObject* parent)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
{
}

QJsonArray RichMetadataResolver::sourceArray(const QString& source)
{
    QJsonArray array;
    array.append(source);
    return array;
}

QString RichMetadataResolver::normalizedTitle(const QString& value)
{
    QString result = value.toLower().trimmed();
    result.replace(QLatin1Char('&'), QStringLiteral(" and "));
    result.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
    result.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return result.trimmed();
}

int RichMetadataResolver::candidateScore(const QString& torrentName, const QJsonObject& candidate)
{
    const QString wantedName = normalizedTitle(metadata::cleanMediaTitle(torrentName));
    const QString candidateName = normalizedTitle(candidate.value(QStringLiteral("name")).toString());
    if (wantedName.isEmpty() || candidateName.isEmpty())
        return -1000;

    int score = 0;
    if (candidateName == wantedName)
        score += 100;
    else if (candidateName.contains(wantedName) || wantedName.contains(candidateName))
        score += 55;
    else {
        const QStringList wantedTokens = wantedName.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int common = 0;
        for (const QString& token : wantedTokens) {
            if (token.size() >= 3 && candidateName.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(token))
                ++common;
        }
        if (!wantedTokens.isEmpty())
            score += (common * 50) / wantedTokens.size();
    }

    const int wantedYear = metadata::extractYear(torrentName);
    int candidateYear = candidate.value(QStringLiteral("year")).toInt();
    if (candidateYear == 0)
        candidateYear = jsonStringOrNumber(candidate.value(QStringLiteral("year"))).left(4).toInt();
    if (candidateYear == 0)
        candidateYear = metadata::extractYear(candidate.value(QStringLiteral("releaseInfo")).toString());

    if (wantedYear > 0 && candidateYear > 0)
        score += (wantedYear == candidateYear) ? 40 : -45;

    return score;
}

void RichMetadataResolver::resolve(
    const QString& infoHash, const QString& torrentName, rats::domain::ContentCategory category)
{
    const QString hash = infoHash.trimmed().toLower();
    if (hash.size() != 40 || torrentName.trimmed().isEmpty())
        return;

    // Independent paths: an outage or a poor catalog match in one must not stop
    // the others.
    requestYts(hash, torrentName);
    requestCinemeta(hash, torrentName, category);
}

void RichMetadataResolver::requestYts(const QString& hash, const QString& torrentName)
{
    const QString title = metadata::cleanMediaTitle(torrentName);
    if (title.isEmpty())
        return;

    QUrl url(QStringLiteral("https://yts.mx/api/v2/list_movies.json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query_term"), title);
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("20"));
    query.addQueryItem(QStringLiteral("with_rt_ratings"), QStringLiteral("false"));
    url.setQuery(query);

    QNetworkReply* reply = networkManager_->get(metadataRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash, torrentName]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError)
            return;

        const QJsonArray movies
            = document.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("movies")).toArray();
        for (const QJsonValue& movieValue : movies) {
            const QJsonObject movie = movieValue.toObject();
            const QJsonArray torrents = movie.value(QStringLiteral("torrents")).toArray();
            for (const QJsonValue& torrentValue : torrents) {
                const QJsonObject torrent = torrentValue.toObject();
                if (torrent.value(QStringLiteral("hash")).toString().compare(hash, Qt::CaseInsensitive) != 0)
                    continue;

                QJsonObject patch;
                patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("YTS"));

                const QString synopsis = movie.value(QStringLiteral("description_full")).toString().trimmed();
                if (!synopsis.isEmpty())
                    patch[QStringLiteral("synopsis")] = synopsis;

                const QString poster = movie.value(QStringLiteral("large_cover_image")).toString().trimmed();
                if (!poster.isEmpty())
                    patch[QStringLiteral("poster")] = poster;

                const QString imdb = movie.value(QStringLiteral("imdb_code")).toString().trimmed();
                if (!imdb.isEmpty())
                    patch[QStringLiteral("imdbId")] = imdb;

                const QString pageUrl = movie.value(QStringLiteral("url")).toString().trimmed();
                if (!pageUrl.isEmpty())
                    patch[QStringLiteral("ytsUrl")] = pageUrl;

                const QJsonArray genres = movie.value(QStringLiteral("genres")).toArray();
                if (!genres.isEmpty())
                    patch[QStringLiteral("genres")] = genres;

                if (movie.value(QStringLiteral("runtime")).toInt() > 0)
                    patch[QStringLiteral("runtimeMinutes")] = movie.value(QStringLiteral("runtime")).toInt();

                QJsonObject tech;
                const QString quality = torrent.value(QStringLiteral("quality")).toString();
                if (!quality.isEmpty())
                    tech[QStringLiteral("resolution")] = quality;
                const QString source = torrent.value(QStringLiteral("type")).toString();
                if (!source.isEmpty())
                    tech[QStringLiteral("source")] = source;
                const QString codec = torrent.value(QStringLiteral("video_codec")).toString();
                if (!codec.isEmpty())
                    tech[QStringLiteral("videoCodec")] = codec;
                const QString bitDepth = torrent.value(QStringLiteral("bit_depth")).toString();
                if (!bitDepth.isEmpty())
                    tech[QStringLiteral("bitDepth")] = bitDepth;
                const QString audioChannels = torrent.value(QStringLiteral("audio_channels")).toString();
                if (!audioChannels.isEmpty()) {
                    QJsonArray channels;
                    channels.append(audioChannels);
                    tech[QStringLiteral("audioChannels")] = channels;
                }
                const QString language = movie.value(QStringLiteral("language")).toString();
                if (!language.isEmpty()) {
                    QJsonArray languages;
                    languages.append(language);
                    tech[QStringLiteral("languages")] = languages;
                }
                if (!tech.isEmpty())
                    patch[QStringLiteral("technicalInfo")] = tech;

                QStringList release;
                appendIfPresent(release, QStringLiteral("Quality"), quality);
                appendIfPresent(release, QStringLiteral("Source"), source);
                appendIfPresent(release, QStringLiteral("Video codec"), codec);
                appendIfPresent(release, QStringLiteral("Bit depth"), bitDepth);
                appendIfPresent(release, QStringLiteral("Audio channels"), audioChannels);
                appendIfPresent(release, QStringLiteral("Language"), language);
                if (!release.isEmpty())
                    patch[QStringLiteral("releaseDetails")] = release.join(QLatin1Char('\n'));

                qInfo() << "RichMetadataResolver: exact YTS hash match for" << hash.left(12);
                emit metadataFound(hash, patch);
                return;
            }
        }

        Q_UNUSED(torrentName);
    });
}

void RichMetadataResolver::requestCinemeta(
    const QString& hash, const QString& torrentName, rats::domain::ContentCategory category)
{
    if (category == rats::domain::ContentCategory::Series) {
        requestCinemetaCatalog(hash, torrentName, QStringLiteral("series"), false);
        return;
    }
    if (category == rats::domain::ContentCategory::Movie) {
        requestCinemetaCatalog(hash, torrentName, QStringLiteral("movie"), false);
        return;
    }

    // Unknown category: movies are more common in the desktop result set. Only
    // try series if the movie catalog cannot produce a confident title/year
    // match.
    requestCinemetaCatalog(hash, torrentName, QStringLiteral("movie"), true);
}

void RichMetadataResolver::requestCinemetaCatalog(
    const QString& hash, const QString& torrentName, const QString& type, bool tryOtherTypeOnFailure)
{
    const QString title = metadata::cleanMediaTitle(torrentName);
    if (title.isEmpty())
        return;

    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(title));
    const QUrl url(QStringLiteral("https://v3-cinemeta.strem.io/catalog/%1/top/search=%2.json").arg(type, encoded));

    QNetworkReply* reply = networkManager_->get(metadataRequest(url));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, hash, torrentName, type, tryOtherTypeOnFailure]() {
            reply->deleteLater();

            auto tryOther = [this, hash, torrentName, type, tryOtherTypeOnFailure]() {
                if (tryOtherTypeOnFailure)
                    requestCinemetaCatalog(hash, torrentName,
                        type == QStringLiteral("movie") ? QStringLiteral("series") : QStringLiteral("movie"), false);
            };

            if (reply->error() != QNetworkReply::NoError) {
                tryOther();
                return;
            }

            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
            if (error.error != QJsonParseError::NoError) {
                tryOther();
                return;
            }

            const QJsonArray metas = document.object().value(QStringLiteral("metas")).toArray();
            int bestScore = -1000;
            QJsonObject best;
            for (const QJsonValue& value : metas) {
                const QJsonObject candidate = value.toObject();
                const int score = candidateScore(torrentName, candidate);
                if (score > bestScore) {
                    bestScore = score;
                    best = candidate;
                }
            }

            // A title-only fuzzy hit is not enough to attach the description of
            // a different movie to this torrent. Exact/near-exact name or
            // matching name+year is required.
            if (bestScore < 55) {
                tryOther();
                return;
            }

            const QString imdbId = best.value(QStringLiteral("id")).toString();
            if (imdbId.isEmpty() || !imdbId.startsWith(QStringLiteral("tt"))) {
                tryOther();
                return;
            }

            requestCinemetaMeta(hash, torrentName, type, imdbId);
        });
}

void RichMetadataResolver::requestCinemetaMeta(
    const QString& hash, const QString& torrentName, const QString& type, const QString& imdbId)
{
    const QUrl url(QStringLiteral("https://v3-cinemeta.strem.io/meta/%1/%2.json").arg(type, imdbId));
    QNetworkReply* reply = networkManager_->get(metadataRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash, torrentName, type, imdbId]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError)
            return;

        const QJsonObject meta = document.object().value(QStringLiteral("meta")).toObject();
        if (meta.isEmpty())
            return;

        QJsonObject patch;
        patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("Cinemeta"));
        patch[QStringLiteral("imdbId")] = imdbId;
        patch[QStringLiteral("mediaType")] = type;

        const QString description = meta.value(QStringLiteral("description")).toString().trimmed();
        if (!description.isEmpty())
            patch[QStringLiteral("synopsis")] = description;

        const QString poster = meta.value(QStringLiteral("poster")).toString().trimmed();
        if (!poster.isEmpty())
            patch[QStringLiteral("poster")] = poster;

        const QString name = meta.value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty())
            patch[QStringLiteral("mediaTitle")] = name;

        const QString year = jsonStringOrNumber(meta.value(QStringLiteral("year")));
        if (!year.isEmpty())
            patch[QStringLiteral("mediaYear")] = year;

        const QString runtime = meta.value(QStringLiteral("runtime")).toString().trimmed();
        if (!runtime.isEmpty())
            patch[QStringLiteral("runtime")] = runtime;

        for (const QString& key : { QStringLiteral("genres"), QStringLiteral("director"), QStringLiteral("cast") }) {
            const QJsonValue value = meta.value(key);
            if (value.isArray() && !value.toArray().isEmpty())
                patch[key] = value;
            else if (value.isString() && !value.toString().trimmed().isEmpty())
                patch[key] = value;
        }

        const QString imdbRating = jsonStringOrNumber(meta.value(QStringLiteral("imdbRating")));
        if (!imdbRating.isEmpty())
            patch[QStringLiteral("imdbRating")] = imdbRating;

        qInfo() << "RichMetadataResolver: Cinemeta match" << imdbId << "for" << hash.left(12);
        emit metadataFound(hash, patch);

        // The IMDb ID is a bridge to an exact-hash stream index. Unlike the
        // generic synopsis above, Torrentio data is accepted only when the
        // returned stream's infoHash is exactly the selected torrent.
        requestTorrentio(hash, torrentName, type, imdbId);
    });
}

QString RichMetadataResolver::streamInfoHash(const QJsonObject& stream)
{
    QString hash = stream.value(QStringLiteral("infoHash")).toString().trimmed().toLower();
    if (hash.size() == 40)
        return hash;

    const QString magnet = stream.value(QStringLiteral("magnet")).toString();
    const QString url = stream.value(QStringLiteral("url")).toString();
    const QString combined = magnet + QLatin1Char(' ') + url;
    static const QRegularExpression hashRe(QStringLiteral(R"(([A-Fa-f0-9]{40}))"));
    const QRegularExpressionMatch match = hashRe.match(combined);
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

void RichMetadataResolver::requestTorrentio(
    const QString& hash, const QString& torrentName, const QString& type, const QString& imdbId)
{
    QString id = imdbId;
    if (type == QStringLiteral("series")) {
        QRegularExpression episodeRe(
            QStringLiteral(R"(\bS(\d{1,2})E(\d{1,3})\b)"), QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch match = episodeRe.match(torrentName);
        if (!match.hasMatch()) {
            episodeRe.setPattern(QStringLiteral(R"(\b(\d{1,2})x(\d{1,3})\b)"));
            match = episodeRe.match(torrentName);
        }
        if (!match.hasMatch())
            return;
        id += QStringLiteral(":%1:%2").arg(match.captured(1).toInt()).arg(match.captured(2).toInt());
    }

    const QUrl url(QStringLiteral("https://torrentio.strem.fun/stream/%1/%2.json").arg(type, id));
    QNetworkReply* reply = networkManager_->get(metadataRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError)
            return;

        const QJsonArray streams = document.object().value(QStringLiteral("streams")).toArray();
        for (const QJsonValue& value : streams) {
            const QJsonObject stream = value.toObject();
            if (streamInfoHash(stream).compare(hash, Qt::CaseInsensitive) != 0)
                continue;

            const QString name = stream.value(QStringLiteral("name")).toString().trimmed();
            const QString title = stream.value(QStringLiteral("title")).toString().trimmed();
            const QString description = stream.value(QStringLiteral("description")).toString().trimmed();

            QString details = title;
            if (details.isEmpty())
                details = description;
            if (details.isEmpty())
                details = name;

            QJsonObject patch;
            patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("Torrentio"));
            if (!details.isEmpty()) {
                patch[QStringLiteral("releaseDetails")] = details;
                patch[QStringLiteral("technicalInfo")]
                    = metadata::extractTechnicalInfo(name + QLatin1Char('\n') + details);
            }

            qInfo() << "RichMetadataResolver: exact Torrentio hash match for" << hash.left(12);
            emit metadataFound(hash, patch);
            return;
        }
    });
}

} // namespace rats::net
