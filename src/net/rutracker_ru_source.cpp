#include "net/rutracker_ru_source.h"

#include "common/infohash.h"
#include "net/source_parse_utils.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>

namespace rats::net {
namespace {

QString rutrackerText(QString html)
{
    html.replace(QRegularExpression(
                     QStringLiteral(R"(<span\b[^>]*class\s*=\s*["'][^"']*\bpost-br\b[^"']*["'][^>]*>)"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    return sourceparse::htmlToText(html);
}

QString firstMatch(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern,
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString elementTextById(const QString& html, const QString& id)
{
    const QRegularExpression openRe(
        QStringLiteral(
            R"(<([A-Za-z][A-Za-z0-9]*)\b[^>]*\bid\s*=\s*["']%1["'][^>]*>)")
            .arg(QRegularExpression::escape(id)),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch open = openRe.match(html);
    if (!open.hasMatch())
        return QString();

    const QString tag = open.captured(1);
    const QRegularExpression closeRe(
        QStringLiteral(R"(</%1\s*>)")
            .arg(QRegularExpression::escape(tag)),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch close = closeRe.match(html, open.capturedEnd());
    if (!close.hasMatch())
        return QString();

    return sourceparse::stripHtml(
        html.mid(open.capturedEnd(), close.capturedStart() - open.capturedEnd()))
        .trimmed();
}


int firstInteger(const QString& text)
{
    const QRegularExpression number(QStringLiteral(R"((\d+))"));
    const QRegularExpressionMatch m = number.match(sourceparse::stripHtml(text));
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

QString tableCellAt(const QString& row, int oneBasedIndex)
{
    if (oneBasedIndex <= 0)
        return QString();

    const QRegularExpression tdRe(
        QStringLiteral(R"(<td\b[^>]*>.*?</td>)"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    auto cells = tdRe.globalMatch(row);
    int index = 1;
    while (cells.hasNext()) {
        const QString cell = cells.next().captured(0);
        if (index++ == oneBasedIndex)
            return cell;
    }
    return QString();
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

QString openingTagForClass(const QString& row, const QString& className)
{
    const QString klass = QRegularExpression::escape(className);
    const QRegularExpression re(
        QStringLiteral(R"(<td\b(?=[^>]*class\s*=\s*(?:"[^"]*\b%1\b[^"]*"|'[^']*\b%1\b[^']*'))[^>]*>)")
            .arg(klass),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(row);
    return m.hasMatch() ? m.captured(0) : QString();
}

qint64 dataTsValue(const QString& tag)
{
    const QString value = firstMatch(tag,
        QStringLiteral(R"(data-ts_text\s*=\s*["'](-?\d+)["'])"));
    bool ok = false;
    const qint64 parsed = value.toLongLong(&ok);
    return ok ? parsed : 0;
}

int counterFromClass(const QString& row, const QString& className)
{
    const QString klass = QRegularExpression::escape(className);
    const QRegularExpression re(
        QStringLiteral(R"(<td\b(?=[^>]*class\s*=\s*(?:"[^"]*\b%1\b[^"]*"|'[^']*\b%1\b[^']*'))([^>]*)>(.*?)</td>)")
            .arg(klass),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(row);
    if (!m.hasMatch())
        return 0;

    const qint64 ts = dataTsValue(m.captured(1));
    if (ts > 0)
        return static_cast<int>(ts);
    return firstInteger(m.captured(2));
}

QDateTime publishDateFromRow(const QString& row)
{
    const QString cell = tableCellAt(row, 10);
    const qint64 seconds = dataTsValue(cell);
    return seconds > 0
        ? QDateTime::fromSecsSinceEpoch(seconds, Qt::UTC)
        : QDateTime();
}

bool isRuTrackerHost(QString host)
{
    host = host.toLower();
    if (host.startsWith(QStringLiteral("www.")))
        host.remove(0, 4);
    return host == QStringLiteral("rutracker.org")
        || host == QStringLiteral("rutracker.net")
        || host == QStringLiteral("rutracker.nl");
}

int topicIdFromUrl(const QUrl& url)
{
    if (!url.isValid() || !isRuTrackerHost(url.host())
        || !url.path().endsWith(QStringLiteral("/forum/viewtopic.php"))) {
        return 0;
    }
    QUrlQuery query(url);
    bool ok = false;
    const int id = query.queryItemValue(QStringLiteral("t")).toInt(&ok);
    return ok ? id : 0;
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
    int end = html.indexOf(QStringLiteral("<!--/post_body-->"), start, Qt::CaseInsensitive);
    if (end < 0) {
        end = html.indexOf(QRegularExpression(
            QStringLiteral(R"(<div\b[^>]*class\s*=\s*["'][^"']*\bpost-footer\b)"),
            QRegularExpression::CaseInsensitiveOption), start);
    }
    if (end < 0)
        end = qMin(html.size(), start + 180000);
    return html.mid(start, end - start);
}

QJsonArray audioLines(const QString& description, bool audioRelease)
{
    QJsonArray out;
    const QStringList lines = description.split(
        QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
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


} // namespace

int RuTrackerRuSource::sortColumn(const QString& sortKey)
{
    if (sortKey.startsWith(QStringLiteral("seeders_")))
        return 10;
    if (sortKey.startsWith(QStringLiteral("size_")))
        return 7;
    if (sortKey.startsWith(QStringLiteral("name_")))
        return 2;
    return 1;
}

int RuTrackerRuSource::sortDirection(const QString& sortKey)
{
    return sortKey.endsWith(QStringLiteral("_asc")) ? 1 : 2;
}

QUrl RuTrackerRuSource::searchUrl(
    const QString& query, const QString& sortKey, const QString& contentType)
{
    Q_UNUSED(sortKey);
    Q_UNUSED(contentType);

    QUrl url(QStringLiteral("https://rutracker.org/forum/tracker.php"));
    QUrlQuery q;

    // Keep the request aligned with current maintained RuTracker clients:
    // tracker.php?nm=<query>. The previous fork sent a large set of legacy
    // tracker-form flags (f[]=-1, prev_*, df/da/ds, tm/sns/srg, o/s). On the
    // user's live authenticated page those flags produced a valid tor-tbl with
    // exactly one placeholder row and zero a.tLink entries even for common
    // queries such as "терминатор". That is a server-side empty result, before
    // our parser ever sees a release.
    //
    // Do not constrain the server here. Exact source category is captured from
    // each returned row, typed filtering stays client-side, and the visible
    // result model applies the user's requested sort after results arrive.
    q.addQueryItem(QStringLiteral("nm"), query.trimmed());
    url.setQuery(q);
    return url;
}

QVector<domain::Torrent> RuTrackerRuSource::parseSearchPage(
    const QByteArray& rawData, const QUrl& pageUrl, int maxCandidates)
{
    QVector<domain::Torrent> out;
    if (rawData.isEmpty() || !pageUrl.isValid() || maxCandidates <= 0)
        return out;

    const QString html = sourceparse::decodeTrackerText(rawData);
    const QRegularExpression rowRe(
        QStringLiteral(R"(<tr\b[^>]*id\s*=\s*["']trs-tr-(\d+)["'][^>]*>(.*?)</tr>)"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression detailRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*viewtopic\.php\?t=(\d+)[^"']*)["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression topicDataRe(
        QStringLiteral(R"(data-topic_id\s*=\s*["'](\d+)["'])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression downloadRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["']([^"']*dl\.php\?t=(\d+)[^"']*)["'][^>]*>)re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression forumRe(
        QStringLiteral(
            R"re(<a\b[^>]*href\s*=\s*["'][^"']*tracker\.php\?f=(\d+)[^"']*["'][^>]*>(.*?)</a>)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);

    QSet<int> seen;
    auto rows = rowRe.globalMatch(html);
    while (rows.hasNext() && out.size() < maxCandidates) {
        const QRegularExpressionMatch rowMatch = rows.next();
        const QString row = rowMatch.captured(2);

        const QRegularExpressionMatch detail = detailRe.match(row);
        if (!detail.hasMatch())
            continue;
        const int linkTopicId = detail.captured(2).toInt();
        // trs-tr-N is a presentation identifier, not a torrent identity:
        // some pages number their rows independently of the topic IDs.
        // Identity comes from the concrete topic URL and data-topic_id.
        const int rowTopicId = linkTopicId;
        if (rowTopicId <= 0 || seen.contains(rowTopicId))
            continue;

        const QRegularExpressionMatch dataTopic = topicDataRe.match(detail.captured(0));
        if (dataTopic.hasMatch() && dataTopic.captured(1).toInt() != rowTopicId)
            continue;
        const QRegularExpressionMatch rowDataTopic = topicDataRe.match(
            rowMatch.captured(0).section(QLatin1Char('>'), 0, 0));
        if (rowDataTopic.hasMatch() && rowDataTopic.captured(1).toInt() != rowTopicId)
            continue;

        const QUrl sourceUrl = sourceparse::resolveUrl(pageUrl, detail.captured(1));
        if (topicIdFromUrl(sourceUrl) != rowTopicId)
            continue;

        const QString name = sourceparse::stripHtml(detail.captured(3)).trimmed();
        if (name.isEmpty())
            continue;

        domain::Torrent torrent;
        torrent.name = name;
        torrent.seeders = counterFromClass(row, QStringLiteral("seedmed"));
        if (torrent.seeders <= 0)
            torrent.seeders = firstInteger(tableCellAt(row, 7));
        torrent.leechers = counterFromClass(row, QStringLiteral("leechmed"));
        if (torrent.leechers <= 0)
            torrent.leechers = firstInteger(tableCellAt(row, 8));
        torrent.added = publishDateFromRow(row);

        const QString sizeTag = openingTagForClass(row, QStringLiteral("tor-size"));
        const qint64 size = dataTsValue(sizeTag);
        if (size > 0)
            torrent.size = size;

        QJsonObject info;
        info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
        info[QStringLiteral("sourceTopicId")] = rowTopicId;
        info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
        info[QStringLiteral("sourceVerified")] = false;

        const QRegularExpressionMatch download = downloadRe.match(row);
        if (download.hasMatch() && download.captured(2).toInt() == rowTopicId) {
            const QUrl torrentUrl = sourceparse::resolveUrl(pageUrl, download.captured(1));
            if (torrentUrl.isValid())
                info[QStringLiteral("sourceTorrentUrl")] = torrentUrl.toString();
        }

        const QRegularExpressionMatch forum = forumRe.match(row);
        if (forum.hasMatch()) {
            const int forumId = forum.captured(1).toInt();
            const QString categoryText
                = sourceparse::stripHtml(forum.captured(2)).trimmed();
            info[QStringLiteral("sourceForumId")] = forumId;
            if (!categoryText.isEmpty())
                info[QStringLiteral("sourceCategory")] = categoryText;
            torrent.contentType
                = sourceparse::contentTypeFromCategoryText(categoryText);
            if (torrent.contentType != domain::ContentType::Unknown) {
                info[QStringLiteral("contentTypeEvidence")]
                    = QStringLiteral("source-category");
            }
        }

        torrent.info = info;
        seen.insert(rowTopicId);
        out.append(std::move(torrent));
    }

    return out;
}

bool RuTrackerRuSource::applyDetailPage(
    domain::Torrent& torrent, const QByteArray& rawData, const QUrl& finalUrl)
{
    if (torrent.name.trimmed().isEmpty() || rawData.isEmpty())
        return false;

    const int expectedTopic
        = torrent.info.value(QStringLiteral("sourceTopicId")).toInt();
    if (expectedTopic <= 0)
        return false;

    const int finalTopic = topicIdFromUrl(finalUrl);
    if (finalTopic > 0 && finalTopic != expectedTopic)
        return false;

    QUrl sourceUrl = finalTopic == expectedTopic
        ? finalUrl
        : QUrl(torrent.info.value(QStringLiteral("sourceUrl")).toString());
    if (topicIdFromUrl(sourceUrl) != expectedTopic)
        return false;

    const QString html = sourceparse::decodeTrackerText(rawData);
    const QRegularExpression magnetRe(
        QStringLiteral(R"(xt=urn:btih:([A-Fa-f0-9]{40}))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch magnet = magnetRe.match(html);
    if (!magnet.hasMatch())
        return false;

    const QString hash = infohash::normalize(magnet.captured(1));
    if (!infohash::isValid(hash))
        return false;
    torrent.hash = hash;

    // topic-title can contain nested markup (for example highlighted or
    // styled words). The old generic "</[^>]+>" terminator stopped at the
    // first nested closing tag and reduced names such as "Багровый прилив …"
    // to just "Багровый". Extract the full element by matching its own closing
    // tag, and never replace an already complete search-row title with a
    // suspiciously shorter detail-page value.
    const QString detailTitle = elementTextById(
        html, QStringLiteral("topic-title"));
    if (!detailTitle.isEmpty()
        && (torrent.name.isEmpty() || detailTitle.size() >= torrent.name.size())) {
        torrent.name = detailTitle;
    }

    QString postHtml = firstPostBody(html);
    if (postHtml.isEmpty())
        postHtml = html;
    const QString description = rutrackerText(postHtml).trimmed();
    if (description.isEmpty())
        return false;

    QJsonObject info = torrent.info;
    info[QStringLiteral("sourceProvider")] = QStringLiteral("rutracker-ru");
    info[QStringLiteral("sourceVerified")] = true;
    info[QStringLiteral("sourceUrl")] = sourceUrl.toString();
    info[QStringLiteral("description")] = description;

    if (info.value(QStringLiteral("sourceTorrentUrl")).toString().isEmpty()) {
        QUrl downloadUrl(sourceUrl);
        downloadUrl.setPath(QStringLiteral("/forum/dl.php"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("t"), QString::number(expectedTopic));
        downloadUrl.setQuery(q);
        info[QStringLiteral("sourceTorrentUrl")] = downloadUrl.toString();
    }

    QString quality = firstMatch(description,
        QStringLiteral(R"((?:Качество|Quality)\s*:\s*([^\n]+))"));
    if (quality.isEmpty()) {
        quality = firstMatch(torrent.name,
            QStringLiteral(
                R"(\b(2160p|1080p|720p|576p|480p|4K|UHD|BluRay|BDRip|BDRemux|REMUX|WEB[- .]?DL|WEBRip|HDLight)\b)"));
    }
    if (!quality.isEmpty())
        info[QStringLiteral("quality")] = quality;

    QString video = firstMatch(description,
        QStringLiteral(R"((?:Видео|Video)\s*:\s*([^\n]+))"));
    if (video.isEmpty()) {
        video = firstMatch(description,
            QStringLiteral(
                R"(\b((?:HEVC|H[ .]?265|x265|AVC|H[ .]?264|x264)[^\n]{0,160})\b)"));
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

    const QString poster = firstMatch(postHtml,
        QStringLiteral(
            R"re(<img\b[^>]*class\s*=\s*["'][^"']*\bpostImg(?:Aligned)?\b[^"']*["'][^>]*(?:title|src)\s*=\s*["']([^"']+)["'])re"));
    if (!poster.isEmpty())
        info[QStringLiteral("poster")] = sourceparse::decodeEntities(poster);

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

    const int expectedTopic
        = info.value(QStringLiteral("sourceTopicId")).toInt();
    if (expectedTopic <= 0
        || topicIdFromUrl(QUrl(info.value(QStringLiteral("sourceUrl")).toString()))
            != expectedTopic) {
        return false;
    }

    // Exact-source admission is about provenance and identity, not whether our
    // heuristic field extractor happened to recognize every codec/track label.
    // A concrete topic page + exact magnet hash + release-specific description
    // is enough to prove this is the selected RuTracker release. Parsed quality,
    // video/audio and subtitle fields remain useful enrichment, but are no
    // longer allowed to hide otherwise valid exact releases from Search Results.
    const QString description
        = info.value(QStringLiteral("description")).toString().trimmed();
    return description.size() >= 60;
}

} // namespace rats::net
