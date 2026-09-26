#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>

#include "net/media_metadata_utils.h"

using namespace rats::net::metadata;

class TestMediaMetadata : public QObject {
    Q_OBJECT

private slots:
    void cleanTitle_movieRelease();
    void cleanTitle_seriesRelease();
    void extractTechnical_richRelease();
    void mergeTechnical_arrays();
    void richness_prefersTechnicalDescription();
};

void TestMediaMetadata::cleanTitle_movieRelease()
{
    QCOMPARE(cleanMediaTitle(QStringLiteral("Se7en (1995) [2160p] [4K] [BluRay] [5.1] [YTS.MX]")),
        QStringLiteral("Se7en"));
    QCOMPARE(extractYear(QStringLiteral("Se7en (1995) [2160p]")), 1995);
}

void TestMediaMetadata::cleanTitle_seriesRelease()
{
    QCOMPARE(cleanMediaTitle(QStringLiteral("The.Show.S02E03.1080p.WEB-DL.x265")),
        QStringLiteral("The Show"));
}

void TestMediaMetadata::extractTechnical_richRelease()
{
    const QString text = QString::fromUtf8(
        "Качество: UHD BluRay 2160p HDR10 Dolby Vision\n"
        "Видео: HEVC x265 10-bit\n"
        "Аудио #1: Russian DTS-HD MA 5.1\n"
        "Audio #2: English TrueHD Atmos 7.1\n"
        "Субтитры: Russian, English");

    const QJsonObject tech = extractTechnicalInfo(text);
    QCOMPARE(tech.value(QStringLiteral("resolution")).toString(), QStringLiteral("2160p"));
    QCOMPARE(tech.value(QStringLiteral("videoCodec")).toString(), QStringLiteral("HEVC / H.265"));
    QCOMPARE(tech.value(QStringLiteral("bitDepth")).toString(), QStringLiteral("10-bit"));
    QVERIFY(tech.value(QStringLiteral("audioCodecs")).toArray().size() >= 3);
    QCOMPARE(tech.value(QStringLiteral("audioDetails")).toArray().size(), 2);
    QVERIFY(tech.value(QStringLiteral("languages")).toArray().size() >= 2);
    QCOMPARE(tech.value(QStringLiteral("subtitles")).toArray().size(), 1);
}

void TestMediaMetadata::mergeTechnical_arrays()
{
    QJsonObject a;
    a[QStringLiteral("audioCodecs")] = QJsonArray { QStringLiteral("DTS") };
    QJsonObject b;
    b[QStringLiteral("audioCodecs")] = QJsonArray { QStringLiteral("DTS"), QStringLiteral("AAC") };

    const QJsonObject merged = mergeTechnicalInfo(a, b);
    QCOMPARE(merged.value(QStringLiteral("audioCodecs")).toArray().size(), 2);
}

void TestMediaMetadata::richness_prefersTechnicalDescription()
{
    const QString prose = QStringLiteral(
        "A very long ordinary plot synopsis that says many words but contains no technical release facts. "
        "It keeps going for a while to make the comparison meaningful.");
    const QString technical = QStringLiteral(
        "Видео: HEVC 2160p HDR10\nАудио #1: Russian DTS-HD MA 5.1\nAudio #2: English TrueHD 7.1");

    QVERIFY(descriptionRichness(technical) > descriptionRichness(prose));
}

QTEST_MAIN(TestMediaMetadata)
#include "test_media_metadata.moc"
