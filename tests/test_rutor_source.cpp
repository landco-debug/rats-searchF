#include <QJsonArray>
#include <QtTest/QtTest>

#include "net/rutor_source.h"

using rats::domain::Torrent;
using rats::net::RutorSource;

class TestRutorSource : public QObject {
    Q_OBJECT

private slots:
    void buildsRutorSearchUrl();
    void buildsPagedCategorySearchUrl();
    void searchRowCarriesExactProvenance();
    void detailPageVerifiesSameHashAndRichReleaseInfo();
    void audioDetailPageIsStrictWithoutVideo();
    void sourceCategoryClassifiesMajorTypes();
    void mp3CategoryDoesNotDependOnExactMetadataLabels();
    void mismatchedDetailHashIsRejected();
    void incompleteDetailPageIsNotStrict();
};

static const QString kHash
    = QStringLiteral("0123456789abcdef0123456789abcdef01234567");

void TestRutorSource::buildsRutorSearchUrl()
{
    const QUrl url = RutorSource::searchUrl(
        QStringLiteral("Police Academy 60 fps"), QStringLiteral("seeders_desc"));
    QCOMPARE(url.host(), QStringLiteral("rutor.info"));
    QCOMPARE(url.path(), QStringLiteral("/search/0/0/100/2/Police Academy 60 fps/"));
}

void TestRutorSource::buildsPagedCategorySearchUrl()
{
    const QUrl url = RutorSource::searchUrl(
        QStringLiteral("Sade"), QStringLiteral("seeders_desc"), 2, 3);
    QCOMPARE(url.path(), QStringLiteral("/search/2/3/100/2/Sade/"));
}

void TestRutorSource::searchRowCarriesExactProvenance()
{
    const QByteArray html = R"(
      <table><tr>
        <td>26 Сен 26</td>
        <td>
          <a class="downgif" href="/download/471557"></a>
          <a href="magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567&amp;tr=udp://tracker.rutor.info:2710/announce">magnet</a>
          <a href="/torrent/471557/police-academy">Police Academy BDRip-AVC 60 fps</a>
        </td>
        <td>3.63 GB</td>
        <td><span class="green"><b>42</b></span></td>
        <td><span class=red><a href="/peers">3</a></span></td>
      </tr></table>)";

    const QVector<Torrent> torrents = RutorSource::parseSearchPage(
        html, QUrl(QStringLiteral("https://rutor.info/search/test")));
    QCOMPARE(torrents.size(), 1);

    const Torrent& t = torrents.first();
    QCOMPARE(t.hash, kHash);
    QCOMPARE(t.info.value(QStringLiteral("sourceProvider")).toString(),
        QStringLiteral("rutor"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 471557);
    QCOMPARE(t.info.value(QStringLiteral("sourceUrl")).toString(),
        QStringLiteral("https://rutor.info/torrent/471557/police-academy"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTorrentUrl")).toString(),
        QStringLiteral("https://rutor.info/download/471557"));
    QCOMPARE(t.info.value(QStringLiteral("sourceMagnet")).toString(),
        QStringLiteral("magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567&tr=udp://tracker.rutor.info:2710/announce"));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
    QCOMPARE(t.seeders, 42);
    QCOMPARE(t.leechers, 3);
    QVERIFY(t.size > 3LL * 1024 * 1024 * 1024);
}

void TestRutorSource::detailPageVerifiesSameHashAndRichReleaseInfo()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Police Academy BDRip-AVC 60 fps");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceTopicId")] = 471557;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/471557/police-academy");

    const QByteArray html = R"(
      <html><body>
      <table id="details"><tbody>
        <tr><td>Качество</td><td>BDRip-AVC 1080p 60 fps</td></tr>
        <tr><td>Видео</td><td>AVC / H.264, 1920x1080, 59.940 fps, 14 Mbps</td></tr>
        <tr><td>Аудио #1</td><td>Russian AC3 5.1, 640 kbps</td></tr>
        <tr><td>Аудио #2</td><td>English DTS 5.1, 1509 kbps</td></tr>
        <tr><td>Субтитры</td><td>Russian, English</td></tr>
        <tr><td>Описание</td><td>
          Detailed information about this exact release, its source master,
          encoding parameters, frame rate, audio tracks and subtitles. This is
          deliberately long enough that a tiny generic synopsis cannot satisfy
          the strict completeness policy by accident.
        </td></tr>
      </tbody></table>
      <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>
      </body></html>)";

    QVERIFY(RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("https://rutor.info/torrent/471557/police-academy"))));
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QVERIFY(!t.info.value(QStringLiteral("quality")).toString().isEmpty());
    QVERIFY(!t.info.value(QStringLiteral("video")).toString().isEmpty());
    QCOMPARE(t.info.value(QStringLiteral("audioTracks")).toArray().size(), 2);
    QVERIFY(RutorSource::isStrictComplete(t));
}

