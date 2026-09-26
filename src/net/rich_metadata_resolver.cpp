#include "net/rich_metadata_resolver.h"

#include "net/media_metadata_utils.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
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

QNetworkRequest htmlRequest(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/153.0 Safari/537.36"));
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.7");
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

QString RichMetadataResolver::releaseSearchQuery(
    const QString& torrentName, const QVector<rats::domain::File>& files)
{
    QString query = torrentName;
    query.replace(QRegularExpression(QStringLiteral("[._]+")), QStringLiteral(" "));
    query.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    query = query.trimmed();

    // Many DHT names omit the release group while the actual video filename
    // carries it (e.g. DoMiNo, HD-Films, YTS). Add the largest file's basename
    // when it contributes words that are not already present.
    const rats::domain::File* largest = nullptr;
    for (const auto& file : files) {
        if (!largest || file.size > largest->size)
            largest = &file;
    }
    if (largest && !largest->path.trimmed().isEmpty()) {
        QString fileName = largest->path;
        fileName.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const int slash = fileName.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0)
            fileName = fileName.mid(slash + 1);
        fileName.remove(QRegularExpression(QStringLiteral(R"(\.[A-Za-z0-9]{2,5}$)")));
        fileName.replace(QRegularExpression(QStringLiteral("[._]+")), QStringLiteral(" "));
        fileName.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        fileName = fileName.trimmed();

        const QString normQuery = normalizedTitle(query);
        const QStringList tokens = normalizedTitle(fileName).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QStringList extra;
        for (const QString& token : tokens) {
            if (token.size() < 3 || normQuery.contains(token, Qt::CaseInsensitive))
                continue;
            bool numericYear = false;
            token.toInt(&numericYear);
            if (numericYear && token.size() == 4)
                continue;
            extra << token;
            if (extra.size() >= 4)
                break;
        }
        if (!extra.isEmpty())
            query += QStringLiteral(" ") + extra.join(QLatin1Char(' '));
    }

    if (query.size() > 150)
        query = query.left(150);
    return query.trimmed();
}

