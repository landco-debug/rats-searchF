#include "net/nnmclub_source.h"

#include "net/source_parse_utils.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace rats::net {
namespace {

int sortCode(const QString& sortKey)
{
    const QString key = sortKey.toLower();
    if (key.startsWith(QStringLiteral("seeders")))
        return 10;
    if (key.startsWith(QStringLiteral("size")))
        return 7;
    if (key.startsWith(QStringLiteral("name")))
        return 2;
    return 1; // created
}

int orderCode(const QString& sortKey)
{
    return sortKey.toLower().endsWith(QStringLiteral("_asc")) ? 1 : 2;
}

bool isNnmClubUrl(const QUrl& url)
{
    const QString host = url.host().toLower();
    return url.isValid()
        && (host == QStringLiteral("nnmclub.to")
            || host == QStringLiteral("www.nnmclub.to"))
        && url.path().endsWith(QStringLiteral("/forum/viewtopic.php"));
}

int topicIdFromUrl(const QUrl& url)
{
    QUrlQuery query(url);
    return query.queryItemValue(QStringLiteral("t")).toInt();
}

QString firstPostBody(const QString& html)
{
    const QRegularExpression openRe(
        QStringLiteral(
            R"re(<(?:div|span|td)\b[^>]*class\s*=\s*(?:"[^"]*\b(?:post_body|postbody)\b[^"]*"|'[^']*\b(?:post_body|postbody)\b[^']*')[^>]*>)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch open = openRe.match(html);
    if (!open.hasMatch())
        return QString();

    const int start = open.capturedEnd();
    int end = -1;
    const QStringList markers {
        QStringLiteral("<!--/post_body-->"),
        QStringLiteral("<!-- / postbody -->"),
        QStringLiteral("<!--/postbody-->")
    };
    for (const QString& marker : markers) {
        const int pos = html.indexOf(marker, start, Qt::CaseInsensitive);
        if (pos >= 0 && (end < 0 || pos < end))
            end = pos;
    }

    if (end < 0) {
        const QRegularExpression nextPost(
            QStringLiteral(
                R"re(<(?:div|span|td)\b[^>]*class\s*=\s*["'][^"']*\b(?:post_body|postbody)\b[^"']*["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch next = nextPost.match(html, start + 200);
        if (next.hasMatch())
            end = next.capturedStart();
    }
    if (end < 0)
        end = qMin(html.size(), start + 180000);
    return html.mid(start, end - start);
}

QString boundedDescription(const QString& html)
{
    QString block = firstPostBody(html);
    if (block.isEmpty())
        block = html;

    QString text = sourceparse::htmlToText(block).trimmed();
    if (text.size() > 50000)
        text = text.left(50000);
    return text;
}

} // namespace

QUrl NnmClubSource::searchUrl()
{
    return QUrl(QStringLiteral("https://nnmclub.to/forum/tracker.php"));
}

QByteArray NnmClubSource::searchBody(
    const QString& query, const QString& sortKey)
{
    QByteArray body;
    auto add = [&body](const QByteArray& key, const QByteArray& value) {
        if (!body.isEmpty())
            body.append('&');
        body.append(key);
        body.append('=');
        body.append(value);
    };

    add("f%5B%5D", "-1");
    add("o", QByteArray::number(sortCode(sortKey)));
    add("s", QByteArray::number(orderCode(sortKey)));
    add("tm", "-1");
    add("shf", "1");
    add("sha", "1");
    add("ta", "-1");
    add("sns", "-1");
    // Public NNM mode: only releases whose torrent download is available
    // without logging in. This avoids pretending that a login-only release can
    // satisfy our exact info-hash/file-list contract.
    add("sds", "4");
    add("nm", sourceparse::formEncodeWindows1251(query.trimmed()));
    add("submit", sourceparse::formEncodeWindows1251(
        QStringLiteral("Поиск")));
    return body;
}

QVector<domain::Torrent> NnmClubSource::parseSearchPage(
    const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates)
{
    QVector<domain::Torrent> out;
    if (rawData.isEmpty() || !pageUrl.isValid() || maxCandidates <= 0)
        return out;

    const QString html = sourceparse::decodeTrackerText(rawData);
    const QRegularExpression rowRe(
        QStringLiteral(R"re(<tr\b[^>]*>(.*?)</tr>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression detailRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*viewtopic\.php\?t=(\d+)[^"']*)["'][^>]*>\s*(?:<b[^>]*>)?(.*?)(?:</b>)?\s*</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression downloadRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*download\.php\?id=(\d+)[^"']*)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression forumRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*tracker\.php\?f=(\d+)[^"']*)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression seedRe(
        QStringLiteral(
            R"re(<td\b[^>]*class\s*=\s*["'][^"']*\bseedmed\b[^"']*["'][^>]*>.*?<b[^>]*>\s*(\d+)\s*</b>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression leechRe(
        QStringLiteral(
            R"re(<td\b[^>]*class\s*=\s*["'][^"']*\bleechmed\b[^"']*["'][^>]*>.*?<b[^>]*>\s*(\d+)\s*</b>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression underlinedNumberRe(
        QStringLiteral(R"re(<u\b[^>]*>\s*(\d+)\s*</u>)re"),
        QRegularExpression::CaseInsensitiveOption);

    QSet<int> seen;
    auto rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QString row = rows.next().captured(1);
        if (!row.contains(QStringLiteral("viewtopic.php?t="), Qt::CaseInsensitive)
            || !row.contains(QStringLiteral("download.php?id="), Qt::CaseInsensitive)) {
            continue;
        }

        const QRegularExpressionMatch detail = detailRe.match(row);
        const QRegularExpressionMatch download = downloadRe.match(row);
        if (!detail.hasMatch() || !download.hasMatch())
            continue;

        const int topicId = detail.captured(2).toInt();
        const int downloadId = download.captured(2).toInt();
        if (topicId <= 0 || downloadId <= 0 || seen.contains(topicId))
            continue;

        domain::Torrent torrent;
        torrent.name = sourceparse::stripHtml(detail.captured(3));
        if (torrent.name.isEmpty())
            continue;

        const QUrl detailUrl
            = sourceparse::resolveUrl(pageUrl, detail.captured(1));
        const QUrl torrentUrl
            = sourceparse::resolveUrl(pageUrl, download.captured(1));
        if (!detailUrl.isValid() || !torrentUrl.isValid())
            continue;

        const QRegularExpressionMatch seed = seedRe.match(row);
        const QRegularExpressionMatch leech = leechRe.match(row);
        if (seed.hasMatch())
            torrent.seeders = seed.captured(1).toInt();
        if (leech.hasMatch())
            torrent.leechers = leech.captured(1).toInt();

        QList<qint64> underlined;
        auto nums = underlinedNumberRe.globalMatch(row);
        while (nums.hasNext())
            underlined.append(nums.next().captured(1).toLongLong());
        if (!underlined.isEmpty()) {
            // NNM's result row wraps the byte size in <u>; the final <u> is the
            // Unix publish timestamp.
            if (underlined.first() > 1024)
                torrent.size = underlined.first();
            if (underlined.size() >= 2
                && underlined.last() > 1000000000LL) {
                torrent.added = QDateTime::fromSecsSinceEpoch(
                    underlined.last(), Qt::UTC);
            }
        }

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("nnmclub");
        info[QStringLiteral("sourceTopicId")] = topicId;
        info[QStringLiteral("sourceDownloadId")] = downloadId;
        info[QStringLiteral("sourceUrl")] = detailUrl.toString();
        info[QStringLiteral("sourceTorrentUrl")] = torrentUrl.toString();
        info[QStringLiteral("sourceVerified")] = false;

        const QRegularExpressionMatch forum = forumRe.match(row);
        if (forum.hasMatch()) {
            info[QStringLiteral("sourceForumId")] = forum.captured(2).toInt();
            const QString category
                = sourceparse::stripHtml(forum.captured(3));
            if (!category.isEmpty()) {
                info[QStringLiteral("sourceCategory")] = category;
                const domain::ContentType type
                    = sourceparse::contentTypeFromCategoryText(category);
                if (type != domain::ContentType::Unknown) {
                    torrent.contentType = type;
                    info[QStringLiteral("contentTypeEvidence")]
                        = QStringLiteral("source-category");
                }
            }
        }
        torrent.info = info;

        seen.insert(topicId);
        out.append(std::move(torrent));
    }

    return out;
}

bool NnmClubSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl)
{
    if (!torrent.isValid() || rawData.isEmpty())
        return false;

    QUrl sourceUrl = finalUrl;
    if (!isNnmClubUrl(sourceUrl))
        sourceUrl = QUrl(torrent.info.value(QStringLiteral("sourceUrl")).toString());
    if (!isNnmClubUrl(sourceUrl))
        return false;

    const int expectedTopic
        = torrent.info.value(QStringLiteral("sourceTopicId")).toInt();
    if (topicIdFromUrl(sourceUrl) != expectedTopic)
        return false;

    const QString html = sourceparse::decodeTrackerText(rawData);
    const int expectedDownload
        = torrent.info.value(QStringLiteral("sourceDownloadId")).toInt();
    const QRegularExpression downloadRe(
        QStringLiteral(
            R"re(href\s*=\s*["'][^"']*download\.php\?id=(\d+)[^"']*["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    bool sameDownload = false;
    auto downloads = downloadRe.globalMatch(html);
    while (downloads.hasNext()) {
        if (downloads.next().captured(1).toInt() == expectedDownload) {
            sameDownload = true;
            break;
        }
    }
    if (!sameDownload)
        return false;

    const QString description = boundedDescription(html);
    if (description.isEmpty())
        return false;

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("nnmclub");
    info[QStringLiteral("sourceVerified")] = true;
    const QString sourceMagnet = sourceparse::magnetForHash(html, torrent.hash);
    if (!sourceMagnet.isEmpty())
        info[QStringLiteral("sourceMagnet")] = sourceMagnet;
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
    info[QStringLiteral("description")] = description;
    torrent.info = info;

    sourceparse::populateTechnicalInfo(torrent);
    torrent.info[QStringLiteral("strictComplete")] = isStrictComplete(torrent);
    return true;
}

bool NnmClubSource::isStrictComplete(const domain::Torrent& torrent)
{
    if (!torrent.isValid())
        return false;
    const QJsonObject& info = torrent.info;
    if (info.value(QStringLiteral("sourceProvider")).toString()
        != QStringLiteral("nnmclub")) {
        return false;
    }
    if (!info.value(QStringLiteral("sourceVerified")).toBool())
        return false;
    if (!isNnmClubUrl(QUrl(info.value(QStringLiteral("sourceUrl")).toString())))
        return false;

    const QString description
        = info.value(QStringLiteral("description")).toString().trimmed();
    if (description.size() < 120)
        return false;

    if (torrent.contentType == domain::ContentType::Video) {
        return !info.value(QStringLiteral("quality")).toString().isEmpty()
            && !info.value(QStringLiteral("video")).toString().isEmpty()
            && !info.value(QStringLiteral("audioTracks")).toArray().isEmpty();
    }
    if (torrent.contentType == domain::ContentType::Audio)
        return !info.value(QStringLiteral("audioTracks")).toArray().isEmpty();

    return torrent.contentType != domain::ContentType::Unknown;
}

} // namespace rats::net
