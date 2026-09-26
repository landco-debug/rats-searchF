#include "net/rutor_source.h"

#include "common/infohash.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

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
    // Preserve table label/value structure: Rutor release descriptions put
    // quality, video and audio facts into rows/cells.
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

qint64 parseSize(QString text)
{
    text = decodeEntities(text);
    QRegularExpression re(
        QStringLiteral(R"((\d+(?:[.,]\d+)?)\s*(TB|GB|MB|KB|B)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return 0;

    QString number = m.captured(1);
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    const double value = number.toDouble();
    const QString unit = m.captured(2).toUpper();

    qint64 multiplier = 1;
    if (unit == QStringLiteral("KB"))
        multiplier = 1024LL;
    else if (unit == QStringLiteral("MB"))
        multiplier = 1024LL * 1024;
    else if (unit == QStringLiteral("GB"))
        multiplier = 1024LL * 1024 * 1024;
    else if (unit == QStringLiteral("TB"))
        multiplier = 1024LL * 1024 * 1024 * 1024;
    return static_cast<qint64>(value * static_cast<double>(multiplier));
}

int firstInteger(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern,
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

QString firstMatch(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern,
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

QJsonArray audioLines(const QString& description)
{
    QJsonArray out;
    const QStringList lines
        = description.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.contains(QRegularExpression(
                QStringLiteral(R"(^\s*(?:Audio|Аудио|Звук|Sound)\s*#?\d*\s*:)"),
                QRegularExpression::CaseInsensitiveOption))) {
            out.append(line);
        }
    }
    return out;
}

} // namespace

int RutorSource::sortCode(const QString& sortKey)
{
    if (sortKey == QStringLiteral("seeders_desc"))
        return 2;
    if (sortKey == QStringLiteral("seeders_asc"))
        return 3;
    if (sortKey == QStringLiteral("size_desc"))
        return 6;
    if (sortKey == QStringLiteral("size_asc"))
        return 7;
    if (sortKey == QStringLiteral("name_desc"))
        return 8;
    if (sortKey == QStringLiteral("name_asc"))
        return 9;
    if (sortKey == QStringLiteral("added_asc"))
        return 1;
    return 0; // date descending
}

QUrl RutorSource::searchUrl(const QString& query, const QString& sortKey)
{
    const QByteArray encoded = QUrl::toPercentEncoding(query.trimmed());
    const QByteArray url = QByteArray("https://rutor.info/search/0/0/100/")
        + QByteArray::number(sortCode(sortKey)) + "/" + encoded + "/";
    return QUrl::fromEncoded(url);
}

QVector<domain::Torrent> RutorSource::parseSearchPage(
    const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates)
{
    QVector<domain::Torrent> out;
    if (rawData.isEmpty() || maxCandidates <= 0)
        return out;

    const QString html = QString::fromUtf8(rawData);
    const QRegularExpression rowRe(
        QStringLiteral(R"(<tr\b[^>]*>(.*?)</tr>)"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression detailRe(
        QStringLiteral(R"re(<a\b[^>]*href\s*=\s*["']([^"']*/torrent/(\d+)[^"']*)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression magnetRe(
        QStringLiteral(R"re(<a\b[^>]*href\s*=\s*["'](magnet:\?[^"']*xt=urn:btih:([A-Fa-f0-9]{40})[^"']*)["'][^>]*>)re"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);

    QSet<QString> seen;
    QRegularExpressionMatchIterator rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QString row = rows.next().captured(1);
        const QRegularExpressionMatch detail = detailRe.match(row);
        const QRegularExpressionMatch magnet = magnetRe.match(row);
        if (!detail.hasMatch() || !magnet.hasMatch())
            continue;

        const QString hash = infohash::normalize(magnet.captured(2));
        if (!infohash::isValid(hash) || seen.contains(hash))
            continue;

        const QUrl sourceUrl = resolveUrl(pageUrl, detail.captured(1));
        if (!sourceUrl.isValid() || !sourceUrl.path().startsWith(QStringLiteral("/torrent/")))
            continue;

        const QString name = htmlToText(detail.captured(3)).trimmed();
        if (name.isEmpty())
            continue;

        domain::Torrent torrent;
        torrent.hash = hash;
        torrent.name = name;
        torrent.size = parseSize(htmlToText(row));
        torrent.seeders = firstInteger(
            row, QStringLiteral(R"(<span[^>]*class\s*=\s*["'][^"']*\bgreen\b[^"']*["'][^>]*>\s*(\d+))"));
        torrent.leechers = firstInteger(
            row, QStringLiteral(R"(<span[^>]*class\s*=\s*["'][^"']*\bred\b[^"']*["'][^>]*>\s*(\d+))"));

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
        info[QStringLiteral("sourceTopicId")] = detail.captured(2).toInt();
        info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
        info[QStringLiteral("sourceVerified")] = false;
        torrent.info = info;

        seen.insert(hash);
        out.append(std::move(torrent));
    }
    return out;
}

bool RutorSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl)
{
    if (!torrent.isValid() || rawData.isEmpty())
        return false;

    const QString html = QString::fromUtf8(rawData);

    // Identity proof: at least one magnet on THIS concrete page must carry the
    // exact hash from the source search row.
    const QRegularExpression magnetRe(
        QStringLiteral(R"(xt=urn:btih:([A-Fa-f0-9]{40}))"),
        QRegularExpression::CaseInsensitiveOption);
    bool exactHash = false;
    auto magnetIt = magnetRe.globalMatch(html);
    while (magnetIt.hasNext()) {
        if (infohash::normalize(magnetIt.next().captured(1)) == torrent.hash) {
            exactHash = true;
            break;
        }
    }
    if (!exactHash)
        return false;

    QString detailBlock;
    const QRegularExpression detailsRe(
        QStringLiteral(R"(<table\b[^>]*id\s*=\s*["']details["'][^>]*>(.*?)</table>)"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch details = detailsRe.match(html);
    if (details.hasMatch())
        detailBlock = details.captured(1);
    else
        detailBlock = html; // exact hash is already proved; tolerate wrapper changes

    const QString description = htmlToText(detailBlock).trimmed();
    if (description.isEmpty())
        return false;

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    info[QStringLiteral("sourceVerified")] = true;

    QUrl sourceUrl = finalUrl;
    if (!sourceUrl.isValid() || !sourceUrl.path().startsWith(QStringLiteral("/torrent/")))
        sourceUrl = QUrl(info.value(QStringLiteral("sourceUrl")).toString());
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
    info[QStringLiteral("description")] = description;

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
            QStringLiteral(R"(\b((?:HEVC|H[ .]?265|x265|AVC|H[ .]?264|x264)[^\n]{0,120})\b)"));
    }
    if (!video.isEmpty())
        info[QStringLiteral("video")] = video;

    const QJsonArray audio = audioLines(description);
    if (!audio.isEmpty())
        info[QStringLiteral("audioTracks")] = audio;

    const QString subtitles = firstMatch(description,
        QStringLiteral(R"((?:Субтитры|Subtitles?)\s*:\s*([^\n]+))"));
    if (!subtitles.isEmpty())
        info[QStringLiteral("subtitles")] = subtitles;

    torrent.info = info;
    torrent.info[QStringLiteral("strictComplete")] = isStrictComplete(torrent);
    return true;
}

bool RutorSource::isStrictComplete(const domain::Torrent& torrent)
{
    const QJsonObject& info = torrent.info;
    if (!torrent.isValid())
        return false;
    if (info.value(QStringLiteral("sourceProvider")).toString() != QStringLiteral("rutor"))
        return false;
    if (!info.value(QStringLiteral("sourceVerified")).toBool())
        return false;

    const QUrl sourceUrl(info.value(QStringLiteral("sourceUrl")).toString());
    if (!sourceUrl.isValid() || !sourceUrl.path().startsWith(QStringLiteral("/torrent/")))
        return false;

    const QString description = info.value(QStringLiteral("description")).toString().trimmed();
    if (description.size() < 160)
        return false;

    return !info.value(QStringLiteral("quality")).toString().isEmpty()
        && !info.value(QStringLiteral("video")).toString().isEmpty()
        && !info.value(QStringLiteral("audioTracks")).toArray().isEmpty();
}

} // namespace rats::net
