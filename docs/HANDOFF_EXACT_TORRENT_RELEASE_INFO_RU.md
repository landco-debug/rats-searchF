# HANDOFF — точная информация о конкретной торрент-раздаче (RU)

Дата: 2026-09-26  
Репозиторий: `landco-debug/rats-searchF`  
Основная ветка: `master`  
Экспериментальная ветка: `fix/rich-torrent-metadata`  
PR: #4 — `Restore rich torrent media/release information with multi-source fallbacks`  
Состояние PR: **НЕ МЕРЖИТЬ как готовое решение**. CI зелёный, но пользователь функционально отверг текущий подход.

## 1. Главная цель пользователя

Нужно показывать **информацию именно о выбранной конкретной раздаче / конкретном видеофайле**, а не общую информацию о фильме.

Нужные данные, когда они реально доступны:
- качество/источник видео (BDRip, BluRay, WEB-DL, REMUX и т. п.);
- разрешение, видеокодек, HDR/DV, bit depth;
- **конкретные аудиодорожки**, языки, тип перевода/озвучки, кодеки/каналы;
- субтитры;
- полное описание релиза;
- постер и описание фильма допустимы только как дополнение;
- если автоматически получить карточку нельзя — можно дать ссылку **только на реально найденную страницу этой конкретной раздачи**.

Недопустимо:
- выдавать общий synopsis фильма вместо сведений о релизе;
- считать `1080p` или `Movie · 1080p` достаточным результатом;
- давать «fallback» кнопку, которая просто ищет название фильма/релиза и приводит на страницу «ничего не найдено»;
- давать exact-hash URL, если конкретный сайт не знает этот hash;
- подменять точную аудиодорожку догадкой по фильму.

## 2. Что пользователь считает эталоном

На одной из раздач `В осаде / Under Siege (1992)` программа смогла показать хороший блок:
- Quality: 1080p · BDRip
- Video: HEVC / H.265 · Dolby Vision · HDR
- Audio tracks: много отдельных дорожек с русскими переводами/студиями, AC3, каналами/битрейтами
- Subtitles
- Genres / Runtime / IMDb / Director / Cast
- постер
- большой `RELEASE DESCRIPTION`

Именно такой уровень сведений о **раздаче** нужен по возможности для каждого выбранного результата.

Проблемные тесты:
1. `Se7en.1995.REMASTERED.MULTi.VF2.1080p.HDLight.AC3.5.1.H264-LiHDL.mkv`, около 3.63 GB.
   - текущая ветка показывала только 1080p/H264/AC3/5.1 + общий synopsis;
   - кнопки поиска на Rutor/RuTracker ничего не находили;
   - Wikipedia дала мусорную disambiguation-страницу `Se7en ... may also refer to`.
2. `Полицейская академия Антология ... HDRip-AVC` и `Полицейская академия AVC 60fps R.G. HD-Films`.
   - exact-hash кнопки RuTracker/1337x открывали «не найдено».
3. Ранее `Se7en (1995) [2160p] [4K] [BluRay] [5.1] [YTS.MX]` получал только очень бедные поля через Magnetz/Cinemeta — это тоже не считается решением.

## 3. Почему текущий подход признан тупиковым

В PR #4 было сделано многоуровневое runtime-обогащение:
- RuTracker
- Nyaa
- 1337x
- Rutor и зеркала
- YTS
- Cinemeta
- Torrentio
- Wikipedia (потом отключена из-за ложных совпадений)
- Magnetz
- DHT/BEP9
- данные peers
- попытка сопоставлять «переупакованный тот же релиз» по имени/файлам/размеру
- EXT и OxTorrent как дополнительные каталоги

Технически код собирается, но реальная проблема остаётся: **после того как Rats Search уже имеет торрент, мы пытаемся задним числом угадать, на каком сайте лежит соответствующая карточка**. У одного и того же payload разные сайты могут иметь другой info-hash, другие названия, другой размер .torrent, антибот/прокси, а часть раздач вообще не присутствует на проверяемых публичных сайтах.

Поэтому:
- exact hash часто не находится;
- поиск по длинному release name часто возвращает 0;
- поиск по названию фильма не доказывает принадлежность карточки конкретной раздаче;
- «похожие размер + release tags» остаётся эвристикой и не даёт гарантии;
- внешние ссылки выглядят как полезный fallback, но у пользователя реально открываются пустые результаты.

**Не продолжать бесконечно добавлять ещё сайты/регулярки в `RichMetadataResolver`. Нужна смена архитектуры.**

## 4. Ключевой новый вывод из legacy-кода — вероятный правильный путь

Оригинальный Electron/legacy Rats Search для Rutor работал принципиально иначе, чем текущая C++ реализация.

Файл: `legacy/background/strategies/rutor.js`

`findHash(hash)`:
1. смотрит локальную sidecar-БД:
   - `rutor/rutor.<первая hex-цифра hash>.json`
   - затем `rutor/rutor.x.json`
2. если файла нет, вызывает **`p2p.file('rutor')`**;
3. в JSON находится словарь `hashes`, то есть **точное соответствие info-hash -> Rutor topic id**;
4. после нахождения id вызывается `parse(id)` и уже открывается конкретная страница `/torrent/<id>`.

