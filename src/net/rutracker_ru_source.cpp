#include "net/rutracker_ru_source.h"

#include "common/infohash.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>

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

QUrl RuTrackerRuSource::searchUrl(const QString& query, const QString& sortKey)
{
    QUrl url(QStringLiteral("http://rutracker.ru/tracker.php"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("f[]"), QStringLiteral("-1"));
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
    if (!video.isEmpty())
        info[QStringLiteral("video")] = video;

    const QJsonArray audio = audioLines(description);
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

    return !info.value(QStringLiteral("quality")).toString().isEmpty()
        && !info.value(QStringLiteral("video")).toString().isEmpty()
        && !info.value(QStringLiteral("audioTracks")).toArray().isEmpty();
}

} // namespace rats::net
