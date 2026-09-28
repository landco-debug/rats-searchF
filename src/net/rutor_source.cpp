#include "net/rutor_source.h"

#include "common/infohash.h"
#include "net/source_parse_utils.h"

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

int spanCounter(const QString& row, const QString& className)
{
    // Rutor currently exposes swarm counters in span.green / span.red. Do not
    // assume that the number is the first raw character after '>'; the site may
    // wrap it in <b>, <a>, etc., and older mirrors also used unquoted class=.
    const QString escapedClass = QRegularExpression::escape(className);
    const QString pattern = QStringLiteral(
        R"(<span\b[^>]*class\s*=\s*(?:"[^"]*\b%1\b[^"]*"|'[^']*\b%1\b[^']*'|[^\s>]*\b%1\b[^\s>]*)[^>]*>(.*?)</span>[^<]*)")
                                .arg(escapedClass);
    const QRegularExpression re(pattern,
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = re.match(row);
    if (!match.hasMatch())
        return 0;

    const QString text = htmlToText(match.captured(1));
    const QRegularExpression number(QStringLiteral(R"((\d+))"));
    const QRegularExpressionMatch numberMatch = number.match(text);
    return numberMatch.hasMatch() ? numberMatch.captured(1).toInt() : 0;
}

QString firstMatch(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern,
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

QJsonArray audioLines(const QString& description, bool audioRelease)
{
    QJsonArray out;
    const QStringList lines
        = description.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);

    // Explicit audio-stream labels are useful for video too. Generic
    // Format/Codec/Bitrate labels are only considered for a release that the
    // source itself (or exact metainfo) has already identified as Audio; this
    // avoids turning a video's "Формат: MKV" line into a fake audio track.
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

domain::ContentType contentTypeForRutorCategory(QString category)
{
    category = category.trimmed().toLower();
    while (category.endsWith(QLatin1Char(':')))
        category.chop(1);
    category = category.trimmed();

    if (category.contains(QStringLiteral("музык"))
        || category.contains(QStringLiteral("аудио"))
        || category.contains(QStringLiteral("music"))
        || category.contains(QStringLiteral("audio"))) {
        return domain::ContentType::Audio;
    }
    if (category.contains(QStringLiteral("игр"))
        || category.contains(QStringLiteral("game"))) {
        return domain::ContentType::Games;
    }
    if (category.contains(QStringLiteral("софт"))
        || category.contains(QStringLiteral("программ"))
        || category.contains(QStringLiteral("software"))) {
        return domain::ContentType::Software;
    }
    if (category.contains(QStringLiteral("книг"))
        || category.contains(QStringLiteral("журнал"))
        || category.contains(QStringLiteral("ebook"))
        || category.contains(QStringLiteral("book"))
        || category.contains(QStringLiteral("комикс"))) {
        return domain::ContentType::Books;
    }
    if (category.contains(QStringLiteral("фото"))
        || category.contains(QStringLiteral("картин"))
        || category.contains(QStringLiteral("обои"))
        || category.contains(QStringLiteral("picture"))
        || category.contains(QStringLiteral("image"))) {
        return domain::ContentType::Pictures;
    }
    if (category.contains(QStringLiteral("фильм"))
        || category.contains(QStringLiteral("сериал"))
        || category.contains(QStringLiteral("мульт"))
        || category.contains(QStringLiteral("аниме"))
        || category.contains(QStringLiteral("видео"))
        || category.contains(QStringLiteral("movie"))
        || category.contains(QStringLiteral("tv"))
        || category.contains(QStringLiteral("video"))) {
        return domain::ContentType::Video;
    }
    return domain::ContentType::Unknown;
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

QUrl RutorSource::searchUrl(
    const QString& query, const QString& sortKey, int page, int category)
{
    const QByteArray encoded = QUrl::toPercentEncoding(query.trimmed());
    page = qMax(0, page);
    category = qBound(0, category, 3);
    const QByteArray url = QByteArray("https://rutor.info/search/")
        + QByteArray::number(page) + "/" + QByteArray::number(category)
        + "/100/" + QByteArray::number(sortCode(sortKey)) + "/" + encoded + "/";
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
    const QRegularExpression downloadRe(
        QStringLiteral(R"re(<a\b(?=[^>]*\bclass\s*=\s*(?:"[^"]*\bdowngif\b[^"]*"|'[^']*\bdowngif\b[^']*'))[^>]*\bhref\s*=\s*["']([^"']+)["'][^>]*>)re"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);

    QSet<QString> seen;
    QRegularExpressionMatchIterator rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QString row = rows.next().captured(1);
        const QRegularExpressionMatch detail = detailRe.match(row);
        const QRegularExpressionMatch magnet = magnetRe.match(row);
        const QRegularExpressionMatch download = downloadRe.match(row);
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
        torrent.seeders = spanCounter(row, QStringLiteral("green"));
        torrent.leechers = spanCounter(row, QStringLiteral("red"));

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
        info[QStringLiteral("sourceTopicId")] = detail.captured(2).toInt();
        info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
        info[QStringLiteral("sourceMagnet")] = decodeEntities(magnet.captured(1));
        if (download.hasMatch()) {
            const QUrl torrentUrl = resolveUrl(pageUrl, download.captured(1));
            if (torrentUrl.isValid())
                info[QStringLiteral("sourceTorrentUrl")] = torrentUrl.toString();
        }
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
    const QString detailMagnet = sourceparse::magnetForHash(html, torrent.hash);
    if (!detailMagnet.isEmpty())
        info[QStringLiteral("sourceMagnet")] = detailMagnet;

    QUrl sourceUrl = finalUrl;
    if (!sourceUrl.isValid() || !sourceUrl.path().startsWith(QStringLiteral("/torrent/")))
        sourceUrl = QUrl(info.value(QStringLiteral("sourceUrl")).toString());
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
    info[QStringLiteral("description")] = description;

    // Rutor exposes a tracker-native category on the exact release page
    // ("Категория: Музыка", "Игры", "Софт", "Зарубежные фильмы", ...).
    // Treat it as the primary content-type signal, just as mature torrent
    // indexers map native tracker categories before falling back to filenames.
    const QString fullText = htmlToText(html);
    QString sourceCategory = firstMatch(fullText,
        QStringLiteral(R"((?:Категория|Category)\s*:\s*([^\n]+))"));
    if (!sourceCategory.isEmpty()) {
        sourceCategory = sourceCategory.section(QLatin1Char(':'), 0, 0).trimmed();
        while (sourceCategory.endsWith(QLatin1Char(':')))
            sourceCategory.chop(1);
        sourceCategory = sourceCategory.trimmed();
        if (!sourceCategory.isEmpty()) {
            info[QStringLiteral("sourceCategory")] = sourceCategory;
            const domain::ContentType sourceType
                = contentTypeForRutorCategory(sourceCategory);
            if (sourceType != domain::ContentType::Unknown) {
                torrent.contentType = sourceType;
                info[QStringLiteral("contentTypeEvidence")]
                    = QStringLiteral("source-category");
            }
        }
    }

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
    if (!video.isEmpty()) {
        info[QStringLiteral("video")] = video;
        if (torrent.contentType == domain::ContentType::Unknown)
            torrent.contentType = domain::ContentType::Video;
    }

    const QJsonArray audio = audioLines(
        description, torrent.contentType == domain::ContentType::Audio);
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

    const bool hasQuality = !info.value(QStringLiteral("quality")).toString().isEmpty();
    const bool hasVideo = !info.value(QStringLiteral("video")).toString().isEmpty();
    const bool hasAudio = !info.value(QStringLiteral("audioTracks")).toArray().isEmpty();

    // Source-native category or exact .torrent file classification is stronger
    // type evidence than the presence/wording of optional technical labels.
    // Do not hide a valid MP3/FLAC/game/book/software release merely because its
    // description uses a label our presentation parser does not know yet.
    if (torrent.contentType != domain::ContentType::Unknown
        && !info.value(QStringLiteral("contentTypeEvidence")).toString().isEmpty()) {
        return true;
    }

    if (torrent.contentType == domain::ContentType::Video || hasVideo || hasQuality)
        return hasQuality && hasVideo && hasAudio;
    if (torrent.contentType == domain::ContentType::Audio)
        return hasAudio;
    return true;
}

} // namespace rats::net