qint64 RichMetadataResolver::parseHumanSize(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral(R"((\d+(?:[\.,]\d+)?)\s*(TB|TiB|GB|GiB|MB|MiB|KB|KiB)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch())
        return 0;

    QString number = match.captured(1);
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double value = number.toDouble(&ok);
    if (!ok)
        return 0;

    const QString unit = match.captured(2).toUpper();
    qint64 multiplier = 1;
    if (unit.startsWith(QStringLiteral("K")))
        multiplier = 1024LL;
    else if (unit.startsWith(QStringLiteral("M")))
        multiplier = 1024LL * 1024LL;
    else if (unit.startsWith(QStringLiteral("G")))
        multiplier = 1024LL * 1024LL * 1024LL;
    else if (unit.startsWith(QStringLiteral("T")))
        multiplier = 1024LL * 1024LL * 1024LL * 1024LL;
    return static_cast<qint64>(value * static_cast<double>(multiplier));
}

QString RichMetadataResolver::stripHtml(QString html)
{
    html.replace(QRegularExpression(QStringLiteral(R"(<\s*br\s*/?\s*>)"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.replace(QRegularExpression(QStringLiteral(R"(</\s*(?:p|div|li|tr|td|th|h[1-6])\s*>)"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.remove(QRegularExpression(QStringLiteral(R"(<script\b[^>]*>[\s\S]*?</script>)"),
        QRegularExpression::CaseInsensitiveOption));
    html.remove(QRegularExpression(QStringLiteral(R"(<style\b[^>]*>[\s\S]*?</style>)"),
        QRegularExpression::CaseInsensitiveOption));
    html.remove(QRegularExpression(QStringLiteral(R"(<[^>]+>)")));
    html.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    html.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    html.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    html.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    html.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    html.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    html.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    html.replace(QRegularExpression(QStringLiteral("\\n[ \\t]*\\n(?:[ \\t]*\\n)+")), QStringLiteral("\n\n"));
    return html.trimmed();
}

int RichMetadataResolver::releaseCandidateScore(const QString& torrentName,
    const QVector<rats::domain::File>& files, qint64 totalSize,
    const QString& candidateTitle, qint64 candidateSize, bool exactHash)
{
    if (candidateTitle.trimmed().isEmpty())
        return -1000;
    if (exactHash)
        return 5000;

    QString wanted = normalizedTitle(torrentName);
    for (const auto& file : files)
        wanted += QLatin1Char(' ') + normalizedTitle(file.path);
    const QString candidate = normalizedTitle(candidateTitle);

    QSet<QString> wantedTokens;
    for (const QString& token : wanted.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (token.size() >= 3)
            wantedTokens.insert(token);
    }
    QSet<QString> candidateTokens;
    for (const QString& token : candidate.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (token.size() >= 3)
            candidateTokens.insert(token);
    }

    int common = 0;
    for (const QString& token : candidateTokens) {
        if (wantedTokens.contains(token))
            ++common;
    }

    int score = 0;
    if (!candidateTokens.isEmpty())
        score += (common * 140) / candidateTokens.size();

    const QString cleanWanted = normalizedTitle(metadata::cleanMediaTitle(torrentName));
    if (!cleanWanted.isEmpty() && candidate.contains(cleanWanted))
        score += 70;

    static const QStringList releaseMarkers = {
        QStringLiteral("2160p"), QStringLiteral("1080p"), QStringLiteral("720p"),
        QStringLiteral("hdrip"), QStringLiteral("bdrip"), QStringLiteral("bluray"),
        QStringLiteral("remux"), QStringLiteral("web"), QStringLiteral("webdl"),
        QStringLiteral("hdtv"), QStringLiteral("avc"), QStringLiteral("hevc"),
        QStringLiteral("h264"), QStringLiteral("h265"), QStringLiteral("x264"),
        QStringLiteral("x265"), QStringLiteral("10bit"), QStringLiteral("60fps"),
        QStringLiteral("domino"), QStringLiteral("hd-films"), QStringLiteral("yts")
    };
    for (const QString& marker : releaseMarkers) {
        if (wanted.contains(marker) && candidate.contains(marker))
            score += 18;
        else if (wanted.contains(marker) != candidate.contains(marker))
            score -= 8;
    }

    if (totalSize > 0 && candidateSize > 0) {
        const double diff = qAbs(static_cast<double>(candidateSize - totalSize)) / static_cast<double>(totalSize);
        if (diff <= 0.006)
            score += 220;
        else if (diff <= 0.015)
            score += 150;
        else if (diff <= 0.04)
            score += 45;
        else
            score -= 180;
    }

    return score;
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

void RichMetadataResolver::resolve(const QString& infoHash, const QString& torrentName, qint64 totalSize,
    const QVector<rats::domain::File>& files, rats::domain::ContentCategory category)
{
    const QString hash = infoHash.trimmed().toLower();
    if (hash.size() != 40 || torrentName.trimmed().isEmpty())
        return;

    // Release resolvers are deliberately independent from info-hash: mirrors
    // often re-create the .torrent and therefore change the hash while preserving
    // the exact encode. Match the release fingerprint + total size first.
    requestExtReleaseMatch(hash, torrentName, totalSize, files);
    requestOxTorrentReleaseMatch(hash, torrentName, totalSize, files);
    requestRutorReleaseMatch(hash, torrentName, totalSize, files);

    // Generic title metadata is supplemental only. It can add synopsis/poster,
    // but never marks a release lookup complete by itself.
    requestYts(hash, torrentName);
    // Cinemeta provides cleaner movie/series synopsis matching. Wikipedia was
    // intentionally removed from the automatic path because ambiguous titles
    // (for example "Se7en") can resolve to disambiguation pages and pollute an
    // otherwise exact release card.
    requestCinemeta(hash, torrentName, category);
}


void RichMetadataResolver::requestExtReleaseMatch(const QString& hash, const QString& torrentName,
    qint64 totalSize, const QVector<rats::domain::File>& files)
{
    QString query = metadata::cleanMediaTitle(torrentName);
    const int year = metadata::extractYear(torrentName);
    if (year > 0)
        query += QStringLiteral(" ") + QString::number(year);
    if (query.trimmed().size() < 3)
        query = torrentName;
    if (query.trimmed().size() < 3)
        return;

    QUrl url(QStringLiteral("https://ext.to/browse/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query.trimmed());
    url.setQuery(q);

    QNetworkReply* reply = networkManager_->get(htmlRequest(url));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, hash, torrentName, totalSize, files]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;

            const QString html = QString::fromUtf8(reply->readAll());
            if (html.trimmed().isEmpty())
                return;

            QString bestUrl;
            QString bestTitle;
            QString bestSource;
            qint64 bestSize = 0;
            int bestScore = -1000;

            const QRegularExpression rowRe(
                QStringLiteral(R"re(<tr[^>]*>(.*?)</tr>)re"),
                QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
            const QRegularExpression torrentLinkRe(
                QStringLiteral(R"re(<a[^>]+href\s*=\s*["'](/[^"'<>]+-\d+/)["'][^>]*>(.*?)</a>)re"),
                QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);

            QRegularExpressionMatchIterator rows = rowRe.globalMatch(html);
            while (rows.hasNext()) {
                const QString row = rows.next().captured(1);
                const QRegularExpressionMatch link = torrentLinkRe.match(row);
                if (!link.hasMatch())
                    continue;

                const QString title = stripHtml(link.captured(2)).trimmed();
                if (title.isEmpty())
                    continue;

                const qint64 rowSize = parseHumanSize(stripHtml(row));
                const int score = releaseCandidateScore(
                    torrentName, files, totalSize, title, rowSize, false);
                if (score <= bestScore)
                    continue;

                // If a size is available, a substantially different payload is
                // not the same release even when the movie/release group matches.
                if (totalSize > 0 && rowSize > 0) {
                    const double diff = qAbs(static_cast<double>(rowSize - totalSize))
                        / static_cast<double>(totalSize);
                    if (diff > 0.045)
                        continue;
                }

                bestScore = score;
                bestTitle = title;
                bestSize = rowSize;
                bestUrl = QStringLiteral("https://ext.to") + link.captured(1);

                const QString rowText = stripHtml(row);
                static const QRegularExpression sourceRe(
                    QStringLiteral(R"(\b(?:person|source)\s+([A-Za-z0-9_.-]{2,40})\b)"),
                    QRegularExpression::CaseInsensitiveOption);
                const auto sourceMatch = sourceRe.match(rowText);
                if (sourceMatch.hasMatch())
                    bestSource = sourceMatch.captured(1);
            }

            if (bestUrl.isEmpty() || bestScore < 280)
                return;

            QJsonObject patch;
            patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("EXT exact release"));
            patch[QStringLiteral("extUrl")] = bestUrl;
            patch[QStringLiteral("releaseReferenceUrl")] = bestUrl;
            patch[QStringLiteral("releaseReferenceTitle")] = bestTitle;
            patch[QStringLiteral("releaseMatchMethod")] = QStringLiteral("exact release fingerprint + total size");
            if (!bestSource.isEmpty())
                patch[QStringLiteral("releaseCatalogSource")] = bestSource;

            QJsonObject tech = metadata::extractTechnicalInfo(bestTitle);
            if (!tech.isEmpty())
                patch[QStringLiteral("technicalInfo")] = tech;

            QStringList release;
            release << bestTitle;
            if (bestSize > 0 && totalSize > 0) {
                const double pct = qAbs(static_cast<double>(bestSize - totalSize))
                    / static_cast<double>(totalSize) * 100.0;
                release << QStringLiteral("Catalog size match: %1% difference").arg(pct, 0, 'f', 2);
            }
            if (!bestSource.isEmpty())
                release << QStringLiteral("Indexed source: ") + bestSource;
            patch[QStringLiteral("releaseDetails")] = release.join(QLatin1Char('\n'));

            qInfo() << "RichMetadataResolver: EXT exact release match"
                    << bestTitle.left(100) << "score" << bestScore;
            emit metadataFound(hash, patch);
        });
}

void RichMetadataResolver::requestOxTorrentReleaseMatch(const QString& hash, const QString& torrentName,
    qint64 totalSize, const QVector<rats::domain::File>& files)
{
    QString query = releaseSearchQuery(torrentName, files);
    if (query.trimmed().size() < 3)
        return;

    // OxTorrent's current search route is /recherche/<query>. Keep dots/hyphens
    // in the release name: they are useful fingerprints and the site accepts
    // percent-encoded path segments.
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(query.trimmed()));
    const QUrl url(QStringLiteral("https://www.oxtorrent.co/recherche/") + encoded);

    QNetworkReply* reply = networkManager_->get(htmlRequest(url));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, hash, torrentName, totalSize, files]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;

            const QString html = QString::fromUtf8(reply->readAll());
            if (html.trimmed().isEmpty())
                return;

            QString bestUrl;
            QString bestTitle;
            qint64 bestSize = 0;
            int bestScore = -1000;

            // Search results contain concrete /torrent/<id>/<slug> links.
            const QRegularExpression linkRe(
                QStringLiteral(R"re(<a[^>]+href\s*=\s*["'](/torrent/\d+/[^"']+)["'][^>]*>(.*?)</a>)re"),
                QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatchIterator it = linkRe.globalMatch(html);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                const QString title = stripHtml(m.captured(2)).trimmed();
                if (title.isEmpty())
                    continue;

                // Read the local neighbourhood for the result's size. The site
                // changes wrappers often, but size text stays next to the link.
                const int start = qMax(0, m.capturedStart(0) - 250);
                const int length = qMin(1400, html.size() - start);
                const QString around = stripHtml(html.mid(start, length));
                const qint64 candidateSize = parseHumanSize(around);

                const int score = releaseCandidateScore(
                    torrentName, files, totalSize, title, candidateSize, false);

                if (totalSize > 0 && candidateSize > 0) {
                    const double diff = qAbs(static_cast<double>(candidateSize - totalSize))
                        / static_cast<double>(totalSize);
                    if (diff > 0.045)
                        continue;
                }

                if (score > bestScore) {
                    bestScore = score;
                    bestTitle = title;
                    bestSize = candidateSize;
                    bestUrl = QStringLiteral("https://www.oxtorrent.co") + m.captured(1);
                }
            }

            if (bestUrl.isEmpty() || bestScore < 280)
                return;

            qInfo() << "RichMetadataResolver: OxTorrent exact release candidate"
                    << bestTitle.left(100) << "score" << bestScore;

            // Surface the concrete page only after an actual candidate has been
            // matched. No generic search button is exposed.
            QJsonObject reference;
            reference[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("OxTorrent exact release"));
            reference[QStringLiteral("oxtorrentUrl")] = bestUrl;
            reference[QStringLiteral("releaseReferenceUrl")] = bestUrl;
            reference[QStringLiteral("releaseReferenceTitle")] = bestTitle;
            reference[QStringLiteral("releaseMatchMethod")] = QStringLiteral("exact release fingerprint + total size");
            emit metadataFound(hash, reference);

            requestOxTorrentDetail(hash, bestUrl, bestTitle, torrentName, totalSize, bestSize);
        });
}

void RichMetadataResolver::requestOxTorrentDetail(const QString& hash, const QString& candidateUrl,
    const QString& candidateTitle, const QString& torrentName, qint64 totalSize, qint64 candidateSize)
{
    QNetworkReply* reply = networkManager_->get(htmlRequest(QUrl(candidateUrl)));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, hash, candidateUrl, candidateTitle, torrentName, totalSize, candidateSize]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;

            const QString html = QString::fromUtf8(reply->readAll());
            if (html.trimmed().isEmpty())
                return;

            const QString plain = stripHtml(html);
            QString description;

            // On the current OxTorrent template the synopsis is between the
            // release title and "Informations du fichier". Use the last title
            // occurrence before the marker to skip breadcrumbs/navigation.
            int infoPos = plain.indexOf(QStringLiteral("Informations du fichier"), 0, Qt::CaseInsensitive);
            if (infoPos < 0)
                infoPos = plain.indexOf(QStringLiteral("Informations fichier"), 0, Qt::CaseInsensitive);
            if (infoPos > 0) {
                const int titlePos = plain.lastIndexOf(candidateTitle, infoPos, Qt::CaseInsensitive);
                if (titlePos >= 0) {
                    description = plain.mid(titlePos + candidateTitle.size(),
                        infoPos - (titlePos + candidateTitle.size())).trimmed();
                }
            }

            if (description.size() > 12000)
                description = description.left(12000) + QStringLiteral("…");

            QJsonObject patch;
            patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("OxTorrent exact release"));
            patch[QStringLiteral("oxtorrentUrl")] = candidateUrl;
            patch[QStringLiteral("releaseReferenceUrl")] = candidateUrl;
            patch[QStringLiteral("releaseReferenceTitle")] = candidateTitle;
            patch[QStringLiteral("releaseMatchMethod")] = QStringLiteral("exact release fingerprint + total size");

            if (description.size() >= 40)
                patch[QStringLiteral("description")] = description;

            const QJsonObject tech = metadata::extractTechnicalInfo(
                candidateTitle + QLatin1Char('\n') + description);
            if (!tech.isEmpty())
                patch[QStringLiteral("technicalInfo")] = tech;

            QStringList release;
            release << candidateTitle;
            if (candidateSize > 0 && totalSize > 0) {
                const double pct = qAbs(static_cast<double>(candidateSize - totalSize))
                    / static_cast<double>(totalSize) * 100.0;
                release << QStringLiteral("Matched size: %1% difference").arg(pct, 0, 'f', 2);
            }
            patch[QStringLiteral("releaseDetails")] = release.join(QLatin1Char('\n'));

            // Prefer the page's OpenGraph image; unlike the first <img>, it does
            // not accidentally pick the site logo.
            const QRegularExpression ogImageRe(
                QStringLiteral(R"re(<meta[^>]+(?:property|name)\s*=\s*["']og:image["'][^>]+content\s*=\s*["']([^"']+)["'][^>]*>)re"),
                QRegularExpression::CaseInsensitiveOption);
            auto imageMatch = ogImageRe.match(html);
            if (!imageMatch.hasMatch()) {
                const QRegularExpression reversedOgImageRe(
                    QStringLiteral(R"re(<meta[^>]+content\s*=\s*["']([^"']+)["'][^>]+(?:property|name)\s*=\s*["']og:image["'][^>]*>)re"),
                    QRegularExpression::CaseInsensitiveOption);
                imageMatch = reversedOgImageRe.match(html);
            }
            if (imageMatch.hasMatch()) {
                const QString poster = imageMatch.captured(1).trimmed();
                if (poster.startsWith(QStringLiteral("http")))
                    patch[QStringLiteral("poster")] = poster;
            }

            qInfo() << "RichMetadataResolver: OxTorrent release detail parsed for"
                    << hash.left(12) << candidateTitle.left(100);
            emit metadataFound(hash, patch);
            Q_UNUSED(torrentName);
        });
}

