#include <QtTest>

#include "net/kinozal_source.h"

using rats::domain::Torrent;
using rats::net::KinozalSource;

class TestKinozalSource : public QObject {
    Q_OBJECT
private slots:
    void buildsCurrentMirrorSearchUrl();
    void parsesFullListingTitleAndExactId();
    void preservesListingTitleOnDetailPage();
    void serverDetailsCompletesHashAndFiles();
    void validatesExactDetailMirrors();
};

void TestKinozalSource::buildsCurrentMirrorSearchUrl()
{
    const QUrl url = KinozalSource::searchUrl(
        QUrl(QStringLiteral("https://kinozal.me")),
        QStringLiteral("Пчеловод"),
        QStringLiteral("seeders_desc"));
    QVERIFY(url.isValid());
    QCOMPARE(url.host(), QStringLiteral("kinozal.me"));
    QCOMPARE(url.path(), QStringLiteral("/browse.php"));
    const QByteArray encoded = url.toEncoded();
    QVERIFY(encoded.contains("s="));
    QVERIFY(encoded.contains("g=0"));
    QVERIFY(encoded.contains("c=0"));
    QVERIFY(encoded.contains("t=1"));
    QVERIFY(encoded.contains("f=0"));
}

void TestKinozalSource::parsesFullListingTitleAndExactId()
{
    const QByteArray html = QStringLiteral(R"(
      <!DOCTYPE HTML><html lang="ru"><head>
      <title>Раздачи :: Кинозал.GURU</title></head><body>
      <div id="user"><a href="/login.php?m=1">Выход</a></div>
      <table class="t_peer w100p"><tbody>
      <tr class='first bg'>
       <td class="bt"><img onclick="cat(6);"></td>
       <td class="nam"><a href="/details.php?id=2147304" class="r1"><b>Пчеловод</b> / The Beekeeper (Дэвид Эйр / David Ayer) / 2024 / 2 x ДБ, ПМ, СТ / 4K, HEVC, HDR10+ / WEB-DL (2160p)</a></td>
       <td class='s'>24</td>
       <td class='s'>26.75 ГБ</td>
       <td class='sl_s'>63</td>
       <td class='sl_p'>4</td>
       <td class='s'>23.06.2024 в 10:56</td>
      </tr></tbody></table></body></html>)").toUtf8();

    const QVector<Torrent> rows = KinozalSource::parseSearchPage(
        html, QUrl(QStringLiteral(
            "https://kinozal.guru/browse.php?s=test")));
    QCOMPARE(rows.size(), 1);

    const Torrent& t = rows.first();
    QCOMPARE(t.info.value(QStringLiteral("sourceProvider")).toString(),
        QStringLiteral("kinozal"));
    QCOMPARE(t.info.value(QStringLiteral("sourceTopicId")).toInt(), 2147304);
    QCOMPARE(t.info.value(QStringLiteral("sourceUrl")).toString(),
        QStringLiteral("https://kinozal.guru/details.php?id=2147304"));
    QCOMPARE(t.seeders, 63);
    QCOMPARE(t.leechers, 4);
    QVERIFY(t.size > 26LL * 1024 * 1024 * 1024);
    QVERIFY(t.name.contains(QStringLiteral(
        "Пчеловод / The Beekeeper")));
    QVERIFY(t.name.contains(QStringLiteral(
        "WEB-DL (2160p)")));
    QVERIFY(t.added.isValid());
    QCOMPARE(rats::domain::toString(t.contentType),
        QStringLiteral("video"));
}