То есть legacy не пытался для каждого клика заново «поиском по названию» найти страницу Rutor. Он использовал **распределённую P2P sidecar-базу provenance / hash -> tracker topic id**.

Также `rutor.js::recheck()`:
- проходил browse-страницы Rutor;
- извлекал magnet hash и topic id;
- записывал `rutor.x.json` с `hashes[hash] = id`.

Файл: `legacy/background/p2p.js`
- содержит полноценный `file(path,...)` для получения файлов/директорий от peers;
- именно этот механизм позволял распространять такие sidecar-индексы между узлами.

**Это главный кандидат для нового принципиального подхода.**

## 5. Рекомендация новому чату: архитектура provenance-at-index-time

Приоритетное направление исследования:

### Вариант A — восстановить legacy sidecar index в native C++
Понять, чего не хватает современной C++ P2P реализации для аналога:
- скачать/получить от peers `rutor/rutor.<prefix>.json`;
- хранить exact `infoHash -> source tracker + topic id/url`;
- обновлять карту фоновым crawler/recheck, как legacy;
- по клику UI получает конкретный topic id и парсит конкретную страницу.

Это значительно ближе к исходной архитектуре, которая реально работала.

### Вариант B — ещё лучше: сохранять provenance в момент индексирования
Когда spider/indexer впервые видит торрент:
- сохранять `sourceTracker`;
- `sourceTopicId` / `sourceUrl`;
- при возможности snapshot нужного release description / poster / technical metadata;
- передавать эти поля вместе с Torrent через P2P;
- для уже индексированных старых записей сделать миграцию/фоновое дозаполнение.

Тогда Details Panel не должен «угадывать источник» после факта. Он просто открывает/показывает provenance, пришедший вместе с индексной записью.

Проверить, есть ли источник/provenance в текущем spider/indexing pipeline и где он теряется до `domain::Torrent`.

### Вариант C — распределённый metadata record
Если прямой source URL нельзя сохранять всегда, сделать отдельный P2P record:
`infoHash -> { source, topicId/url, capturedAt, description, mediaInfo, poster }`.
Кэшировать и раздавать соседям.

## 6. Что в текущей ветке всё же можно сохранить

Текущая ветка не бесполезна полностью. Отдельные части можно cherry-pick/перенести после выбора новой архитектуры:

- `media_metadata_utils.*` — парсинг release-тегов из точного текста/имени файла;
- обработка `VF2/VFF/VFQ/MULTi` как **release tags**, а не как доказательство полной аудиокарты;
- UI с отдельными Quality/Video/Audio/Languages/Subtitles;
- отделение `RELEASE DESCRIPTION` от общего `SYNOPSIS`;
- логика: sparse metadata не считается окончательным успехом;
- UI-width fixes:
  - `1df94f52` — grid для ссылок;
  - `41c14639` — controls не заставляют панель расширяться;
  - `139a8f7c` — details panel max width / splitter stretch.
- тест `tests/test_media_metadata.cpp`.

Но любые `RichMetadataResolver`-поиски по сторонним сайтам нужно рассматривать как эксперимент, а не основу новой версии.

## 7. Что НЕ нужно повторять

Не тратить новый чат на очередной цикл:
1. добавить ещё один torrent-сайт;
2. составить URL поиска по title/hash;
3. показать кнопку пользователю;
4. обнаружить, что сайт ничего не нашёл.

Это уже многократно проверено и не решает задачу.

Не использовать generic movie APIs (Cinemeta/Wikipedia) как доказательство точности раздачи. Они могут использоваться только как optional enrichment уже после идентификации конкретной release/source record.

## 8. UI — отдельная проблема

Пользователь отдельно сообщил, что интерфейс начал растягиваться по горизонтали и ссылки/кнопки выглядели некорректно/обрезались.

В PR #4 сделаны частичные фиксы:
- max width Details Panel = 500;
- splitter: результаты растягиваются, details нет;
- длинные QLabel получили `QSizePolicy::Ignored`;
- link buttons переведены с HBox в 2-column grid;
- max width кнопки 210.

Но пользователь финально заявил, что «улучшения зашли в тупик, ничего не меняется», поэтому **UI тоже перепроверить в новом подходе, не считать полностью закрытым**.

Лучше сначала вернуть/сравнить геометрию с `master` и менять минимально.

## 9. Git state

Стабильный baseline перед экспериментом:
- `master`: `fd72202acda7aba181726abd911e6e68f7183b8e`
- message: `fix: re-sign final macOS bundle after Manticore packaging`

Эксперимент:
- branch: `fix/rich-torrent-metadata`
- PR: #4
- head перед этим handoff: `67e94fed161f96634834fbd4e6d05a5b03c2690d`
- branch ahead of master: 48 commits
- PR mergeable, но **функционально отвергнут пользователем**
- latest CI at `67e94fed...`: success (Linux, Windows, macOS ARM; macOS Intel intentionally skipped)