void RichMetadataResolver::requestRutorReleaseMatch(const QString& hash, const QString& torrentName,
    qint64 totalSize, const QVector<rats::domain::File>& files)
{
    const QStringList mirrors = {
        QStringLiteral("https://new-rutor.org"),
        QStringLiteral("https://r.rss.new-rutor.org"),
        QStringLiteral("https://www55.new-rutor.org"),
        QStringLiteral("https://aa.new-rutor.org"),
        QStringLiteral("https://rutor.info")
    };
    requestRutorMirror(hash, torrentName, totalSize, files, mirrors, 0);
}

void RichMetadataResolver::requestRutorMirror(const QString& hash, const QString& torrentName, qint64 totalSize,
    const QVector<rats::domain::File>& files, const QStringList& mirrors, int mirrorIndex)
{
    if (mirrorIndex >= mirrors.size())
        return;

    const QString query = releaseSearchQuery(torrentName, files);
    if (query.size() < 3)
        return;

    const QString base = mirrors.at(mirrorIndex);
    const QUrl url(base + QStringLiteral("/search/")
        + QString::fromLatin1(QUrl::toPercentEncoding(query)));

    QNetworkReply* reply = networkManager_->get(htmlRequest(url));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, hash, torrentName, totalSize, files, mirrors, mirrorIndex, base]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                requestRutorMirror(hash, torrentName, totalSize, files, mirrors, mirrorIndex + 1);
                return;
            }

            const QString html = QString::fromUtf8(reply->readAll());
            if (html.trimmed().isEmpty()) {
                requestRutorMirror(hash, torrentName, totalSize, files, mirrors, mirrorIndex + 1);
                return;
            }

            QString bestUrl;
            QString bestTitle;
            qint64 bestSize = 0;
            int bestScore = -1000;

            const QRegularExpression rowRe(
                QStringLiteral(R"re(<tr[^>]*>(.*?)</tr>)re"),
                QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatchIterator rows = rowRe.globalMatch(html);
            while (rows.hasNext()) {
                const QString row = rows.next().captured(1);

                const QRegularExpression linkRe(
                    QStringLiteral(R"re(<a[^>]+href\s*=\s*["'](/torrent/\d+/[^"']*)["'][^>]*>(.*?)</a>)re"),
                    QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch link = linkRe.match(row);
                if (!link.hasMatch())
                    continue;

                const QString title = stripHtml(link.captured(2));
                if (title.isEmpty())
                    continue;

                const QString rowText = stripHtml(row);
                const qint64 rowSize = parseHumanSize(rowText);

                bool exactHash = false;
                const QRegularExpression hashRe(
                    QStringLiteral(R"(magnet:[^"'<>]*?xt=urn:btih:([A-Fa-f0-9]{40}))"),
                    QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch hashMatch = hashRe.match(row);
                if (hashMatch.hasMatch())
                    exactHash = hashMatch.captured(1).compare(hash, Qt::CaseInsensitive) == 0;

                const int score
                    = releaseCandidateScore(torrentName, files, totalSize, title, rowSize, exactHash);
                if (score > bestScore) {
                    bestScore = score;
                    bestTitle = title;
                    bestSize = rowSize;
                    bestUrl = base + link.captured(1);
                }
            }

            // 260 requires either a near-identical size plus strong title/release
            // overlap, or an exact info-hash. This intentionally rejects a
            // generic movie title with the wrong encode.
            if (!bestUrl.isEmpty() && bestScore >= 260) {
                qInfo() << "RichMetadataResolver: release mirror candidate" << bestTitle.left(80)
                        << "score" << bestScore;

                QJsonObject reference;
                reference[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("Rutor release match"));
                reference[QStringLiteral("rutorUrl")] = bestUrl;
                reference[QStringLiteral("releaseReferenceUrl")] = bestUrl;
                reference[QStringLiteral("releaseReferenceTitle")] = bestTitle;
                reference[QStringLiteral("releaseMatchMethod")] = QStringLiteral("release fingerprint + total size");
                emit metadataFound(hash, reference);

                requestRutorDetail(hash, bestUrl, bestTitle, torrentName, totalSize, bestSize);
                return;
            }

            requestRutorMirror(hash, torrentName, totalSize, files, mirrors, mirrorIndex + 1);
        });
}

