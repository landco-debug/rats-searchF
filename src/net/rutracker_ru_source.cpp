#include "net/rutracker_ru_source.h"

#include "common/infohash.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>
#include <algorithm>

namespace rats::net {
namespace {

QString decodeEntities(QString text)
{
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
    text.replace(QChar(0x00A0), QLatin1Char(' '));
    return text;
}

QString htmlToText(QString html)
{
    html.replace(QRegularExpression(QStringLiteral("<span\\b[^>]*class\\s*=\\s*[\"'][^\"']*\\bpost-br\\b[^\"']*[\"'][^>]*>"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.replace(QRegularExpression(QStringLiteral("</t[dh]>"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral(": "));
    html.replace(QRegularExpression(QStringLiteral("</tr>"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.replace(QRegularExpression(
                     QStringLiteral("</(?:p|div|li|pre|h[1-6])>"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    html = decodeEntities(html);

    QStringList clean;
    const QStringList lines
        = html.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line.replace(QRegularExpression(QStringLiteral("[\\t ]+")), QStringLiteral(" "));
        line = line.trimmed();
        if (!line.isEmpty())
            clean.append(line);
    }
    return clean.join(QLatin1Char('\n'));
}

QUrl resolveUrl(const QUrl& base, const QString& href)
{
    QUrl target(href);
    if (target.isRelative())
        return base.resolved(target);
    if (target.scheme().isEmpty() && href.startsWith(QStringLiteral("//"))) {
        target.setScheme(base.scheme());
        return target;
    }
    return target;
}

QString firstMatch(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern,
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

int firstInteger(const QString& text)
{
    const QRegularExpression number(QStringLiteral(R"((\d+))"));
    const QRegularExpressionMatch m = number.match(htmlToText(text));
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

QVector<QString> tableCells(const QString& row)
{
    QVector<QString> cells;
    const QRegularExpression tdRe(
        QStringLiteral(R"(<td\b[^>]*>(.*?)</td>)"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    auto it = tdRe.globalMatch(row);
    while (it.hasNext())
        cells.append(it.next().captured(1));
    return cells;
}

qint64 sizeFromRow(const QString& row)
{
    const QVector<QString> cells = tableCells(row);
    if (cells.size() < 6)
        return 0;

    // Current RuTracker.RU markup keeps the sortable byte count in a hidden
    // <u> inside the sixth cell; Jackett uses the same field.
    const QString bytes = firstMatch(cells.at(5),
        QStringLiteral(R"(<u\b[^>]*>\s*(\d+)\s*</u>)"));
    bool ok = false;
    const qint64 value = bytes.toLongLong(&ok);
    return ok ? value : 0;
}

int counterFromClass(const QString& row, const QString& className)
{
    const QString klass = QRegularExpression::escape(className);
    const QRegularExpression re(
        QStringLiteral(R"(<td\b[^>]*class\s*=\s*(?:"[^"]*\b%1\b[^"]*"|'[^']*\b%1\b[^']*'|[^\s>]*\b%1\b[^\s>]*)[^>]*>(.*?)</td>)")
            .arg(klass),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(row);
    return m.hasMatch() ? firstInteger(m.captured(1)) : 0;
}

QJsonArray audioLines(const QString& description, bool audioRelease)
{
    QJsonArray out;
    const QStringList lines
        = description.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    const QRegularExpression explicitAudio(
        QStringLiteral("^\\s*(?:Audio|Аудио|Звук|Sound)\\s*#?\\d*\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression audioReleaseTechnical(
        QStringLiteral(
            "^\\s*(?:Формат(?:\\s*/\\s*Кодек)?|Format(?:\\s*/\\s*Codec)?|"
            "Формат\\s+аудио|Audio\\s+format|Аудиокодек|Аудио\\s+кодек|"
            "Audio\\s+codec|Кодек|Codec|Битрейт(?:\\s+аудио)?|Audio\\s+bitrate|"
            "Качество\\s+аудио|Audio\\s+quality|Тип\\s+рипа|Rip\\s+type)\\s*:"),
        QRegularExpression::CaseInsensitiveOption);

    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (explicitAudio.match(line).hasMatch()
            || (audioRelease && audioReleaseTechnical.match(line).hasMatch())) {
            out.append(line);
        }
    }
    return out;
}

QString firstPostBody(const QString& html)
{
    const QRegularExpression openRe(
        QStringLiteral(R"(<div\b[^>]*class\s*=\s*(?:"[^"]*\bpost_body\b[^"]*"|'[^']*\bpost_body\b[^']*')[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch open = openRe.match(html);
    if (!open.hasMatch())
        return QString();

    const int start = open.capturedEnd();

    // RuTracker terminates the first-post body with this explicit HTML marker.
    // Using it prevents technical lines from a later forum reply from ever
    // satisfying our strict-release completeness check.
    int end = html.indexOf(QStringLiteral("<!--/post_body-->"), start,
        Qt::CaseInsensitive);
    if (end < 0) {
        // Tolerate mirror/template changes but keep extraction bounded to the
        // first post area rather than the rest of the discussion.
        end = html.indexOf(QRegularExpression(
            QStringLiteral(R"(<tbody\b[^>]*id\s*=\s*["']post_\d+["'])"),
            QRegularExpression::CaseInsensitiveOption), start);
    }
    if (end < 0)
        end = qMin(html.size(), start + 180000);
    return html.mid(start, end - start);
}

bool isPublicRuTrackerUrl(const QUrl& url)
{
    return url.isValid()
        && url.host().compare(QStringLiteral("rutracker.ru"), Qt::CaseInsensitive) == 0
        && url.path().endsWith(QStringLiteral("/viewtopic.php"));
}

const QSet<int>& audioForums()
{
    static const QSet<int> ids = {
        730,776,777,1156,1158,1233,1159,1315,1223,1635,1637,1643,1636,1639,1640,1177,1642,1427,1641,
        1561,1598,1599,1600,1601,1200,1552,1565,1554,1553,1567,1566,1713,1556,1588,1580,1581,1582,
        1583,1584,1585,1586,1587,1602,1590,1591,1592,1593,1594,1595,1596,1597,1626,1627,1628,1610,
        1611,1457,1613,1614,1203,1615,1616,1617,1618,1205,1619,1620,1206,1575,1576,1577,1630,1631,
        1633,1540,1604,1562,1185,1183,1664,1665,1666,1667,1668,1670,1746,1669,1740,1679,1680,1681,
        1682,1683,1684,1685,1686,1687,1688,1689,1690,1691,1692,1693
    };
    return ids;
}
const QSet<int>& videoForums()
{
    static const QSet<int> ids = {
        1748,1757,1749,1758,1735,1736,1737,1738,1739,1695,1697,1696,1698,1699,1701,1702,1671,1677,
        1676,1675,1674,1673,1672,1656,1662,1661,1660,1659,1658,1657,1730,1731,1732,1733,1725,1726,
        1727,1728,1719,1720,1721,1722,1715,1734,1716,820,840,841,825,830,1317,838,845,1560,798,802,
        801,1318,1751,1752,1754,1756,1742,1743,1744,1745,1708,1710,1709,1711,1705,1086,1085,1551,
        1087,1703,1083,1082,1084,125,1353,1355,1352,1343,1025,8,1347,1348,1349,12,13
    };
    return ids;
}
const QSet<int>& bookForums(){ static const QSet<int> ids={726,728,761,760,757,1314,722,727,1021,1020}; return ids; }
const QSet<int>& gameForums(){ static const QSet<int> ids={60,73,61,1234,84,82,85,78,77,76,1538,1539,878}; return ids; }
const QSet<int>& softwareForums()
{
    static const QSet<int> ids = {
        105,1663,1120,706,212,210,213,215,1395,107,1405,1398,193,1518,195,341,196,969,1523,1505,201,
        1506,1508,1509,1507,108,217,218,222,1404,1522,1504,220,221,219,1511,1512,1513,1514,1515,1516,
        110,966,1500,1501,967,965,1499,1502,1503,968,1287,1307,1306,1305,1289,1302,1301,1298,1293,
        1292,1291,1294,1303,1300,1299,1296,1295
    };
    return ids;
}
domain::ContentType contentTypeForForum(int forumId)
{
    if (audioForums().contains(forumId)) return domain::ContentType::Audio;
    if (videoForums().contains(forumId)) return domain::ContentType::Video;
    if (bookForums().contains(forumId)) return domain::ContentType::Books;
    if (gameForums().contains(forumId)) return domain::ContentType::Games;
    if (softwareForums().contains(forumId)) return domain::ContentType::Software;
    return domain::ContentType::Unknown;
}
const QSet<int>* forumsForType(const QString& type)
{
    const QString key=type.trimmed().toLower();
    if(key==QStringLiteral("audio")) return &audioForums();
    if(key==QStringLiteral("video")) return &videoForums();
    if(key==QStringLiteral("books")) return &bookForums();
    if(key==QStringLiteral("games")) return &gameForums();
    if(key==QStringLiteral("software")) return &softwareForums();
    return nullptr;
}

} // namespace

int RuTrackerRuSource::sortColumn(const QString& sortKey)
{
    if (sortKey.startsWith(QStringLiteral("seeders_")))
        return 10;
    if (sortKey.startsWith(QStringLiteral("size_")))
        return 7;
    if (sortKey.startsWith(QStringLiteral("name_")))
        return 2;
    return 1; // registered/date
}

int RuTrackerRuSource::sortDirection(const QString& sortKey)
{
    return sortKey.endsWith(QStringLiteral("_asc")) ? 1 : 2;
}

QUrl RuTrackerRuSource::searchUrl(const QString& query, const QString& sortKey, const QString& contentType)
{
    QUrl url(QStringLiteral("http://rutracker.ru/tracker.php"));
    QUrlQuery q;
    if (const QSet<int>* forums=forumsForType(contentType); forums && !forums->isEmpty()) {
        QList<int> sorted=forums->values(); std::sort(sorted.begin(),sorted.end());
        for(int forumId:sorted) q.addQueryItem(QStringLiteral("f[]"),QString::number(forumId));
    } else {
        q.addQueryItem(QStringLiteral("f[]"),QStringLiteral("-1"));
    }
    q.addQueryItem(QStringLiteral("prev_allw"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("prev_a"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_dla"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_dlc"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_dld"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_dlw"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_my"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_new"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_sd"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_da"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("prev_dc"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_df"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("prev_ds"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("prev_tor_type"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("o"), QString::number(sortColumn(sortKey)));
    q.addQueryItem(QStringLiteral("s"), QString::number(sortDirection(sortKey)));
    q.addQueryItem(QStringLiteral("dc"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("df"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("da"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("ds"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("tm"), QStringLiteral("-1"));
    q.addQueryItem(QStringLiteral("sns"), QStringLiteral("-1"));
    q.addQueryItem(QStringLiteral("srg"), QStringLiteral("-1"));
    q.addQueryItem(QStringLiteral("allw"), QStringLiteral("0"));
    q.addQueryItem(QStringLiteral("nm"), query.trimmed());
    url.setQuery(q);
    return url;
}

QVector<domain::Torrent> RuTrackerRuSource::parseSearchPage(
    const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates)
{
    QVector<domain::Torrent> out;
    if (rawData.isEmpty() || maxCandidates <= 0)
        return out;

    const QString html = QString::fromUtf8(rawData);
    const QRegularExpression rowRe(
        QStringLiteral(R"(<tr\b[^>]*id\s*=\s*["']tor_(\d+)["'][^>]*>(.*?)</tr>)"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression detailRe(
        QStringLiteral(R"re(<a\b[^>]*href\s*=\s*["']([^"']*viewtopic\.php\?t=(\d+)[^"']*)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression magnetRe(
        QStringLiteral(R"re(href\s*=\s*["'](magnet:\?[^"']*xt=urn:btih:([A-Fa-f0-9]{40})[^"']*)["'])re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression forumRe(
        QStringLiteral(R"re(href\s*=\s*["'][^"']*tracker\.php\?f=(\d+)[^"']*["'])re"),
        QRegularExpression::CaseInsensitiveOption);

    QSet<QString> seen;
    auto rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QString row = rows.next().captured(2);
        const QRegularExpressionMatch detail = detailRe.match(row);
        const QRegularExpressionMatch magnet = magnetRe.match(row);
        if (!detail.hasMatch() || !magnet.hasMatch())
            continue;

        const QString hash = infohash::normalize(magnet.captured(2));
        if (!infohash::isValid(hash) || seen.contains(hash))
            continue;

        const QUrl sourceUrl = resolveUrl(pageUrl, detail.captured(1));
        if (!isPublicRuTrackerUrl(sourceUrl))
            continue;

        const QString name = htmlToText(detail.captured(3)).trimmed();
        if (name.isEmpty())
            continue;

        domain::Torrent torrent;
        torrent.hash = hash;
        torrent.name = name;
        torrent.size = sizeFromRow(row);
        torrent.seeders = counterFromClass(row, QStringLiteral("seedmed"));
        torrent.leechers = counterFromClass(row, QStringLiteral("leechmed"));

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
        info[QStringLiteral("sourceTopicId")] = detail.captured(2).toInt();
        info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
        const QRegularExpressionMatch forum = forumRe.match(row);
        if (forum.hasMatch()) {
            const int forumId = forum.captured(1).toInt();
            info[QStringLiteral("sourceForumId")] = forumId;
            torrent.contentType = contentTypeForForum(forumId);
            if (torrent.contentType != domain::ContentType::Unknown)
                info[QStringLiteral("contentTypeEvidence")]
                    = QStringLiteral("source-category");
        }
        info[QStringLiteral("sourceVerified")] = false;
        torrent.info = info;

        seen.insert(hash);
        out.append(std::move(torrent));
    }

    return out;
}

bool RuTrackerRuSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl)
{
    if (!torrent.isValid() || rawData.isEmpty())
        return false;

    const QString html = QString::fromUtf8(rawData);

    const QRegularExpression magnetRe(
        QStringLiteral(R"(xt=urn:btih:([A-Fa-f0-9]{40}))"),
        QRegularExpression::CaseInsensitiveOption);
    bool exactHash = false;
    auto magnets = magnetRe.globalMatch(html);
    while (magnets.hasNext()) {
        if (infohash::normalize(magnets.next().captured(1)) == torrent.hash) {
            exactHash = true;
            break;
        }
    }
    if (!exactHash)
        return false;

    const QString title = firstMatch(html,
        QStringLiteral(R"(<[^>]*id\s*=\s*["']topic-title["'][^>]*>(.*?)</[^>]+>)"));
    if (!title.isEmpty())
        torrent.name = htmlToText(title);

    QString postHtml = firstPostBody(html);
    if (postHtml.isEmpty())
        postHtml = html;
    const QString description = htmlToText(postHtml).trimmed();
    if (description.isEmpty())
        return false;

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    info[QStringLiteral("sourceVerified")] = true;
    info[QStringLiteral("description")] = description;

    QUrl sourceUrl = finalUrl;
    if (!isPublicRuTrackerUrl(sourceUrl))
        sourceUrl = QUrl(info.value(QStringLiteral("sourceUrl")).toString());
    if (!isPublicRuTrackerUrl(sourceUrl))
        return false;
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();

    QString quality = firstMatch(description,
        QStringLiteral(R"((?:Качество|Quality)\s*:\s*([^\n]+))"));
    if (quality.isEmpty()) {
        quality = firstMatch(torrent.name,
            QStringLiteral(R"(\b(2160p|1080p|720p|576p|480p|4K|UHD|BluRay|BDRip|BDRemux|REMUX|WEB[- .]?DL|WEBRip|HDLight)\b)"));
    }
    if (!quality.isEmpty())
        info[QStringLiteral("quality")] = quality;

    QString video = firstMatch(description,
        QStringLiteral(R"((?:Видео|Video)\s*:\s*([^\n]+))"));
    if (video.isEmpty()) {
        video = firstMatch(description,
            QStringLiteral(R"(\b((?:HEVC|H[ .]?265|x265|AVC|H[ .]?264|x264)[^\n]{0,160})\b)"));
    }
    if (!video.isEmpty()) {
        info[QStringLiteral("video")] = video;
        if (torrent.contentType == domain::ContentType::Unknown) torrent.contentType = domain::ContentType::Video;
    }

    const QJsonArray audio = audioLines(
        description, torrent.contentType == domain::ContentType::Audio);
    if (!audio.isEmpty())
        info[QStringLiteral("audioTracks")] = audio;

    const QString subtitles = firstMatch(description,
        QStringLiteral(R"((?:Субтитры|Subtitles?)\s*:\s*([^\n]+))"));
    if (!subtitles.isEmpty())
        info[QStringLiteral("subtitles")] = subtitles;

    const QString poster = firstMatch(postHtml,
        QStringLiteral(R"re(<img\b[^>]*class\s*=\s*["'][^"']*\bpostImg(?:Aligned)?\b[^"']*["'][^>]*(?:title|src)\s*=\s*["']([^"']+)["'])re"));
    if (!poster.isEmpty())
        info[QStringLiteral("poster")] = decodeEntities(poster);

    torrent.info = info;
    torrent.info[QStringLiteral("strictComplete")] = isStrictComplete(torrent);
    return true;
}

bool RuTrackerRuSource::isStrictComplete(const domain::Torrent& torrent)
{
    const QJsonObject& info = torrent.info;
    if (!torrent.isValid())
        return false;
    if (info.value(QStringLiteral("sourceProvider")).toString()
        != QStringLiteral("rutracker-ru"))
        return false;
    if (!info.value(QStringLiteral("sourceVerified")).toBool())
        return false;

    const QUrl sourceUrl(info.value(QStringLiteral("sourceUrl")).toString());
    if (!isPublicRuTrackerUrl(sourceUrl))
        return false;

    const QString description
        = info.value(QStringLiteral("description")).toString().trimmed();
    if (description.size() < 160)
        return false;

    const bool hasQuality
        = !info.value(QStringLiteral("quality")).toString().isEmpty();
    const bool hasVideo
        = !info.value(QStringLiteral("video")).toString().isEmpty();
    const bool hasAudio
        = !info.value(QStringLiteral("audioTracks")).toArray().isEmpty();

    // The forum id is RuTracker's own category signal. Once an exact
    // viewtopic page has re-proved the same info-hash, that source-native
    // category is stronger than optional wording inside the post body.
    if (torrent.contentType != domain::ContentType::Unknown
        && !info.value(QStringLiteral("contentTypeEvidence")).toString().isEmpty()) {
        return true;
    }

    if (torrent.contentType == domain::ContentType::Video
        || hasVideo || hasQuality) {
        return hasQuality && hasVideo && hasAudio;
    }
    if (torrent.contentType == domain::ContentType::Audio)
        return hasAudio;
    return true;
}

} // namespace rats::net
