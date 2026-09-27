#include "net/megapeer_source.h"

#include "common/infohash.h"
#include "net/source_parse_utils.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTime>
#include <QUrlQuery>

namespace rats::net {
namespace {

int sortCode(const QString& sortKey)
{
    const QString key = sortKey.toLower();
    if (key.startsWith(QStringLiteral("name")))
        return 1;
    if (key.startsWith(QStringLiteral("size")))
        return 2;
    // MegaPeer does not expose server-side seeder sorting. Prefer newest as
    // the least lossy fallback; the GUI/model still displays exact S/L values.
    return 0;
}

int orderCode(const QString& sortKey)
{
    return sortKey.toLower().endsWith(QStringLiteral("_asc")) ? 1 : 0;
}

QDateTime parseMegaPeerDate(QString text)
{
    text = sourceparse::stripHtml(text).trimmed();
    const QHash<QString, int> months {
        { QStringLiteral("янв"), 1 }, { QStringLiteral("фев"), 2 },
        { QStringLiteral("мар"), 3 }, { QStringLiteral("апр"), 4 },
        { QStringLiteral("мая"), 5 }, { QStringLiteral("май"), 5 },
        { QStringLiteral("июн"), 6 }, { QStringLiteral("июл"), 7 },
        { QStringLiteral("авг"), 8 }, { QStringLiteral("сен"), 9 },
        { QStringLiteral("окт"), 10 }, { QStringLiteral("ноя"), 11 },
        { QStringLiteral("дек"), 12 }
    };
    const QRegularExpression re(
        QStringLiteral(R"((\d{1,2})\s+([\p{L}]+)\s+(\d{2,4}))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return {};

    const QString mon = m.captured(2).left(3).toLower();
    const int month = months.value(mon);
    int year = m.captured(3).toInt();
    if (year < 100)
        year += year >= 70 ? 1900 : 2000;
    const QDate date(year, month, m.captured(1).toInt());
    return date.isValid() ? QDateTime(date, QTime(0, 0), Qt::UTC) : QDateTime();
}

QString relevantDescription(const QString& html, const QString& torrentName)
{
    QString text = sourceparse::htmlToText(html);
    int start = -1;
    const QStringList markers {
        QStringLiteral("Информация о фильме"),
        QStringLiteral("Информация о раздаче"),
        QStringLiteral("Название:"),
        QStringLiteral("Описание:"),
        QStringLiteral("Качество:")
    };
    for (const QString& marker : markers) {
        const int pos = text.indexOf(marker, 0, Qt::CaseInsensitive);
        if (pos >= 0 && (start < 0 || pos < start))
            start = pos;
    }
    if (start < 0 && !torrentName.isEmpty())
        start = text.indexOf(torrentName, 0, Qt::CaseInsensitive);
    if (start > 0)
        text = text.mid(start);

    const QStringList endings {
        QStringLiteral("\nКомментарии"),
        QStringLiteral("\nДобавить комментарий"),
        QStringLiteral("\nПохожие торренты")
    };
    int end = -1;
    for (const QString& marker : endings) {
        const int pos = text.indexOf(marker, 0, Qt::CaseInsensitive);
        if (pos > 0 && (end < 0 || pos < end))
            end = pos;
    }
    if (end > 0)
        text = text.left(end);
    if (text.size() > 40000)
        text = text.left(40000);
    return text.trimmed();
}

int topicIdFromMegaPeerUrl(const QUrl& url)
{
    if (!url.isValid()
        || url.host().compare(QStringLiteral("megapeer.vip"), Qt::CaseInsensitive) != 0) {
        return 0;
    }
    const QRegularExpression topicRe(
        QStringLiteral(R"(^/torrent/(\d+)(?:/|$))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch topic = topicRe.match(url.path());
    return topic.hasMatch() ? topic.captured(1).toInt() : 0;
}

bool findDirectTorrentLink(
    const QString& html, const QUrl& baseUrl, QUrl* urlOut, int* idOut)
{
    if (urlOut)
        *urlOut = {};
    if (idOut)
        *idOut = 0;

    // Current MegaPeer uses /download/<id>/..., while older/current mirrors
    // have also used download.php?id=<id> / download2.php?id=<id>. Accept
    // either shape only when it is linked from this exact detail/search page.
    const QRegularExpression pathRe(
        QStringLiteral(
            R"re(href\s*=\s*["']([^"']*/download/(\d+)(?:/[^"']*)?)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = pathRe.match(html);
    QString href;
    int id = 0;
    if (match.hasMatch()) {
        href = match.captured(1);
        id = match.captured(2).toInt();
    } else {
        const QRegularExpression phpRe(
            QStringLiteral(
                R"re(href\s*=\s*["']([^"']*download(?:2)?\.php\?[^"']*\bid=(\d+)[^"']*)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        match = phpRe.match(html);
        if (match.hasMatch()) {
            href = match.captured(1);
            id = match.captured(2).toInt();
        }
    }

    if (href.isEmpty() || id <= 0)
        return false;

    const QUrl url = sourceparse::resolveUrl(
        baseUrl, sourceparse::decodeEntities(href));
    if (!url.isValid()
        || url.host().compare(QStringLiteral("megapeer.vip"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    if (urlOut)
        *urlOut = url;
    if (idOut)
        *idOut = id;
    return true;
}

} // namespace

QUrl MegaPeerSource::searchUrl(const QString& query, const QString& sortKey)
{
    QByteArray url("https://megapeer.vip/browse.php?search=");
    url += sourceparse::percentEncodeWindows1251(query.trimmed());
    url += "&age=&cat=0&stype=0&sort=";
    url += QByteArray::number(sortCode(sortKey));
    url += "&ascdesc=";
    url += QByteArray::number(orderCode(sortKey));
    return QUrl::fromEncoded(url);
}

QVector<domain::Torrent> MegaPeerSource::parseSearchPage(
    const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates)
{
    QVector<domain::Torrent> out;
    if (rawData.isEmpty() || !pageUrl.isValid() || maxCandidates <= 0)
        return out;

    const QString html = sourceparse::decodeTrackerText(rawData);
    const QRegularExpression rowRe(
        QStringLiteral(
            R"re(<tr\b[^>]*class\s*=\s*["'][^"']*\btable_fon\b[^"']*["'][^>]*>(.*?)</tr>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression detailRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*/torrent/(\d+)(?:/[^"']*)?)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression cellRe(
        QStringLiteral(R"re(<td\b[^>]*>(.*?)</td>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression fontRe(
        QStringLiteral(R"re(<font\b[^>]*>(.*?)</font>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);

    QSet<int> seen;
    auto rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QString row = rows.next().captured(1);
        const QRegularExpressionMatch detail = detailRe.match(row);
        if (!detail.hasMatch())
            continue;

        const int topicId = detail.captured(2).toInt();
        if (topicId <= 0 || seen.contains(topicId))
            continue;

        domain::Torrent torrent;
        torrent.name = sourceparse::stripHtml(detail.captured(3));
        if (torrent.name.isEmpty())
            continue;

        const QUrl detailUrl
            = sourceparse::resolveUrl(pageUrl, detail.captured(1));
        if (topicIdFromMegaPeerUrl(detailUrl) != topicId)
            continue;

        QStringList cells;
        auto cellIt = cellRe.globalMatch(row);
        while (cellIt.hasNext())
            cells.append(cellIt.next().captured(1));

        if (cells.size() >= 2) {
            torrent.added = parseMegaPeerDate(cells.first());
            torrent.size = sourceparse::parseSize(cells.at(cells.size() - 2));

            QList<int> swarm;
            auto fontIt = fontRe.globalMatch(cells.last());
            while (fontIt.hasNext()) {
                bool ok = false;
                const int value = sourceparse::stripHtml(
                    fontIt.next().captured(1)).toInt(&ok);
                if (ok)
                    swarm.append(value);
            }
            if (swarm.size() >= 2) {
                torrent.seeders = swarm.at(0);
                torrent.leechers = swarm.at(1);
            }
        }

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("megapeer");
        info[QStringLiteral("sourceTopicId")] = topicId;
        info[QStringLiteral("sourceUrl")] = detailUrl.toString();
        info[QStringLiteral("sourceVerified")] = false;

        QUrl torrentUrl;
        int downloadId = 0;
        if (findDirectTorrentLink(row, pageUrl, &torrentUrl, &downloadId)) {
            info[QStringLiteral("sourceDownloadId")] = downloadId;
            info[QStringLiteral("sourceTorrentUrl")] = torrentUrl.toString();
        }

        torrent.info = info;
        seen.insert(topicId);
        out.append(std::move(torrent));
    }
    return out;
}

bool MegaPeerSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl)
{
    if (rawData.isEmpty())
        return false;

    const int expectedTopic
        = torrent.info.value(QStringLiteral("sourceTopicId")).toInt();
    if (expectedTopic <= 0)
        return false;

    const int finalTopic = topicIdFromMegaPeerUrl(finalUrl);
    if (finalTopic > 0 && finalTopic != expectedTopic)
        return false;

    QUrl sourceUrl = finalTopic == expectedTopic
        ? finalUrl
        : QUrl(torrent.info.value(QStringLiteral("sourceUrl")).toString());
    if (topicIdFromMegaPeerUrl(sourceUrl) != expectedTopic)
        return false;

    const QString html = sourceparse::decodeTrackerText(rawData);
    QJsonObject info = torrent.info;

    QUrl pageTorrentUrl;
    int pageDownloadId = 0;
    if (findDirectTorrentLink(
            html, sourceUrl, &pageTorrentUrl, &pageDownloadId)) {
        const int expectedDownload
            = info.value(QStringLiteral("sourceDownloadId")).toInt();
        if (expectedDownload > 0 && pageDownloadId != expectedDownload)
            return false;
        info[QStringLiteral("sourceDownloadId")] = pageDownloadId;
        info[QStringLiteral("sourceTorrentUrl")] = pageTorrentUrl.toString();
    }

    const QRegularExpression magnetRe(
        QStringLiteral(R"(xt=urn:btih:([A-Fa-f0-9]{40}))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch magnet = magnetRe.match(html);
    bool verified = false;
    if (magnet.hasMatch()) {
        const QString hash = infohash::normalize(magnet.captured(1));
        if (!infohash::isValid(hash))
            return false;
        if (!torrent.hash.isEmpty()
            && torrent.hash.compare(hash, Qt::CaseInsensitive) != 0) {
            return false;
        }
        torrent.hash = hash;
        verified = true;
        info[QStringLiteral("identityEvidence")]
            = QStringLiteral("detail-magnet");
    } else if (torrent.isValid()) {
        // The client may have parsed the direct .torrent linked by this same
        // exact page. Topic/download provenance was checked above.
        verified = true;
        info[QStringLiteral("identityEvidence")]
            = QStringLiteral("torrent-fallback");
    } else {
        info[QStringLiteral("identityEvidence")]
            = QStringLiteral("pending-torrent-fallback");
    }

    const QString description = relevantDescription(html, torrent.name);
    if (description.isEmpty())
        return false;

    info[QStringLiteral("sourceProvider")] = QStringLiteral("megapeer");
    info[QStringLiteral("sourceVerified")] = verified;
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
    info[QStringLiteral("description")] = description;

    const QRegularExpression categoryRe(
        QStringLiteral(
            R"((?:Категория|Раздел|Category)\s*:\s*([^\n]+))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch category = categoryRe.match(description);
    if (category.hasMatch()) {
        const QString categoryText = category.captured(1).trimmed();
        info[QStringLiteral("sourceCategory")] = categoryText;
        const domain::ContentType type
            = sourceparse::contentTypeFromCategoryText(categoryText);
        if (type != domain::ContentType::Unknown) {
            torrent.contentType = type;
            info[QStringLiteral("contentTypeEvidence")]
                = QStringLiteral("source-category");
        }
    }

    torrent.info = info;
    sourceparse::populateTechnicalInfo(torrent);
    torrent.info[QStringLiteral("strictComplete")] = isStrictComplete(torrent);
    return true;
}

bool MegaPeerSource::isStrictComplete(const domain::Torrent& torrent)
{
    if (!torrent.isValid())
        return false;
    const QJsonObject& info = torrent.info;
    if (info.value(QStringLiteral("sourceProvider")).toString()
        != QStringLiteral("megapeer")) {
        return false;
    }
    if (!info.value(QStringLiteral("sourceVerified")).toBool())
        return false;
    if (topicIdFromMegaPeerUrl(
            QUrl(info.value(QStringLiteral("sourceUrl")).toString()))
        != info.value(QStringLiteral("sourceTopicId")).toInt()) {
        return false;
    }

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