Изменённые файлы PR #4:
- `src/net/media_metadata_utils.cpp/.h`
- `src/net/rich_metadata_resolver.cpp/.h`
- `src/net/tracker_site_scraper.cpp/.h`
- `src/ui/mainwindow.cpp`
- `src/ui/torrentdetailspanel.cpp/.h`
- `tests/CMakeLists.txt`
- `tests/test_media_metadata.cpp`

Рекомендация по старту соседнего чата:
- **не merge PR #4**;
- сначала исследовать legacy P2P sidecar/provenance architecture;
- для чистой реализации создать новую ветку от `master@fd72202...`;
- cherry-pick из PR #4 только те маленькие куски, которые действительно нужны.

## 10. Полный журнал 48 функциональных коммитов PR #4

1. `8a0b1650` feat: add media metadata parsing helpers
2. `3165c984` feat: parse rich video and audio release metadata
3. `4eda2992` feat: add multi-source rich metadata resolver
4. `35d6ba86` feat: enrich torrents from YTS Cinemeta and Torrentio
5. `05e91a85` feat: restore rich tracker source declarations
6. `018c1db4` feat: enable legacy rich tracker sources
7. `599617d4` feat: restore 1337x and Rutor rich metadata strategies
8. `f37e4207` fix: prefer richest tracker description and aggregate technical info
9. `eca9b808` test: cover rich media metadata parsing
10. `f0389142` test: register media metadata parser test
11. `d4dde84d` feat: prepare rich media information UI state
12. `5f600e6b` feat: add rich media release section to details panel
13. `4b1947b6` feat: resolve human-facing torrent metadata through rich multi-source fallbacks
14. `9a94af33` fix: include JSON types for rich metadata resolver
15. `44ed9ec8` fix: include QStringList for restored tracker strategies
16. `538c8ac4` feat: present audio video synopsis and release metadata instead of tracker URLs
17. `89b4a625` fix: use unambiguous QNetworkRequest construction
18. `9cc01627` test: include media metadata parser in check target
19. `98689872` fix: construct 1337x detail request without vexing parse
20. `ba9387e5` fix: parse dotted episode names and numbered audio tracks
21. `10ff7017` fix: retain full tracker release specifications
22. `29548bb3` fix: preserve tracker table rows in rich descriptions
23. `63d8f36f` feat: add Wikipedia metadata fallback
24. `f618148b` feat: enrich localized titles from Wikipedia
25. `c1f5564f` fix: infer common release audio and language tags from filenames
26. `d167c5af` feat: add identity-derived metadata and guaranteed reference links
27. `4e7d40af` fix: treat sparse quality as incomplete and mine selected torrent filenames
28. `7607e2f7` feat: derive exact media tags from names and keep fallback chain running
29. `ae992936` feat: always provide fallback description links for unresolved torrents
30. `9d35852d` fix: expose description-site links even when automatic lookup fails
31. `d1360c2d` fix: require real audio and language detail before marking release info complete
32. `74c55117` fix: match Rutor info hashes directly in search results
33. `91867049` feat: add exact release matching by title files and size
34. `e085142f` feat: resolve exact rehashed releases by fingerprint and size
35. `6370f029` feat: pass exact release size and file list to resolver
36. `1df94f52` fix: use wrapping grid for metadata links
37. `41c14639` fix: prevent metadata controls from forcing horizontal expansion
38. `139a8f7c` fix: keep torrent details panel compact on wide displays
39. `d39833c5` feat: expose matched release page before detail fetch
40. `47b86928` fix: remove dead hash links and search only by full release identity
41. `1caa2f69` fix: escape quote entity replacement
42. `1df3a8b6` feat: add OxTorrent exact-release resolver
43. `a169b61f` feat: add EXT exact-release catalog fallback
44. `debf05a2` feat: resolve concrete releases via EXT and OxTorrent
45. `1fd7a7a7` fix: show only verified concrete release links
46. `d77539ed` fix: avoid ambiguous Wikipedia synopsis fallback
47. `f7bb459f` fix: hide stale ambiguous synopsis text
48. `67e94fed` feat: decode VF2 VFF VFQ and MULTi audio tags

## 11. Важная оговорка по последним коммитам

Последние EXT/OxTorrent/Rutor matching additions могут выглядеть разумно по коду и проходить CI, но **пользователь уже признал весь этот класс решения неэффективным**. Не надо считать зелёный CI доказательством решения задачи. CI подтверждает только сборку/тесты.

## 12. Definition of Done для нового подхода

На случайно выбранной раздаче из списка:
1. UI показывает source provenance или явно говорит, что provenance неизвестен.
2. Если источник известен — ссылка открывает **конкретную страницу этой раздачи**, а не search results.
3. Автоматически извлекается release description/MediaInfo именно с этой страницы или из сохранённого snapshot.
4. Аудиодорожки/видео/субтитры относятся к выбранному релизу.
5. При неизвестном provenance нет ложных «точных» кнопок.
6. Общая информация о фильме визуально отделена от release-specific данных.
7. Правая панель не ломает геометрию основного списка.
8. Проверить как минимум на `Under Siege`, `Se7en LiHDL 3.63 GB`, `Police Academy anthology`.
