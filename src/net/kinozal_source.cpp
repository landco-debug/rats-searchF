#include "net/kinozal_source.h"

#include "common/infohash.h"
#include "domain/content_classifier.h"
#include "net/source_parse_utils.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTime>
#include <QUrlQuery>

#include <limits>

namespace rats::net {
namespace {

bool isKinozalHost(QString host)
{
    host = host.toLower();
    if (host.startsWith(QStringLiteral("www.")))
        host.remove(0, 4);
    return host == QStringLiteral("kinozal.me")
        || host == QStringLiteral("kinozal.guru");
}

int detailIdFromUrl(const QUrl& url)
{
    if (!url.isValid() || !isKinozalHost(url.host())
        || url.path() != QStringLiteral("/details.php")) {
        return 0;
    }
    QUrlQuery query(url);
    return query.queryItemValue(QStringLiteral("id")).toInt();
}

int sortCode(const QString& sortKey)
{
    const QString key = sortKey.toLower();
    if (key.startsWith(QStringLiteral("seeders")))
        return 1;
    if (key.startsWith(QStringLiteral("size")))
        return 3;
    return 0;
}

int orderCode(const QString& sortKey)
{
    return sortKey.toLower().endsWith(QStringLiteral("_asc")) ? 1 : 0;
}

QString normalizedSizeText(QString text)
{
    text.replace(QStringLiteral("ТБ"), QStringLiteral("TB"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("ГБ"), QStringLiteral("GB"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("МБ"), QStringLiteral("MB"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("КБ"), QStringLiteral("KB"), Qt::CaseInsensitive);
    return text;
}

QDateTime parseKinozalDate(const QString& raw)
{
    const QString text = raw.trimmed().toLower();
    QDate date;
    const QDate today = QDate::currentDate();

    if (text.contains(QStringLiteral("сегодня")))
        date = today;
    else if (text.contains(QStringLiteral("вчера")))
        date = today.addDays(-1);
    else {
        const QRegularExpression re(
            QStringLiteral(R"((\d{1,2})\.(\d{1,2})\.(\d{4}))"));
        const QRegularExpressionMatch m = re.match(text);
        if (m.hasMatch())
            date = QDate(m.captured(3).toInt(),
                m.captured(2).toInt(), m.captured(1).toInt());
    }

    if (!date.isValid())
        return {};
    return QDateTime(date, QTime(12, 0), Qt::LocalTime).toUTC();
}

void applyCategory(int id, domain::Torrent& torrent)
{
    using domain::ContentCategory;
    using domain::ContentType;

    switch (id) {
    case 8: case 6: case 15: case 17: case 35: case 39:
    case 13: case 14: case 24: case 11: case 10: case 9:
    case 47: case 18: case 37: case 12: case 7: case 16:
    case 48: case 49: case 50:
        torrent.contentType = ContentType::Video;
        torrent.contentCategory = ContentCategory::Movie;
        break;
    case 45: case 46:
        torrent.contentType = ContentType::Video;
        torrent.contentCategory = ContentCategory::Series;
        break;
    case 20:
        torrent.contentType = ContentType::Video;
        torrent.contentCategory = ContentCategory::Anime;
        break;
    case 21: case 22:
        torrent.contentType = ContentType::Video;
        break;
    case 1:
        torrent.contentType = ContentType::Video;
        break;
    case 2:
        torrent.contentType = ContentType::Audio;
        torrent.contentCategory = ContentCategory::Ebook;
        break;
    case 3: case 4: case 5: case 42:
        torrent.contentType = ContentType::Audio;
        torrent.contentCategory = ContentCategory::Music;
        break;
    case 23:
        torrent.contentType = ContentType::Games;
        torrent.contentCategory = ContentCategory::Game;
        break;
    case 32:
        torrent.contentType = ContentType::Software;
        torrent.contentCategory = ContentCategory::Software;
        break;
    case 40:
        torrent.contentType = ContentType::Pictures;
        break;
    case 41:
        torrent.contentType = ContentType::Books;
        torrent.contentCategory = ContentCategory::Ebook;
        break;
    default:
        break;
    }
}

QString fullPageDescription(const QString& html)
{
    QString text = sourceparse::htmlToText(html).trimmed();
    if (text.size() > 50000)
        text = text.left(50000);
    return text;
}

} // namespace

QUrl KinozalSource::searchUrl(
    const QUrl& baseUrl, const QString& query, const QString& sortKey)
{
    if (!baseUrl.isValid() || !isKinozalHost(baseUrl.host()))
        return {};

    const QByteArray encoded = baseUrl.scheme().toLatin1()
        + QByteArray("://") + baseUrl.host().toLatin1()
        + QByteArray("/browse.php?s=")
        + sourceparse::formEncodeWindows1251(query.trimmed())
        + QByteArray("&g=0&c=0&v=0&d=0&w=0&t=")
        + QByteArray::number(sortCode(sortKey))
        + QByteArray("&f=") + QByteArray::number(orderCode(sortKey));
    return QUrl::fromEncoded(encoded);
}

QVector<domain::Torrent> KinozalSource::parseSearchPage(
    const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates)
{
    QVector<domain::Torrent> out;
    if (rawData.isEmpty() || !pageUrl.isValid() || maxCandidates <= 0
        || !isKinozalHost(pageUrl.host())) {
        return out;
    }

    const QString html = sourceparse::decodeTrackerText(rawData);
    const QRegularExpression rowRe(
        QStringLiteral(R"re(<tr\b[^>]*>(.*?)</tr>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression detailRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*details\.php\?id=(\d+)[^"']*)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression sizeRe(
        QStringLiteral(
            R"re(<td\b[^>]*>\s*([0-9]+(?:[.,][0-9]+)?\s*(?:КБ|МБ|ГБ|ТБ|KB|MB|GB|TB))\s*</td>)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression seedRe(
        QStringLiteral(
            R"re(<td\b[^>]*class\s*=\s*["'][^"']*\bsl_s\b[^"']*["'][^>]*>\s*(\d+)\s*</td>)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression leechRe(
        QStringLiteral(
            R"re(<td\b[^>]*class\s*=\s*["'][^"']*\bsl_p\b[^"']*["'][^>]*>\s*(\d+)\s*</td>)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression categoryRe(
        QStringLiteral(R"re(onclick\s*=\s*["']\s*cat\((\d+)\))re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression cellRe(
        QStringLiteral(R"re(<td\b[^>]*>(.*?)</td>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);

    QSet<int> seenIds;
    auto rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QString row = rows.next().captured(1);
        const QRegularExpressionMatch detail = detailRe.match(row);
        if (!detail.hasMatch())
            continue;

        const int topicId = detail.captured(2).toInt();
        if (topicId <= 0 || seenIds.contains(topicId))
            continue;

        const QUrl detailUrl
            = sourceparse::resolveUrl(pageUrl, detail.captured(1));
        if (!isExactDetailUrl(detailUrl, topicId))
            continue;

        const QString title
            = sourceparse::stripHtml(detail.captured(3)).trimmed();
        if (title.isEmpty())
            continue;

        domain::Torrent torrent;
        torrent.name = title;

        const QRegularExpressionMatch size = sizeRe.match(row);
        if (size.hasMatch()) {
            torrent.size = sourceparse::parseSize(
                normalizedSizeText(size.captured(1)));
        }

        const QRegularExpressionMatch seed = seedRe.match(row);
        const QRegularExpressionMatch leech = leechRe.match(row);
        if (seed.hasMatch())
            torrent.seeders = seed.captured(1).toInt();
        if (leech.hasMatch())
            torrent.leechers = leech.captured(1).toInt();

        auto cells = cellRe.globalMatch(row);
        while (cells.hasNext()) {
            const QString cellText
                = sourceparse::stripHtml(cells.next().captured(1));
            const QDateTime parsed = parseKinozalDate(cellText);
            if (parsed.isValid()) {
                torrent.added = parsed;
                break;
            }
        }

        int categoryId = 0;
        const QRegularExpressionMatch category = categoryRe.match(row);
        if (category.hasMatch()) {
            categoryId = category.captured(1).toInt();
            applyCategory(categoryId, torrent);
        }

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("kinozal");
        info[QStringLiteral("sourceTopicId")] = topicId;
        info[QStringLiteral("sourceUrl")] = detailUrl.toString();
        info[QStringLiteral("sourceVerified")] = false;
        info[QStringLiteral("detailVerified")] = false;
        if (categoryId > 0) {
            info[QStringLiteral("sourceCategoryId")] = categoryId;
            if (torrent.contentType != domain::ContentType::Unknown) {
                info[QStringLiteral("contentTypeEvidence")]
                    = QStringLiteral("source-category");
            }
        }
        torrent.info = info;

        seenIds.insert(topicId);
        out.append(std::move(torrent));
    }

    return out;
}

bool KinozalSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData,
    const QUrl& finalUrl)
{
    if (rawData.isEmpty())
        return false;

    const int expectedId
        = torrent.info.value(QStringLiteral("sourceTopicId")).toInt();
    QUrl sourceUrl = finalUrl;
    if (!isExactDetailUrl(sourceUrl, expectedId)) {
        sourceUrl = QUrl(
            torrent.info.value(QStringLiteral("sourceUrl")).toString());
    }
    if (!isExactDetailUrl(sourceUrl, expectedId))
        return false;

    const QString html = sourceparse::decodeTrackerText(rawData);
    if (!html.contains(QStringLiteral("Кинозал"), Qt::CaseInsensitive)
        && !html.contains(QStringLiteral("Kinozal"), Qt::CaseInsensitive)) {
        return false;
    }

    const QString description = fullPageDescription(html);
    if (description.size() < 80)
        return false;

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("kinozal");
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
    info[QStringLiteral("detailVerified")] = true;
    info[QStringLiteral("description")] = description;
    torrent.info = info;

    sourceparse::populateTechnicalInfo(torrent);
    torrent.info[QStringLiteral("strictComplete")] = false;
    return true;
}

bool KinozalSource::applyServerDetails(
    domain::Torrent& torrent, const QByteArray& rawData)
{
    if (rawData.isEmpty()
        || !torrent.info.value(QStringLiteral("detailVerified")).toBool()) {
        return false;
    }

    const QString html = sourceparse::decodeTrackerText(rawData);
    const QString text = sourceparse::htmlToText(html);

    // Prefer the labelled form, but do not make exact identity depend on the
    // encoding of the Cyrillic label. Kinozal's own UI and Jackett both treat
    // the first <li> of get_srv_details.php as the authoritative info-hash.
    QString capturedHash;
    const QRegularExpression labelledHashRe(
        QStringLiteral(
            R"((?:Инфо\s*хеш|Info\s*hash)\s*:\s*([A-Fa-f0-9]{40}))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch labelled = labelledHashRe.match(text);
    if (labelled.hasMatch()) {
        capturedHash = labelled.captured(1);
    } else {
        const QRegularExpression firstLiRe(
            QStringLiteral(R"re(<li\b[^>]*>(.*?)</li>)re"),
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch firstLi = firstLiRe.match(html);
        if (firstLi.hasMatch()) {
            const QString firstLiText
                = sourceparse::stripHtml(firstLi.captured(1));
            const QRegularExpression tokenRe(
                QStringLiteral(R"(\b([A-Fa-f0-9]{40})\b)"));
            const QRegularExpressionMatch token
                = tokenRe.match(firstLiText);
            if (token.hasMatch())
                capturedHash = token.captured(1);
        }
    }
    if (capturedHash.isEmpty())
        return false;

    const QString hash = infohash::normalize(capturedHash);
    if (!infohash::isValid(hash))
        return false;
    if (!torrent.hash.isEmpty() && torrent.hash != hash)
        return false;
    torrent.hash = hash;

    const QRegularExpression pieceRe(
        QStringLiteral(
            R"((?:Размер\s+части\s+торрента|Piece\s+size)\s*:\s*([^\n]+))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch piece = pieceRe.match(text);
    if (piece.hasMatch()) {
        const qint64 bytes = sourceparse::parseSize(
            normalizedSizeText(piece.captured(1)));
        if (bytes > 0
            && bytes <= std::numeric_limits<int>::max()) {
            torrent.pieceLength = static_cast<int>(bytes);
        }
    }

    QVector<domain::File> files;
    qint64 totalSize = 0;
    const QRegularExpression fileRe(
        QStringLiteral(
            R"re(<div\b[^>]*class\s*=\s*["'][^"']*\bing\b[^"']*["'][^>]*>(.*?)<i\b[^>]*>(.*?)</i>.*?</div>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression bytesRe(
        QStringLiteral(R"(\((\d+)\))"));

    auto it = fileRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString path
            = sourceparse::stripHtml(m.captured(1)).trimmed();
        const QString sizeText
            = sourceparse::stripHtml(m.captured(2)).trimmed();
        if (path.isEmpty())
            continue;

        qint64 bytes = 0;
        const QRegularExpressionMatch rawBytes
            = bytesRe.match(sizeText);
        if (rawBytes.hasMatch())
            bytes = rawBytes.captured(1).toLongLong();
        if (bytes <= 0) {
            bytes = sourceparse::parseSize(
                normalizedSizeText(sizeText));
        }
        if (bytes < 0)
            bytes = 0;

        files.append(domain::File { path, bytes });
        totalSize += bytes;
    }

    if (!files.isEmpty()) {
        torrent.fileList = files;
        torrent.files = files.size();
        if (torrent.size <= 0 && totalSize > 0)
            torrent.size = totalSize;

        if (torrent.contentType == domain::ContentType::Unknown) {
            const domain::Classification classification
                = domain::ContentClassifier::classify(
                    torrent.name, files);
            torrent.contentType = classification.type;
            torrent.contentCategory = classification.category;
            if (torrent.contentType != domain::ContentType::Unknown) {
                torrent.info[QStringLiteral("contentTypeEvidence")]
                    = QStringLiteral("server-file-list");
            }
        }
    }

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("kinozal");
    info[QStringLiteral("sourceVerified")] = true;
    info[QStringLiteral("identityEvidence")]
        = QStringLiteral("kinozal-get-srv-details");
    torrent.info = info;
    torrent.info[QStringLiteral("strictComplete")]
        = isStrictComplete(torrent);
    return torrent.info
        .value(QStringLiteral("strictComplete")).toBool();
}

bool KinozalSource::isExactDetailUrl(
    const QUrl& url, int expectedId)
{
    const int id = detailIdFromUrl(url);
    if (id <= 0)
        return false;
    return expectedId <= 0 || id == expectedId;
}

bool KinozalSource::isStrictComplete(
    const domain::Torrent& torrent)
{
    if (!torrent.isValid())
        return false;

    const QJsonObject& info = torrent.info;
    if (info.value(QStringLiteral("sourceProvider")).toString()
        != QStringLiteral("kinozal")) {
        return false;
    }
    if (!info.value(QStringLiteral("sourceVerified")).toBool()
        || !info.value(QStringLiteral("detailVerified")).toBool()) {
        return false;
    }

    const int expectedId
        = info.value(QStringLiteral("sourceTopicId")).toInt();
    if (!isExactDetailUrl(
            QUrl(info.value(QStringLiteral("sourceUrl")).toString()),
            expectedId)) {
        return false;
    }

    const QString description
        = info.value(QStringLiteral("description"))
              .toString().trimmed();
    return expectedId > 0 && description.size() >= 80;
}

} // namespace rats::net