void TestRutorSource::audioDetailPageIsStrictWithoutVideo()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Sade - Diamond Life (1984) [FLAC]");
    t.contentType = rats::domain::ContentType::Audio;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceUrl")] = QStringLiteral("https://rutor.info/torrent/99/sade-diamond-life");
    const QByteArray html = R"(
      <table id="details"><tbody>
        <tr><td>Исполнитель</td><td>Sade</td></tr>
        <tr><td>Альбом</td><td>Diamond Life</td></tr>
        <tr><td>Формат</td><td>FLAC</td></tr>
        <tr><td>Битрейт</td><td>Lossless</td></tr>
        <tr><td>Тип рипа</td><td>tracks + .cue</td></tr>
        <tr><td>Описание</td><td>Exact album release with a complete track listing, rip provenance,
        lossless audio format, cue sheet information and edition notes. This text is intentionally long enough
        to prove that a concrete music release can be strict-complete without fake video fields.</td></tr>
      </tbody></table>
      <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>)";
    QVERIFY(RutorSource::applyDetailPage(t, html,
        QUrl(QStringLiteral("https://rutor.info/torrent/99/sade-diamond-life"))));
    QVERIFY(t.info.value(QStringLiteral("video")).toString().isEmpty());
    QVERIFY(t.info.value(QStringLiteral("audioTracks")).toArray().size() >= 2);
    QVERIFY(RutorSource::isStrictComplete(t));
}

void TestRutorSource::sourceCategoryClassifiesMajorTypes()
{
    struct Case {
        const char* category;
        rats::domain::ContentType type;
    };
    const Case cases[] = {
        { "Музыка", rats::domain::ContentType::Audio },
        { "Игры", rats::domain::ContentType::Games },
        { "Софт", rats::domain::ContentType::Software },
        { "Книги и журналы", rats::domain::ContentType::Books },
        { "Картинки и обои", rats::domain::ContentType::Pictures },
        { "Зарубежные фильмы", rats::domain::ContentType::Video },
        { "Сериалы", rats::domain::ContentType::Video },
    };

    for (const Case& c : cases) {
        Torrent t;
        t.hash = kHash;
        t.name = QStringLiteral("Exact release");
        t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
        t.info[QStringLiteral("sourceUrl")]
            = QStringLiteral("https://rutor.info/torrent/100/exact");

        const QString html = QStringLiteral(R"(
          <html><body>
          <table id="details"><tr><td>Описание</td><td>
            This is a deliberately substantial exact release description with
            concrete edition-specific information, packaging notes and source
            provenance. It is long enough for the strict release-page policy
            while the native tracker category supplies the content type.
          </td></tr></table>
          <table><tr><td>Категория</td><td>%1</td></tr></table>
          <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>
          </body></html>)").arg(QString::fromUtf8(c.category));

        QVERIFY2(RutorSource::applyDetailPage(
            t, html.toUtf8(),
            QUrl(QStringLiteral("https://rutor.info/torrent/100/exact"))),
            c.category);
        QCOMPARE(rats::domain::toId(t.contentType),
            rats::domain::toId(c.type));
        QCOMPARE(t.info.value(QStringLiteral("contentTypeEvidence")).toString(),
            QStringLiteral("source-category"));
        QVERIFY(RutorSource::isStrictComplete(t));
    }
}

void TestRutorSource::mp3CategoryDoesNotDependOnExactMetadataLabels()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Artist - Album (2005) MP3");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/101/artist-album-mp3");

    const QByteArray html = R"(
      <html><body>
      <table id="details"><tbody>
        <tr><td>Исполнитель</td><td>Artist</td></tr>
        <tr><td>Название</td><td>Album</td></tr>
        <tr><td>Формат/Кодек</td><td>MP3</td></tr>
        <tr><td>Битрейт аудио</td><td>320 kbps</td></tr>
        <tr><td>Описание</td><td>
          Exact MP3 edition with track listing, encoder information, source
          notes and release-specific packaging details. The wording deliberately
          differs from movie-style Audio #1 fields, because music releases must
          not disappear merely because their metadata labels are different.
        </td></tr>
      </tbody></table>
      <table><tr><td>Категория</td><td>Музыка</td></tr></table>
      <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>
      </body></html>)";

    QVERIFY(RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://rutor.info/torrent/101/artist-album-mp3"))));
    QCOMPARE(rats::domain::toId(t.contentType),
        rats::domain::toId(rats::domain::ContentType::Audio));
    QCOMPARE(t.info.value(QStringLiteral("sourceCategory")).toString(),
        QStringLiteral("Музыка"));
    QVERIFY(t.info.value(QStringLiteral("audioTracks")).toArray().size() >= 2);
    QVERIFY(RutorSource::isStrictComplete(t));
}

void TestRutorSource::mismatchedDetailHashIsRejected()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Expected release");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/1/wrong");

    const QByteArray html
        = R"(<table id="details"><tr><td>Качество</td><td>1080p</td></tr></table>
             <a href="magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa">magnet</a>)";

    QVERIFY(!RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("https://rutor.info/torrent/1/wrong"))));
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
}

void TestRutorSource::incompleteDetailPageIsNotStrict()
{
    Torrent t;
    t.hash = kHash;
    t.name = QStringLiteral("Some Movie 1080p");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("rutor");
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://rutor.info/torrent/2/some-movie");

    const QByteArray html = R"(
      <table id="details"><tr><td>Описание</td><td>
        This page has an exact magnet but deliberately omits concrete video and
        audio track information, so it must never enter the strict result set.
        The description is long enough to prove that length alone is not enough.
      </td></tr></table>
      <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567">magnet</a>)";

    QVERIFY(RutorSource::applyDetailPage(
        t, html, QUrl(QStringLiteral("https://rutor.info/torrent/2/some-movie"))));
    QVERIFY(!RutorSource::isStrictComplete(t));
}

QTEST_MAIN(TestRutorSource)
#include "test_rutor_source.moc"