void TestKinozalSource::preservesListingTitleOnDetailPage()
{
    Torrent t;
    t.name = QStringLiteral(
        "Пчеловод / The Beekeeper / 2024 / ДБ, ПМ, СТ / BDRip 1080p");
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("kinozal");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://kinozal.me/details.php?id=777");

    const QByteArray html = QStringLiteral(R"(
      <html><head><title>Пчеловод / Кинозал.GURU</title></head><body>
      <div>Кинозал.GURU</div>
      <b>Продолжительность:</b> 01:45:14<br>
      <b>Перевод:</b> Профессиональный (дублированный)<br>
      <b>Качество:</b> BDRip 1080p<br>
      <b>Видео:</b> AVC, 1920x1080, 23.976 fps<br>
      <b>Аудио:</b> AC3, 6 ch, Русский<br>
      <b>Субтитры:</b> Русские, Английские<br>
      <p>Подробное описание именно этой конкретной раздачи с информацией
      о фильме, варианте перевода, исходнике, видео и звуковых дорожках.
      Этот текст достаточно длинный для строгой exact-source карточки.</p>
      </body></html>)").toUtf8();

    QVERIFY(KinozalSource::applyDetailPage(
        t, html, QUrl(QStringLiteral(
            "https://kinozal.me/details.php?id=777"))));

    QCOMPARE(t.name, QStringLiteral(
        "Пчеловод / The Beekeeper / 2024 / ДБ, ПМ, СТ / BDRip 1080p"));
    QVERIFY(t.info.value(QStringLiteral("detailVerified")).toBool());
    QVERIFY(!t.info.value(QStringLiteral("sourceVerified")).toBool());
    QVERIFY(t.info.value(QStringLiteral("description")).toString()
        .contains(QStringLiteral("Продолжительность")));
}

void TestKinozalSource::serverDetailsCompletesHashAndFiles()
{
    Torrent t;
    t.name = QStringLiteral(
        "Пчеловод / The Beekeeper / 2024 / BDRip 1080p");
    t.contentType = rats::domain::ContentType::Video;
    t.info[QStringLiteral("sourceProvider")] = QStringLiteral("kinozal");
    t.info[QStringLiteral("sourceTopicId")] = 777;
    t.info[QStringLiteral("sourceUrl")]
        = QStringLiteral("https://kinozal.me/details.php?id=777");
    t.info[QStringLiteral("detailVerified")] = true;
    t.info[QStringLiteral("description")] = QStringLiteral(
        "Кинозал. Подробное описание конкретной раздачи фильма, качество "
        "BDRip 1080p, видео AVC, русские и оригинальные аудиодорожки, "
        "субтитры и сведения об исходнике. Это exact-source описание.");

    const QByteArray srv = QStringLiteral(R"(
      <ul>
       <li>Инфо хеш: 0123456789abcdef0123456789abcdef01234567</li>
       <li>Размер части торрента: 8 МБ</li>
       <li><div class='b ing'>The.Beekeeper.2024.BDRip.1080p.mkv <i>1.46 ГБ (1567663063)</i></div></li>
       <li><div class='b ing'>sample.txt <i>1 КБ (1024)</i></div></li>
      </ul>)").toUtf8();

    QVERIFY(KinozalSource::applyServerDetails(t, srv));
    QCOMPARE(t.hash,
        QStringLiteral("0123456789abcdef0123456789abcdef01234567"));
    QCOMPARE(t.fileList.size(), 2);
    QCOMPARE(t.files, 2);
    QCOMPARE(t.fileList.first().path,
        QStringLiteral("The.Beekeeper.2024.BDRip.1080p.mkv"));
    QCOMPARE(t.fileList.first().size, 1567663063LL);
    QCOMPARE(t.pieceLength, 8 * 1024 * 1024);
    QVERIFY(t.info.value(QStringLiteral("sourceVerified")).toBool());
    QVERIFY(KinozalSource::isStrictComplete(t));
}

void TestKinozalSource::validatesExactDetailMirrors()
{
    QVERIFY(KinozalSource::isExactDetailUrl(
        QUrl(QStringLiteral("https://kinozal.me/details.php?id=55")), 55));
    QVERIFY(KinozalSource::isExactDetailUrl(
        QUrl(QStringLiteral("https://kinozal.guru/details.php?id=55")), 55));

    QVERIFY(!KinozalSource::isExactDetailUrl(
        QUrl(QStringLiteral("https://kinozal.me/details.php?id=56")), 55));
    QVERIFY(!KinozalSource::isExactDetailUrl(
        QUrl(QStringLiteral("https://kinozal.tv/details.php?id=55")), 55));
    QVERIFY(!KinozalSource::isExactDetailUrl(
        QUrl(QStringLiteral("https://evil.example/details.php?id=55")), 55));
    QVERIFY(!KinozalSource::isExactDetailUrl(
        QUrl(QStringLiteral("https://kinozal.me/browse.php?id=55")), 55));
}

QTEST_MAIN(TestKinozalSource)
#include "test_kinozal_source.moc"
