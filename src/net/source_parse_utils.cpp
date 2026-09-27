#include "net/source_parse_utils.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace rats::net::sourceparse {
namespace {

QChar cp1251Char(unsigned char b)
{
    if (b < 0x80)
        return QChar(b);
    if (b >= 0xC0)
        return QChar(0x0410 + (b - 0xC0));

    static const ushort table[64] = {
        0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
        0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
        0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
        0x0098,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
        0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,
        0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
        0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,
        0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457
    };
    return QChar(table[b - 0x80]);
}

unsigned char toCp1251(QChar ch)
{
    const ushort u = ch.unicode();
    if (u < 0x80)
        return static_cast<unsigned char>(u);
    if (u >= 0x0410 && u <= 0x042F)
        return static_cast<unsigned char>(0xC0 + (u - 0x0410));
    if (u >= 0x0430 && u <= 0x044F)
        return static_cast<unsigned char>(0xE0 + (u - 0x0430));

    switch (u) {
    case 0x0401: return 0xA8;
    case 0x0451: return 0xB8;
    case 0x0404: return 0xAA;
    case 0x0454: return 0xBA;
    case 0x0406: return 0xB2;
    case 0x0456: return 0xB3;
    case 0x0407: return 0xAF;
    case 0x0457: return 0xBF;
    case 0x0490: return 0xA5;
    case 0x0491: return 0xB4;
    case 0x2116: return 0xB9;
    case 0x00AB: return 0xAB;
    case 0x00BB: return 0xBB;
    case 0x2013: return 0x96;
    case 0x2014: return 0x97;
    default: return static_cast<unsigned char>('?');
    }
}

QByteArray encodeWindows1251(const QString& text)
{
    QByteArray out;
    out.reserve(text.size());
    for (QChar ch : text)
        out.append(static_cast<char>(toCp1251(ch)));
    return out;
}

QByteArray percentEncodeBytes(const QByteArray& bytes, bool form)
{
    static const char hex[] = "0123456789ABCDEF";
    QByteArray out;
    out.reserve(bytes.size() * 3);
    for (unsigned char b : bytes) {
        const bool safe = (b >= 'a' && b <= 'z')
            || (b >= 'A' && b <= 'Z')
            || (b >= '0' && b <= '9')
            || b == '-' || b == '_' || b == '.' || b == '~';
        if (safe) {
            out.append(static_cast<char>(b));
        } else if (form && b == ' ') {
            out.append('+');
        } else {
            out.append('%');
            out.append(hex[(b >> 4) & 0x0f]);
            out.append(hex[b & 0x0f]);
        }
    }
    return out;
}

QString firstLabelValue(const QString& description, const QStringList& labels)
{
    QStringList escaped;
    escaped.reserve(labels.size());
    for (const QString& label : labels)
        escaped.append(QRegularExpression::escape(label));

    const QRegularExpression re(
        QStringLiteral("(?im)^\\s*(?:%1)\\s*:\\s*([^\\n]+)")
            .arg(escaped.join(QLatin1Char('|'))),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(description);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

QString qualityFromName(const QString& name)
{
    const QRegularExpression re(
        QStringLiteral(
            R"(\b(UHD\s*BDRemux|BDRemux|Blu[- ]?Ray|BDRip|WEB[- ]?DL(?:Rip)?|WEBRip|HDTVRip|DVDRip|HDRip|2160p|1080p|720p)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(name);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

} // namespace

QString decodeTrackerText(const QByteArray& bytes)
{
    QString utf8 = QString::fromUtf8(bytes);
    if (!utf8.contains(QChar::ReplacementCharacter))
        return utf8;

    QString out;
    out.reserve(bytes.size());
    for (unsigned char b : bytes)
        out.append(cp1251Char(b));
    return out;
}

QByteArray percentEncodeWindows1251(const QString& text)
{
    return percentEncodeBytes(encodeWindows1251(text), false);
}

QByteArray formEncodeWindows1251(const QString& text)
{
    return percentEncodeBytes(encodeWindows1251(text), true);
}

QString decodeEntities(QString text)
{
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("""));
    text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
    text.replace(QChar(0x00A0), QLatin1Char(' '));
    return text;
}

QString stripHtml(QString html)
{
    html.remove(QRegularExpression(
        QStringLiteral("<(?:script|style)\\b[^>]*>.*?</(?:script|style)>"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption));
    html.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    html = decodeEntities(html);
    html.replace(QRegularExpression(QStringLiteral("[\\t\\r\\n ]+")),
        QStringLiteral(" "));
    return html.trimmed();
}

QString htmlToText(QString html)
{
    html.remove(QRegularExpression(
        QStringLiteral("<(?:script|style)\\b[^>]*>.*?</(?:script|style)>"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption));
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
                     QStringLiteral("</(?:p|div|li|pre|h[1-6]|section)>"),
                     QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\n"));
    html.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    html = decodeEntities(html);

    QStringList clean;
    const QStringList lines = html.split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line.replace(QRegularExpression(QStringLiteral("[\\t ]+")),
            QStringLiteral(" "));
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
    text.replace(QStringLiteral("ТБ"), QStringLiteral("TB"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("ГБ"), QStringLiteral("GB"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("МБ"), QStringLiteral("MB"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("КБ"), QStringLiteral("KB"), Qt::CaseInsensitive);

    const QRegularExpression re(
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

domain::ContentType contentTypeFromCategoryText(QString category)
{
    category = category.trimmed().toLower();

    if (category.contains(QStringLiteral("музык"))
        || category.contains(QStringLiteral("аудио"))
        || category.contains(QStringLiteral("audio"))
        || category.contains(QStringLiteral("music"))) {
        return domain::ContentType::Audio;
    }
    if (category.contains(QStringLiteral("игр"))
        || category.contains(QStringLiteral("game"))) {
        return domain::ContentType::Games;
    }
    if (category.contains(QStringLiteral("софт"))
        || category.contains(QStringLiteral("программ"))
        || category.contains(QStringLiteral("windows"))
        || category.contains(QStringLiteral("linux"))
        || category.contains(QStringLiteral("mac os"))
        || category.contains(QStringLiteral("software"))) {
        return domain::ContentType::Software;
    }
    if (category.contains(QStringLiteral("книг"))
        || category.contains(QStringLiteral("литератур"))
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

void populateTechnicalInfo(domain::Torrent& torrent)
{
    QJsonObject info = torrent.info;
    const QString description
        = info.value(QStringLiteral("description")).toString();

    QString quality = firstLabelValue(description,
        { QStringLiteral("Качество"), QStringLiteral("Quality"),
          QStringLiteral("Качество видео"), QStringLiteral("Video quality") });
    if (quality.isEmpty() && torrent.contentType == domain::ContentType::Video)
        quality = qualityFromName(torrent.name);
    if (!quality.isEmpty())
        info[QStringLiteral("quality")] = quality;

    const QString video = firstLabelValue(description,
        { QStringLiteral("Видео"), QStringLiteral("Video"),
          QStringLiteral("Видео кодек"), QStringLiteral("Video codec") });
    if (!video.isEmpty())
        info[QStringLiteral("video")] = video;

    QJsonArray audio;
    QSet<QString> seen;
    const QStringList lines = description.split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    const QRegularExpression explicitAudio(
        QStringLiteral(
            "^\\s*(?:Audio|Аудио|Звук|Sound)(?:\\s*#?\\d+)?\\s*:\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression audioReleaseTech(
        QStringLiteral(
            "^\\s*(?:Формат(?:\\s*/\\s*Кодек)?|Format(?:\\s*/\\s*Codec)?|"
            "Формат\\s+аудио|Audio\\s+format|Аудиокодек|Аудио\\s+кодек|"
            "Audio\\s+codec|Кодек|Codec|Битрейт(?:\\s+аудио)?|Audio\\s+bitrate|"
            "Качество\\s+аудио|Audio\\s+quality|Тип\\s+рипа|Rip\\s+type)\\s*:\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption);

    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        QRegularExpressionMatch m = explicitAudio.match(line);
        if (!m.hasMatch() && torrent.contentType == domain::ContentType::Audio)
            m = audioReleaseTech.match(line);
        if (!m.hasMatch())
            continue;

        const QString value = line;
        const QString key = value.toLower();
        if (!seen.contains(key)) {
            seen.insert(key);
            audio.append(value);
        }
    }
    if (!audio.isEmpty())
        info[QStringLiteral("audioTracks")] = audio;

    const QString subtitles = firstLabelValue(description,
        { QStringLiteral("Субтитры"), QStringLiteral("Subtitles"),
          QStringLiteral("Subtitle") });
    if (!subtitles.isEmpty())
        info[QStringLiteral("subtitles")] = subtitles;

    torrent.info = info;
}

} // namespace rats::net::sourceparse