void RichMetadataResolver::requestRutorDetail(const QString& hash, const QString& candidateUrl,
    const QString& candidateTitle, const QString& torrentName, qint64 totalSize, qint64 candidateSize)
{
    QNetworkReply* reply = networkManager_->get(htmlRequest(QUrl(candidateUrl)));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, hash, candidateUrl, candidateTitle, torrentName, totalSize, candidateSize]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;

            const QString html = QString::fromUtf8(reply->readAll());
            if (html.trimmed().isEmpty())
                return;

            QString detailsHtml;
            const QRegularExpression detailsRe(
                QStringLiteral(R"re(<table[^>]*id\s*=\s*["']details["'][^>]*>(.*?)</table>)re"),
                QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch detailsMatch = detailsRe.match(html);
            if (detailsMatch.hasMatch())
                detailsHtml = detailsMatch.captured(1);
            else
                detailsHtml = html;

            QString description = stripHtml(detailsHtml);
            if (description.size() > 30000)
                description = description.left(30000) + QStringLiteral("…");
            if (description.size() < 80)
                return;

            QJsonObject patch;
            patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("Rutor exact release"));
            patch[QStringLiteral("description")] = description;
            patch[QStringLiteral("rutorUrl")] = candidateUrl;
            patch[QStringLiteral("releaseReferenceUrl")] = candidateUrl;
            patch[QStringLiteral("releaseReferenceTitle")] = candidateTitle;
            patch[QStringLiteral("releaseMatchMethod")] = QStringLiteral("release fingerprint + total size");

            const QJsonObject tech
                = metadata::extractTechnicalInfo(candidateTitle + QLatin1Char('\n') + description);
            if (!tech.isEmpty())
                patch[QStringLiteral("technicalInfo")] = tech;

            QString releaseLine = candidateTitle;
            if (candidateSize > 0 && totalSize > 0) {
                const double pct = qAbs(static_cast<double>(candidateSize - totalSize))
                    / static_cast<double>(totalSize) * 100.0;
                releaseLine += QStringLiteral("\nMatched size: %1% difference").arg(pct, 0, 'f', 2);
            }
            patch[QStringLiteral("releaseDetails")] = releaseLine;

            // Poster from the exact release page, if present.
            const QRegularExpression imgRe(
                QStringLiteral(R"re(<img[^>]+src\s*=\s*["']([^"']+)["'][^>]*>)re"),
                QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch img = imgRe.match(detailsHtml);
            if (img.hasMatch()) {
                QString poster = img.captured(1).trimmed();
                if (poster.startsWith(QStringLiteral("//")))
                    poster.prepend(QStringLiteral("https:"));
                else if (poster.startsWith(QLatin1Char('/'))) {
                    const QUrl base(candidateUrl);
                    poster = base.scheme() + QStringLiteral("://") + base.host() + poster;
                }
                if (poster.startsWith(QStringLiteral("http")))
                    patch[QStringLiteral("poster")] = poster;
            }

            qInfo() << "RichMetadataResolver: exact release description resolved from mirror for"
                    << hash.left(12) << candidateTitle.left(80);
            emit metadataFound(hash, patch);
            Q_UNUSED(torrentName);
        });
}

