#include "net/media_metadata_utils.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

namespace rats::net::metadata {
namespace {

QString normalizedSpaces(QString value)
{
    value.replace(QRegularExpression(QStringLiteral("[._]+")), QStringLiteral(" "));
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return value.trimmed();
}

void appendUnique(QStringList& list, const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return;
    for (const QString& existing : list) {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0)
            return;
    }
    list.append(trimmed);
}

QJsonArray toArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

QStringList fromArray(const QJsonArray& array)
{
    QStringList result;
    for (const QJsonValue& value : array)
        appendUnique(result, value.toString());
    return result;
}

QString firstMatch(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString canonicalVideoCodec(QString value)
{
    const QString lower = value.toLower();
    if (lower.contains(QStringLiteral("265")) || lower.contains(QStringLiteral("hevc")))
        return QStringLiteral("HEVC / H.265");
    if (lower.contains(QStringLiteral("264")) || lower.contains(QStringLiteral("avc")))
        return QStringLiteral("AVC / H.264");
    if (lower.contains(QStringLiteral("av1")))
        return QStringLiteral("AV1");
    if (lower.contains(QStringLiteral("vp9")))
        return QStringLiteral("VP9");
    return value.trimmed();
}

} // namespace

int extractYear(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("\\b(19\\d{2}|20\\d{2})\\b"));
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString cleanMediaTitle(const QString& torrentName)
{
    QString value = torrentName.trimmed();
    if (value.isEmpty())
        return value;

    // Release names commonly use dots/underscores as word separators. Normalize
    // them before looking for SxxEyy/year/quality boundaries; otherwise
    // "The.Show.S02E03.1080p" never exposes the whitespace boundary required by
    // those patterns.
    value = normalizedSpaces(value);

    // Prefer the text before a release year; this handles the overwhelming
    // majority of movie and series naming conventions without guessing at
    // individual release-group tags.
    static const QRegularExpression yearBoundary(
        QStringLiteral(R"(^\s*(.+?)\s*[\(\[]?\b(?:19\d{2}|20\d{2})\b)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = yearBoundary.match(value);
    if (match.hasMatch() && match.captured(1).trimmed().size() >= 2)
        value = match.captured(1);

    // Series names often have no year but do have S01E02 / 1x02.
    static const QRegularExpression episodeBoundary(
        QStringLiteral(R"(^\s*(.+?)\s+(?:S\d{1,2}(?:E\d{1,3})?|\d{1,2}x\d{1,3})\b)"),
        QRegularExpression::CaseInsensitiveOption);
    match = episodeBoundary.match(value);
    if (match.hasMatch() && match.captured(1).trimmed().size() >= 2)
        value = match.captured(1);

    // If there was neither year nor episode marker, stop at a strong release
    // token rather than feeding "BluRay x265 ..." to a title metadata service.
    static const QRegularExpression releaseBoundary(
        QStringLiteral(R"(^\s*(.+?)\s+(?:2160p|1080p|720p|480p|4K|UHD|BluRay|BDRemux|REMUX|WEB[- .]?DL|WEBRip|BDRip|HDRip|HDTV)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    match = releaseBoundary.match(value);
    if (match.hasMatch() && match.captured(1).trimmed().size() >= 2)
        value = match.captured(1);

    value.remove(QRegularExpression(QStringLiteral(R"(\[[^\]]*\]$)")));
    value = normalizedSpaces(value);
    value.remove(QRegularExpression(QStringLiteral(R"(^[-\s]+|[-\s]+$)")));
    return value.trimmed();
}

QJsonObject extractTechnicalInfo(const QString& text)
{
    QJsonObject info;
    if (text.trimmed().isEmpty())
        return info;

    QString resolution = firstMatch(text, QStringLiteral(R"(\b(2160p|1080p|720p|576p|480p|4K)\b)"));
    if (resolution.compare(QStringLiteral("4K"), Qt::CaseInsensitive) == 0)
        resolution = QStringLiteral("2160p / 4K");
    else if (resolution.compare(QStringLiteral("2160p"), Qt::CaseInsensitive) == 0
        && text.contains(QRegularExpression(QStringLiteral("\\b4K\\b"), QRegularExpression::CaseInsensitiveOption)))
        resolution = QStringLiteral("2160p / 4K");
    if (!resolution.isEmpty())
        info[QStringLiteral("resolution")] = resolution;

    const QString source = firstMatch(text,
        QStringLiteral(R"(\b(UHD[ ._-]?BluRay|BluRay|BDRemux|BD[- ]?Remux|REMUX|WEB[- .]?DL|WEBRip|BDRip|HDRip|HDTV|DVDRip)\b)"));
    if (!source.isEmpty())
        info[QStringLiteral("source")] = source;

    const QString codec = firstMatch(
        text, QStringLiteral(R"(\b(HEVC|H[ .]?265|x265|AVC|H[ .]?264|x264|AV1|VP9)\b)"));
    if (!codec.isEmpty())
        info[QStringLiteral("videoCodec")] = canonicalVideoCodec(codec);

    const QString bitDepth = firstMatch(text, QStringLiteral(R"(\b(8|10|12)[ -]?bit\b)"));
    if (!bitDepth.isEmpty())
        info[QStringLiteral("bitDepth")] = bitDepth + QStringLiteral("-bit");

    QStringList hdr;
    if (text.contains(QRegularExpression(QStringLiteral(R"(Dolby\s*Vision|\bDV\b)"),
            QRegularExpression::CaseInsensitiveOption)))
        appendUnique(hdr, QStringLiteral("Dolby Vision"));
    if (text.contains(QRegularExpression(QStringLiteral(R"(HDR10\+)"), QRegularExpression::CaseInsensitiveOption)))
        appendUnique(hdr, QStringLiteral("HDR10+"));
    else if (text.contains(QRegularExpression(QStringLiteral(R"(HDR10)"), QRegularExpression::CaseInsensitiveOption)))
        appendUnique(hdr, QStringLiteral("HDR10"));
    else if (text.contains(QRegularExpression(QStringLiteral(R"(\bHDR\b)"), QRegularExpression::CaseInsensitiveOption)))
        appendUnique(hdr, QStringLiteral("HDR"));
    if (!hdr.isEmpty())
        info[QStringLiteral("hdr")] = toArray(hdr);

    QStringList audioCodecs;
    const QList<QPair<QString, QString>> audioPatterns = {
        { QStringLiteral(R"(DTS[- .]?HD[- .]?MA)"), QStringLiteral("DTS-HD MA") },
        { QStringLiteral(R"(DTS[: -]?X)"), QStringLiteral("DTS:X") },
        { QStringLiteral(R"(TrueHD)"), QStringLiteral("TrueHD") },
        { QStringLiteral(R"(Atmos)"), QStringLiteral("Dolby Atmos") },
        { QStringLiteral(R"(E[- .]?AC[- .]?3|DDP(?:lus)?)"), QStringLiteral("E-AC-3 / DDP") },
        { QStringLiteral(R"(AC[- .]?3|Dolby\s*Digital(?!\s*Plus))"), QStringLiteral("AC-3 / Dolby Digital") },
        { QStringLiteral(R"(\bDTS\b)"), QStringLiteral("DTS") },
        { QStringLiteral(R"(\bAAC(?=\b|[ ._-]?\d))"), QStringLiteral("AAC") },
        { QStringLiteral(R"(\bFLAC\b)"), QStringLiteral("FLAC") },
        { QStringLiteral(R"(\bOpus\b)"), QStringLiteral("Opus") },
        { QStringLiteral(R"(\bMP3\b)"), QStringLiteral("MP3") },
    };
    for (const auto& pair : audioPatterns) {
        if (text.contains(QRegularExpression(pair.first, QRegularExpression::CaseInsensitiveOption)))
            appendUnique(audioCodecs, pair.second);
    }
    if (!audioCodecs.isEmpty())
        info[QStringLiteral("audioCodecs")] = toArray(audioCodecs);

    QStringList channels;
    QRegularExpression channelsRe(QStringLiteral(R"(\b(9\.1|7\.1|5\.1|2\.1|2\.0|1\.0)\b)"));
    QRegularExpressionMatchIterator channelIt = channelsRe.globalMatch(text);
    while (channelIt.hasNext())
        appendUnique(channels, channelIt.next().captured(1));
    if (!channels.isEmpty())
        info[QStringLiteral("audioChannels")] = toArray(channels);

    QStringList languages;
    const QList<QPair<QString, QString>> languagePatterns = {
        { QStringLiteral(R"(\bRussian\b|\bRus\b|\bRUS\b|\bРусск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Russian") },
        { QStringLiteral(R"(\bEnglish\b|\bEng\b|\bENG\b|\bАнглийск(?:ий|ая|ое|ие)\b)"), QStringLiteral("English") },
        { QStringLiteral(R"(\bUkrainian\b|\bУкраинск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Ukrainian") },
        { QStringLiteral(R"(\bJapanese\b|\bЯпонск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Japanese") },
        { QStringLiteral(R"(\bSpanish\b|\bИспанск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Spanish") },
        { QStringLiteral(R"(\bFrench\b|\bФранцузск(?:ий|ая|ое|ие)\b)"), QStringLiteral("French") },
        { QStringLiteral(R"(\bGerman\b|\bНемецк(?:ий|ая|ое|ие)\b)"), QStringLiteral("German") },
        { QStringLiteral(R"(\bItalian\b|\bИтальянск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Italian") },
        { QStringLiteral(R"(\bKorean\b|\bКорейск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Korean") },
        { QStringLiteral(R"(\bChinese\b|\bКитайск(?:ий|ая|ое|ие)\b)"), QStringLiteral("Chinese") },
    };
    for (const auto& pair : languagePatterns) {
        if (text.contains(QRegularExpression(pair.first, QRegularExpression::CaseInsensitiveOption)))
            appendUnique(languages, pair.second);
    }

    // French scene/release naming carries useful audio-language information
    // even when a tracker page has no MediaInfo block.
    const bool hasVf2 = text.contains(QRegularExpression(
        QStringLiteral(R"(\b(?:VF2|FR2)\b)"), QRegularExpression::CaseInsensitiveOption));
    const bool hasVff = text.contains(QRegularExpression(
        QStringLiteral(R"(\b(?:VFF|TRUEFRENCH)\b)"), QRegularExpression::CaseInsensitiveOption));
    const bool hasVfq = text.contains(QRegularExpression(
        QStringLiteral(R"(\bVFQ\b)"), QRegularExpression::CaseInsensitiveOption));
    const bool hasMulti = text.contains(QRegularExpression(
        QStringLiteral(R"(\bMULTI\b)"), QRegularExpression::CaseInsensitiveOption));

    if (hasVf2) {
        appendUnique(languages, QStringLiteral("French (VF2: VFF + VFQ)"));
    } else {
        if (hasVff)
            appendUnique(languages, QStringLiteral("French (VFF)"));
        if (hasVfq)
            appendUnique(languages, QStringLiteral("French (VFQ)"));
    }
    if (hasMulti)
        appendUnique(languages, QStringLiteral("MULTi (multiple audio languages)"));

    QStringList audioDetails;
    QStringList subtitleDetails;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        const bool audioLabel = line.contains(QRegularExpression(
            QStringLiteral(R"(^\s*(?:Audio|Аудио|Звук|Sound)\s*#?\d*\s*[:：-]|^\s*(?:Аудиодорожк\w*|Дорожк\w*|Перевод|Translation|Voice)\s*#?\d*\s*[:：-])"),
            QRegularExpression::CaseInsensitiveOption));
        if (audioLabel)
            appendUnique(audioDetails, line);

        const bool subtitleLabel = line.contains(QRegularExpression(
            QStringLiteral(R"(^\s*(?:Subtitles?|Субтитры)\s*[:：-])"),
            QRegularExpression::CaseInsensitiveOption));
        if (subtitleLabel)
            appendUnique(subtitleDetails, line);

        if (line.contains(QRegularExpression(
                QStringLiteral(R"(^\s*(?:Language|Язык|Audio\s*language|Язык\s*аудио)\s*[:：-])"),
                QRegularExpression::CaseInsensitiveOption))) {
            const int colon = qMax(line.indexOf(QLatin1Char(':')), line.indexOf(QChar(0xFF1A)));
            if (colon >= 0)
                appendUnique(languages, line.mid(colon + 1).trimmed());
        }
    }

    // When no tracker MediaInfo lines are available, preserve what the exact
    // release name itself states instead of showing only "1080p".
    if (audioDetails.isEmpty()) {
        QStringList releaseAudio;
        if (!audioCodecs.isEmpty())
            releaseAudio << audioCodecs.join(QStringLiteral(" + "));
        if (!channels.isEmpty())
            releaseAudio << channels.join(QStringLiteral(" / "));
        if (!releaseAudio.isEmpty())
            appendUnique(audioDetails, QStringLiteral("Audio (release name): ") + releaseAudio.join(QStringLiteral(", ")));
        if (hasVf2)
            appendUnique(audioDetails, QStringLiteral("Audio tag: VF2 = French VFF + VFQ"));
        else {
            if (hasVff)
                appendUnique(audioDetails, QStringLiteral("Audio tag: VFF (French-France dub)"));
            if (hasVfq)
                appendUnique(audioDetails, QStringLiteral("Audio tag: VFQ (French-Quebec dub)"));
        }
        if (hasMulti)
            appendUnique(audioDetails, QStringLiteral("Audio tag: MULTi (multiple audio languages)"));
    }

    if (!audioDetails.isEmpty())
        info[QStringLiteral("audioDetails")] = toArray(audioDetails);
    if (!subtitleDetails.isEmpty())
        info[QStringLiteral("subtitles")] = toArray(subtitleDetails);
    if (!languages.isEmpty())
        info[QStringLiteral("languages")] = toArray(languages);

    return info;
}

QJsonObject mergeTechnicalInfo(const QJsonObject& base, const QJsonObject& incoming)
{
    QJsonObject merged = base;

    for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
        if (it.value().isArray()) {
            QStringList values = fromArray(merged.value(it.key()).toArray());
            for (const QJsonValue& value : it.value().toArray())
                appendUnique(values, value.toString());
            if (!values.isEmpty())
                merged[it.key()] = toArray(values);
            continue;
        }

        const QString newValue = it.value().toString().trimmed();
        const QString oldValue = merged.value(it.key()).toString().trimmed();
        if (oldValue.isEmpty() || newValue.size() > oldValue.size())
            merged[it.key()] = it.value();
    }

    return merged;
}

int descriptionRichness(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return 0;

    int score = qMin(trimmed.size(), 8000);
    const QJsonObject tech = extractTechnicalInfo(trimmed);
    score += tech.size() * 700;
    score += tech.value(QStringLiteral("audioDetails")).toArray().size() * 900;
    score += tech.value(QStringLiteral("subtitles")).toArray().size() * 300;

    // Label-heavy tracker descriptions tend to be much more useful than a long
    // prose synopsis because they carry release-specific video/audio facts.
    const QStringList lines = trimmed.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.contains(QRegularExpression(
                QStringLiteral(R"(^\s*[^:]{2,30}:\s*\S+)"), QRegularExpression::CaseInsensitiveOption)))
            score += 80;
    }
    return score;
}

} // namespace rats::net::metadata
