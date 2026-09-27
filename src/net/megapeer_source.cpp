#include "net/megapeer_source.h"

#include "net/source_parse_utils.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

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

bool isMegaPeerUrl(const QUrl& url)
{
    return url.isValid()
        && url.host().compare(QStringLiteral("megapeer.vip"), Qt::CaseInsensitive) == 0
        && url.path().startsWith(QStringLiteral("/torrent/"));
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
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*/torrent/(\d+)/[^"']*)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression downloadRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*/download/(\d+)/[^"']*)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
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
        info[QStringLiteral("sourceDownloadId")] = downloadId;
        info[QStringLiteral("sourceUrl")] = detailUrl.toString();
        info[QStringLiteral("sourceTorrentUrl")] = torrentUrl.toString();
        info[QStringLiteral("sourceVerified")] = false;
        torrent.info = info;

        seen.insert(topicId);
        out.append(std::move(torrent));
    }
    return out;
}

bool MegaPeerSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl)
{
    if (!torrent.isValid() || rawData.isEmpty())
        return false;

    QUrl sourceUrl = finalUrl;
    if (!isMegaPeerUrl(sourceUrl))
        sourceUrl = QUrl(torrent.info.value(QStringLiteral("sourceUrl")).toString());
    if (!isMegaPeerUrl(sourceUrl))
        return false;

    const int expectedTopic
        = torrent.info.value(QStringLiteral("sourceTopicId")).toInt();
    const QRegularExpression topicRe(QStringLiteral(R"(/torrent/(\d+)(?:/|$))"));
    const QRegularExpressionMatch topic = topicRe.match(sourceUrl.path());
    if (!topic.hasMatch() || topic.captured(1).toInt() != expectedTopic)
        return false;

    // The exact page must expose the same download id that was paired with this
    // detail URL in the search row. Its .torrent has already supplied the hash.
    const QString html = sourceparse::decodeTrackerText(rawData);
    const int expectedDownload
        = torrent.info.value(QStringLiteral("sourceDownloadId")).toInt();
    const QRegularExpression downloadRe(
        QStringLiteral(R"re(href\s*=\s*["'][^"']*/download/(\d+)/[^"']*["'])re"),
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

    const QString description = relevantDescription(html, torrent.name);
    if (description.isEmpty())
        return false;

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("megapeer");
    info[QStringLiteral("sourceVerified")] = true;
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
    if (!isMegaPeerUrl(QUrl(info.value(QStringLiteral("sourceUrl")).toString())))
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