void RichMetadataResolver::requestWikipedia(const QString& hash, const QString& torrentName)
{
    QString title = metadata::cleanMediaTitle(torrentName);
    if (title.isEmpty())
        return;

    const int year = metadata::extractYear(torrentName);
    QString query = title;
    if (year > 0)
        query += QStringLiteral(" ") + QString::number(year);

    const bool hasCyrillic = title.contains(QRegularExpression(QStringLiteral("[\\x{0400}-\\x{04FF}]")));
    const QString host = hasCyrillic ? QStringLiteral("ru.wikipedia.org") : QStringLiteral("en.wikipedia.org");

    QUrl url(QStringLiteral("https://%1/w/api.php").arg(host));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("action"), QStringLiteral("query"));
    q.addQueryItem(QStringLiteral("generator"), QStringLiteral("search"));
    q.addQueryItem(QStringLiteral("gsrsearch"), query);
    q.addQueryItem(QStringLiteral("gsrlimit"), QStringLiteral("5"));
    q.addQueryItem(QStringLiteral("prop"), QStringLiteral("extracts|pageimages|info"));
    q.addQueryItem(QStringLiteral("exintro"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("explaintext"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("piprop"), QStringLiteral("original"));
    q.addQueryItem(QStringLiteral("inprop"), QStringLiteral("url"));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("formatversion"), QStringLiteral("2"));
    url.setQuery(q);

    QNetworkReply* reply = networkManager_->get(metadataRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash, torrentName]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError)
            return;

        const QJsonArray pages
            = document.object().value(QStringLiteral("query")).toObject().value(QStringLiteral("pages")).toArray();

        int bestScore = -1000;
        QJsonObject best;
        for (const QJsonValue& value : pages) {
            const QJsonObject page = value.toObject();
            QJsonObject candidate;
            candidate[QStringLiteral("name")] = page.value(QStringLiteral("title")).toString();

            const QString extract = page.value(QStringLiteral("extract")).toString();
            const int year = metadata::extractYear(page.value(QStringLiteral("title")).toString() + QLatin1Char(' ') + extract);
            if (year > 0)
                candidate[QStringLiteral("year")] = year;

            const int score = candidateScore(torrentName, candidate);
            if (score > bestScore) {
                bestScore = score;
                best = page;
            }
        }

        if (best.isEmpty() || bestScore < 55)
            return;

        const QString extract = best.value(QStringLiteral("extract")).toString().trimmed();
        const QString fullUrl = best.value(QStringLiteral("fullurl")).toString().trimmed();
        const QString poster
            = best.value(QStringLiteral("original")).toObject().value(QStringLiteral("source")).toString().trimmed();

        QJsonObject patch;
        patch[QStringLiteral("metadataSources")] = sourceArray(QStringLiteral("Wikipedia"));
        if (!extract.isEmpty())
            patch[QStringLiteral("synopsis")] = extract;
        if (!poster.isEmpty())
            patch[QStringLiteral("poster")] = poster;
        if (!fullUrl.isEmpty())
            patch[QStringLiteral("wikipediaUrl")] = fullUrl;

        if (!patch.isEmpty()) {
            qInfo() << "RichMetadataResolver: Wikipedia fallback for" << hash.left(12);
            emit metadataFound(hash, patch);
        }
    });
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
