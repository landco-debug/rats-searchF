# AGENTS.md

## Project handoff policy

- Hardware target: MacBook Air M1, macOS Sequoia.
- Repository: `landco-debug/rats-searchF`.
- Stable baseline for this work: `master@fd72202acda7aba181726abd911e6e68f7183b8e`.
- Working branch: `feat/rutor-strict-source`.
- Every functional stage must be a Git commit and must add/update its own handoff subsection here.
- Do not merge experimental work into `master` until the user accepts the runtime behavior.
- Canonical problem statement: exact release information must belong to the selected concrete torrent; generic search links are not acceptable.

## Rutor strict-source implementation

### Stage 1 — exact provenance parser and strict completeness core

Commit message: `feat: add strict Rutor provenance parser`.

Purpose:
- stop trying to discover a tracker page after a DHT/P2P torrent was already found;
- derive `sourceUrl`, Rutor topic id and info-hash from the same Rutor search row;
- verify the exact info-hash again on the concrete Rutor detail page;
- reject detail pages that do not contain enough release-specific information.

Implemented:
- new `src/net/rutor_source.{h,cpp}`;
- Rutor search URL builder with native sort mapping;
- search-page parser that stores `sourceProvider=rutor`, `sourceTopicId`, exact `sourceUrl`, and `sourceVerified=false`;
- detail-page parser that sets `sourceVerified=true` only when the page magnet matches the exact torrent hash;
- strict completeness rule requiring a verified concrete page plus substantial description, quality, video and at least one explicit audio-track line;
- unit tests for provenance, exact-hash verification, mismatch rejection and incomplete-page rejection.

Not implemented yet:
- no live HTTP fetching;
- no wiring into `Application` or the main search UI;
- no change to existing DHT/P2P search behavior yet.

Next stage:
- add a small asynchronous live Rutor fetcher around this tested parsing core, still without changing the existing GUI search path.


### Stage 2 — asynchronous live Rutor transport

Commit message: `feat: add asynchronous strict Rutor search client`.

Stage 1 verification before starting this stage:
- macOS ARM (Apple Silicon): Build success, Run tests success;
- Linux x64: Build success, Run tests success;
- Windows x64: Build success, Run tests success;
- draft CI PR: #6.

Implemented:
- new `src/net/rutor_search_client.{h,cpp}`;
- asynchronous search-page HTTP request using the exact Rutor title-search URL;
- fallback from `rutor.info` to `rutor.is` when the primary request fails or is unusable;
- bounded detail-page concurrency (4 simultaneous requests);
- each candidate keeps its exact `/torrent/<id>` provenance while queued;
- detail result is emitted only after the Stage 1 parser verifies the same info-hash and strict completeness;
- cancellation/generation guard prevents stale responses from an older query leaking into a newer query;
- detail-page mirror retry is used only for network failure, not to weaken identity/completeness checks.

Still intentionally not implemented:
- no `Application` ownership/wiring yet;
- no replacement of the GUI's existing local/P2P/DHT search yet;
- no user-visible behavior change in this stage.

Next stage:
- wire one `RutorSearchClient` into `Application` and expose it through an accessor, without changing the search UI yet.


### Stage 3 — Application ownership, still no search UI change

Commit message: `feat: own strict Rutor client in Application`.

Stage 2 verification before starting this stage:
- macOS ARM (Apple Silicon): Build success, Run tests success;
- Linux x64: Build success, Run tests success;
- Stage 2 remained isolated from the GUI search path.

Implemented:
- `Application::Private` now owns exactly one `net::RutorSearchClient`;
- the client is constructed with the other network-layer adapters;
- `Application::rutorSearch()` exposes a non-owning accessor to front-ends;
- shutdown calls `cancel()` before tracker/network teardown so no Rutor HTTP reply can outlive application shutdown.

Still intentionally not implemented:
- `MainWindow::performSearch()` is untouched;
- existing local/P2P/DHT search results are still the user-visible behavior;
- no details-panel changes yet.

Next stage:
- replace only the Search tab orchestration with strict Rutor results, while leaving torrent download/P2P infrastructure intact.


### Stage 4 — protect verified provenance during persistence

Commit message: `fix: keep exact source metadata authoritative`.

Why this stage was inserted before the GUI switch:
- the existing duplicate-hash merge only filled missing `Torrent::info` keys;
- therefore an old guessed `description` or tracker link could survive even after Rutor proved the exact release;
- newly indexed torrents also triggered the legacy tracker-site resolver immediately.

Implemented:
- verified source metadata (`sourceVerified=true` + concrete `sourceUrl`) replaces the old info object on duplicate hash instead of merely backfilling it;
- ordinary P2P/import metadata keeps the old conservative missing-key-only merge behavior;
- `TrackerService::onTorrentIndexed()` still refreshes swarm counts but skips legacy site-info guessing for an already verified source result;
- live-Manticore integration test proves that stale guessed description/source keys are removed when exact Rutor provenance arrives.

Stage 3 verification before starting this stage:
- macOS ARM (Apple Silicon): Build success, Run tests success;
- Linux x64: Build success, Run tests success.

Next stage:
- switch only the Search tab to `RutorSearchClient` results and persist accepted releases through `IndexingService`;
- do not mix local/P2P/DHT-only rows into that search result set.


### Stage 5 — Search tab switched to strict Rutor-only discovery

Commit message: `feat: show only verified Rutor releases in search`.

Stage 4 verification before starting this stage:
- macOS ARM (Apple Silicon): Build success, Run tests success;
- the live-Manticore authoritative-metadata test passed.

User-visible behavior changed here:
- `MainWindow::performSearch()` now starts only `RutorSearchClient`;
- local-index search, remote P2P search and DHT-only fallback no longer feed the Search Results table;
- remote peer search signals are no longer connected to the Search Results model;
- a row is accepted defensively only when `sourceVerified=true`, `strictComplete=true`, and exact `sourceUrl` are present;
- accepted Rutor releases are persisted through `IndexingService`, so exact provenance can be shared/reused by the existing database/P2P infrastructure;
- existing size/type/safe/file filters are applied after classification; when a requested bound cannot be proven (for example file-count is unknown), the row is hidden rather than guessed;
- peer single-torrent replies remain available only for an already selected verified row, e.g. to obtain its file list.

Important invariant:
- Search Results must never contain a torrent merely because its title/hash was found in the old local/P2P/DHT index.
- Fewer results are expected and intentional.

Next stage:
- make Torrent Details render the verified Rutor source URL and structured quality/video/audio/subtitle facts directly from the stored exact-source snapshot;
- ensure it does not launch post-hoc fallbacks for a verified-source row.


### Stage 6 — Torrent Details renders only exact Rutor release data

Commit message: `feat: render exact Rutor release details`.

Stage 5 verification before starting this stage:
- current Stage 5 HEAD `6a01a3742cc6ce11557b7a7735fab3afea5b5dca`;
- GitHub Actions run #61 completed successfully;
- macOS ARM (Apple Silicon): Build, tests, bundle signing and DMG creation all succeeded;
- Linux x64 and Windows x64: Build and tests succeeded.

Implemented:
- a verified Rutor source URL is accepted only when `sourceProvider=rutor`, `sourceVerified=true`, host is `rutor.info` or `rutor.is`, and path is a concrete `/torrent/...` page;
- Torrent Details renders stored exact-source fields directly: `quality`, `video`, every `audioTracks` entry, `subtitles`, and the exact release URL;
- the exact release URL is both selectable text and a dedicated `Open exact Rutor release` button;
- for a verified Rutor row, `requestTrackerRefresh()` may refresh swarm counts but returns before any legacy RuTracker/Nyaa/peer/Magnetz/DHT rich-info lookup starts;
- legacy tracker/search buttons are never shown beside a verified Rutor result; only the exact Rutor page is exposed;
- the full description remains the snapshot captured from that concrete Rutor page; fallback metadata annotations are not mixed into it;
- unknown file count from a source-first result is displayed as `-`, not falsely as `0 files`.

Invariant after this stage:
- selecting a verified search result must never trigger a title-based attempt to find a different tracker page;
- the only website button for such a row must open the concrete Rutor release page stored with its verified info-hash.

Next stage:
- build the macOS ARM artifact and manually verify the UI with the known Police Academy / Under Siege cases before considering merge to `master`.


### Stage 7 — robust Rutor seeder/leecher counters

Commit message: `fix: parse Rutor swarm counters robustly`.

Observed on the Stage 6 macOS ARM build:
- exact release metadata works, but every search result displayed `0` seeders and `0` leechers.

Root cause:
- the source-first parser looked for digits immediately after the opening `span.green` / `span.red` tag;
- Rutor/mirrors may wrap the number in nested markup or use an unquoted `class=` attribute, so the row identity parsed correctly while the counter regex failed.

Implemented:
- parse the complete contents of `span.green` and `span.red`, strip nested HTML, then extract the integer;
- accept quoted and unquoted class attributes;
- unit test now covers a nested `<b>` seeder value and an unquoted/nested leecher value.

Next stage:
- fix clipped main/settings tab captions without changing search behavior.


### Stage 8 — keep tab captions readable

Commit message: `fix: stop eliding tab captions`.

Observed on macOS Sequoia:
- main tabs were shown as `Search Res...`, `Downlo...`, `Favori...`;
- Settings tabs were similarly shortened.

Implemented:
- main and Settings tab bars use `Qt::ElideNone`;
- tabs use their natural content width instead of expanding equally;
- native tab scroll buttons stay enabled as the fallback when a genuinely narrow window cannot fit all complete captions.

This is UI-only; search/source behavior is unchanged.

Next stage:
- repair macOS hide/minimize → Dock restore lifecycle and make explicit Quit stop mutating persisted tray preferences.


### Stage 9 — macOS tray/Dock restore and settings persistence

Commit message: `fix: restore hidden macOS window without losing settings`.

Observed on macOS Sequoia:
- after minimize-to-tray, activating Rats Search from the Dock could show a blank grey main window until restart;
- tray Quit could make a saved tray setting appear to revert.

Root causes found:
- `bringToFront()` called `show()` while the hidden native window was still in `WindowMinimized`, then cleared the minimized state afterwards;
- tray Quit forced shutdown by calling `ConfigStore::setTrayOnClose(false)`, and normal shutdown then persisted that artificial value.

Implemented:
- clear `WindowMinimized` before showing the hidden window;
- explicitly reactivate/show the central widget, splitters and tab widget after tray restoration;
- remember whether the window was intentionally hidden by tray/minimize logic;
- when macOS application activation comes from the Dock and that flag is set, route restoration through `bringToFront()`;
- File > Quit and tray Quit set an in-memory `forceQuit_` flag instead of changing any preference;
- close-to-tray records the hidden state but leaves settings untouched;
- Settings dialog explicitly `sync()`s its QSettings values on Save.

Next stage:
- validate these three fixes in the macOS ARM artifact; after user acceptance, add additional torrent sources using the same exact-source/strict-completeness contract rather than reintroducing post-hoc title matching.


### Stage 10 — strict public RuTracker.RU parser core

Commit message: `feat: add strict RuTracker.RU provenance parser`.

Why this source:
- current public RuTracker.RU search rows expose a concrete `viewtopic.php?t=<id>` link and magnet/info-hash in the same result row;
- that satisfies the source-first identity requirement without post-hoc title matching.

Implemented:
- new `src/net/rutracker_ru_source.{h,cpp}`;
- public search URL construction with the same sort dimensions used by the main UI;
- result-row parsing for exact topic URL, info-hash, byte size, seeders and leechers;
- detail-page identity check requires the same info-hash;
- first-post description extraction follows RuTracker's `post_body` structure;
- strict fields: quality, video, explicit audio tracks and optional subtitles/poster;
- strict completeness contract mirrors Rutor: verified concrete page + substantial exact-release description + quality + video + at least one audio track;
- unit tests cover provenance, swarm counters, exact-hash acceptance and mismatch rejection.

Not wired into the GUI yet.

Next stage:
- add the asynchronous RuTracker.RU transport and Application ownership, then aggregate Rutor + RuTracker.RU without mixing old local/P2P/DHT-only search results.


### Stage 11 — asynchronous RuTracker.RU transport and Application ownership

Commit message: `feat: add strict RuTracker.RU search client`.

Implemented:
- new `src/net/rutracker_ru_search_client.{h,cpp}`;
- asynchronous public RuTracker.RU search request;
- bounded detail-page verification (4 concurrent requests);
- every emitted result must pass Stage 10 exact-infohash verification and strict completeness;
- generation/cancellation guards prevent stale query results;
- `Application` owns one `RuTrackerRuSearchClient`, exposes it through `ruTrackerRuSearch()`, and cancels it during shutdown.

Still intentionally unchanged:
- Search Results continues to call only Rutor at this stage;
- Torrent Details still recognizes only Rutor as a user-visible exact-source provider.

Next stage:
- aggregate both strict clients in Search Results, deduplicate by info-hash, keep source provenance, and generalize exact-source details rendering to Rutor + RuTracker.RU.


### Stage 12 — aggregate Rutor + RuTracker.RU in Search Results

Commit message: `feat: search Rutor and RuTracker.RU exact releases`.

Implemented:
- Search Results launches Rutor and public RuTracker.RU strict clients for the same query/sort;
- both providers must still pass `sourceVerified=true`, `strictComplete=true` and carry a concrete `sourceUrl`;
- same info-hash is deduplicated across providers before indexing/display;
- combined completion status waits for both providers and reports provider-specific failures without discarding successful results from the other source;
- local-index/P2P/DHT-only discovery remains excluded from Search Results;
- Torrent Details now recognizes both exact-source contracts:
  - Rutor: `rutor.info|rutor.is/torrent/...`;
  - RuTracker.RU: `rutracker.ru/viewtopic.php?t=...`;
- source label and exact-release button are provider-specific; no legacy guessed links are mixed into a verified-source row.

Important:
- RuTracker.RU is public and needs no account in this implementation.
- Semi-private sources (Kinozal/Rustorka/etc.) are NOT silently enabled; they require explicit credential/settings work.
- MegaPeer/NewStudio are also not yet enabled because their search rows do not expose an info-hash directly; exact identity must be derived from their torrent/detail payload first rather than weakened to title matching.

Next stage:
- macOS ARM runtime validation of Rutor + RuTracker.RU;
- then add the next public source only if its exact info-hash can be proven from the concrete release payload.


### Stage 13 — deterministic macOS test artifact revision

Commit message: `ci: build macOS artifact from exact branch head`.

Problem found while verifying the file handed to the user:
- GitHub Actions `pull_request` jobs checked out GitHub's temporary PR merge ref;
- run #71 therefore packaged `RatsSearch-macOS-ARM-dev-6421483.dmg`, where `6421483` was the temporary merge commit, not branch HEAD `282e25f...`;
- although that merge contained the branch changes, the delivered file was not self-identifying as the exact working HEAD and could be mistaken for/stale against the requested build.

Implemented for the Apple Silicon job:
- checkout `github.event.pull_request.head.sha` on PR events, falling back to `github.sha` otherwise;
- immediately compare `git rev-parse HEAD` with the expected source SHA and fail on mismatch;
- name development DMGs `RatsSearch-macOS-ARM-head-<branch-head>.dmg`;
- add `BUILD-REVISION.txt` at the DMG root containing the full source SHA.

This stage changes CI/package provenance only; application behavior from Stages 7-12 is unchanged.


### Stage 14 — on-demand exact torrent file lists

Commit message: `feat: resolve selected torrent file lists on demand`.

User-visible goal:
- replace the empty `No files` bottom panel for strict-source search results with the real contents of the selected torrent;
- do this without weakening source identity or downloading the media payload.

Implemented:
- Rutor search provenance now also stores `sourceTorrentUrl` from the same `a.downgif` result row that provided the exact detail URL and info-hash;
- when a selected torrent already has files in the repository, the existing instant path remains unchanged;
- otherwise the Files panel shows `Loading torrent metadata…`, not a false `No files`;
- for Rutor, Rats Search first downloads the exact source `.torrent`, parses it with the existing `TorrentEngine::readTorrentFile()`, and accepts it ONLY when its computed info-hash equals the selected verified hash;
- if the direct source file is unavailable/unparseable/mismatched, the resolver falls back to existing DHT/BEP 9 `TorrentEngine::fetchMetadata()` for the same verified info-hash;
- public RuTracker.RU goes directly to the BEP 9 path because its public search contract has exact topic+magnet provenance but no stable direct download field;
- BEP 9 downloads metadata only, not movie/content pieces;
- a 20-second BEP 9 timeout and stale-selection generation guard prevent an old selection from repainting the current file panel;
- successful file lists are persisted with `TorrentRepository::updateFiles()`, so subsequent selections use the local database immediately;
- the lower tree shows real paths and sizes and keeps the existing file-selection UI for later downloads;
- source `.torrent` responses are capped at 32 MiB before parsing;
- `TorrentFilesWidget` now distinguishes loading and unavailable states.

Identity invariant:
- HTML title matching is never used for file contents;
- every accepted file list comes either from a source `.torrent` whose computed hash matches the verified result, or from BEP 9 metadata addressed by that exact info-hash.

Next validation:
- macOS ARM: select a Rutor result with no cached files and confirm the tree appears quickly;
- select a RuTracker.RU result and confirm BEP 9 fills the tree when swarm metadata is available;
- reselect either result and confirm the cached file list appears immediately.


### Stage 15 — first-pass Date/Video correctness and compact search columns

Commit message: `fix: compact strict search results and filters`.

Observed on the validated Stage 14 macOS ARM build:
- Date was blank on the first source search and appeared only after repeating the query;
- Date included hours/minutes although only the day is useful in the compact result table;
- selecting the main `Video` type produced an empty table while the same query under `All types` returned verified video releases;
- full `Seeders` / `Leechers` headings consumed too much horizontal space;
- the old self-update check is not appropriate for this fork, where test builds are delivered from the project's GitHub Actions.

Root causes:
- a fresh source-first torrent has no local-index `added` timestamp until its first persistence round-trip;
- the legacy content classifier normally derives type from file extensions, but strict results are displayed before Stage 14 resolves the torrent file list, so they were still `Unknown` when the search type filter ran.

Implemented:
- assign `added=UTC now` before the first strict-source insert/display when it is missing, matching the existing local-index "added" semantics but removing the first-search blank;
- Date displays `yyyy-MM-dd` only;
- strict results carrying the already-verified non-empty `video` field are classified as `Video` before filtering;
- older stored exact-source rows with `Unknown` type are healed through `TorrentRepository::updateClassification()`;
- new searches clear the previous selection, details panel, files panel and stale file-metadata request context;
- result headers use compact Cyrillic `С` / `Л` with full Seeders/Leechers tooltips;
- `С` is theme green (`success`) and `Л` is theme red (`danger`);
- fixed widths: Size 92 px, С 42 px, Л 42 px, Date 96 px; Name stretches into the reclaimed space;
- Help > Check for Updates is removed;
- automatic startup update checking is disabled;
- the Updates group/checkbox is removed from Settings.

Preserved intentionally:
- UpdateService/config compatibility code remains compiled but dormant; the desktop UI no longer initiates update checks;
- strict Rutor + RuTracker.RU source identity, Stage 14 exact file-list resolution, downloads, P2P/DHT background services and existing settings are otherwise unchanged.

Next validation:
- first search must show Date immediately and without time;
- Video must return the same valid strict video releases rather than an empty table;
- С/Л headers must be compact and visibly green/red;
- Help and Settings must contain no update-check control.


### Stage 16 — authoritative type filters, Latin S/L and immediate state persistence

Commit message: `fix: make strict source type filters authoritative`.

Regression found in Stage 15:
- source strict-completeness was still video-only, so Audio/Books/Games/Software/etc. were rejected before the UI filter could see them;
- MainWindow additionally forced rows carrying Video metadata to Video;
- Cyrillic С/Л were used instead of standard Latin S/L, and native macOS headers ignored model ForegroundRole colours;
- type selection was only guaranteed to persist during orderly window shutdown;
- old saved QHeaderView geometry could restore wide pre-compact columns.

Implemented:
- selected type is passed into both strict source clients; MainWindow no longer force-labels results as Video;
- Rutor typed searches download the exact source .torrent from the same row, verify its info-hash, classify from the real file list, and only matching types proceed to exact detail-page verification;
- typed Rutor results retain that verified file list for immediate Files-panel display;
- RuTracker.RU rows capture sourceForumId and map public forum categories to Video/Audio/Books/Games/Software; supported typed queries send those exact f[] forum IDs to tracker.php and validate the returned row type again;
- strict completeness is type-aware: Video still requires Quality+Video+Audio, Audio requires exact provenance + substantial description + concrete audio technical fields, other non-video types require exact provenance + substantial release-specific text;
- audio parsing includes Format/Формат, Codec/Кодек, Bitrate/Битрейт and Rip type/Тип рипа;
- Latin S/L are drawn by a custom macOS-safe header renderer: S green, L red; positive seeder values are green and positive leecher values red;
- content-type selection is written to QSettings immediately; invalid/missing state restores All types;
- stale search-header geometry is discarded so compact widths survive upgrades.

Tests cover Rutor strict audio, RuTracker audio forum query, RuTracker forum-to-type mapping, and strict RuTracker audio without fake video fields.

Identity invariants remain unchanged: exact source page and exact info-hash are mandatory; no title-only provenance is accepted.


### Stage 17 — source-native content categories and deeper exact-source search

Commit message: `fix: classify by tracker category before file heuristics`.

Problem reproduced from macOS screenshots:
- a release could appear under All types but disappear under Audio even when its title/file set was clearly FLAC/MP3;
- typed Rutor search in Stage 16 made the exact .torrent download a mandatory precondition, so a missing/blocked .torrent link or an unrecognised description label could hide a valid exact release;
- Audio strict-completeness still depended on a small list of metadata labels;
- broad Rutor queries examined only the first result page, so valid releases could be omitted before type filtering.

Design change, following the pattern used by mature torrent indexers:
- tracker-native category is primary type evidence;
- exact .torrent file-list classification is a fallback when the source category is absent/unknown;
- title/technical-label heuristics are presentation/fallback evidence, not the primary admission gate.

Implemented for Rutor:
- parse the exact release page's native `Категория` value and map common categories to Video/Audio/Games/Software/Books/Pictures;
- store `sourceCategory` and `contentTypeEvidence=source-category`;
- typed search now fetches/verifies the exact release page first; it no longer rejects a result merely because the .torrent pre-probe failed;
- only an unknown/unmapped source category falls back to exact .torrent file classification, which is marked `contentTypeEvidence=torrent-files`;
- non-video typed searches use Rutor's coarse Other bucket (category 3), reducing the flood of movie/TV rows before filtering;
- search traverses up to three Rutor result pages and de-duplicates info-hashes across pages;
- searchUrl now supports explicit page/category while keeping the existing default URL unchanged;
- MP3/FLAC metadata parsing recognises real-world labels such as `Формат/Кодек`, `Формат аудио`, `Битрейт аудио`, `Аудио кодек`, etc.;
- generic Format/Codec labels are treated as audio only after the release is already known to be Audio, so video `Формат: MKV` is not misrepresented as an audio track;
- source-category or exact-file-list evidence is sufficient for strict type admission once the exact page and info-hash are verified; optional wording of technical fields no longer hides a valid release.

Implemented for public RuTracker.RU:
- mapped forum IDs now explicitly carry `contentTypeEvidence=source-category`;
- exact forum category evidence is sufficient for typed admission after exact viewtopic/info-hash verification;
- the same broader music metadata labels are parsed for display.

Important search-coverage note:
- Search Results still deliberately uses only exact-source Rutor + public RuTracker.RU adapters. The old local/P2P/DHT corpus is not admitted because it does not prove a concrete tracker release page.
- Stage 17 improves recall inside those two sources (especially Rutor) but does not claim global torrent coverage. Adding another tracker requires another exact-source adapter rather than silently reintroducing unverified generic hits.

Validation added:
- Rutor page/category URL construction;
- Rutor native categories map Music/Games/Software/Books/Pictures/Video correctly;
- MP3 release with `Формат/Кодек` + `Битрейт аудио` remains Audio and strict-complete;
- RuTracker forum-based Audio evidence is retained and accepts MP3-style labels.


### Stage 18 — public MegaPeer exact-source adapter

Commit message: `feat: add public MegaPeer exact-source search`.

Why this source:
- MegaPeer is public and does not require account credentials for its ordinary
  search/detail/torrent endpoints;
- a search row carries a concrete `/torrent/<id>/...` details URL and concrete
  `/download/<id>/...` .torrent URL, which lets Rats Search prove identity
  without title matching;
- release pages are rich enough for the existing Torrent Info contract
  (quality/video/audio/subtitles/full description), while the .torrent supplies
  the authoritative info-hash and immediate file list.

Implemented:
- Windows-1251 aware tracker text/query utilities without Qt5Compat/QTextCodec;
- `MegaPeerSource` parses public browse rows, exact detail/download IDs,
  size/S/L/date, and rich exact-page technical metadata;
- `MegaPeerSearchClient` downloads the paired .torrent first, computes the real
  info-hash with TorrentEngine, classifies from the real file list, then verifies
  the concrete detail page exposes the same download ID;
- selected Audio/Video/etc. filtering happens only after exact .torrent
  classification and source-page category enrichment, so MP3/FLAC are not
  excluded by movie-oriented metadata heuristics;
- strict Video admission still requires quality + video + at least one audio
  track; Audio requires concrete technical audio lines; other recognized types
  require a substantial exact-page description;
- Application owns/cancels the client; Search Results aggregates MegaPeer with
  Rutor + public RuTracker.RU and still deduplicates by info-hash;
- Torrent Details recognizes the verified MegaPeer URL and exposes only the exact
  release page;
- the Files panel can reuse MegaPeer's already verified direct .torrent URL
  instead of falling back to BEP 9.

Tests cover search URL construction, exact row coupling, rich MP3 metadata and
rejection when a detail page does not expose the download ID paired with the
search candidate.

Preserved invariant:
- no MegaPeer title-only result can enter Search Results; a candidate needs the
  concrete page, concrete downloadable .torrent, computed info-hash and strict
  exact-page release information.


### Stage 19 — public NNM-Club exact-source adapter

Commit message: `feat: add public NNM-Club exact-source search`.

Scope:
- integrate only the no-login NNM-Club surface; no credentials, cookies copied
  from a browser, or Keychain/account UI are introduced;
- public search explicitly requests `sds=4`, matching NNM's publicly
  downloadable/freeleech rows, so every admitted candidate can provide the
  actual .torrent without pretending a login-only release is usable.

Implemented:
- Windows-1251 POST body matching NNM's public `forum/tracker.php` search;
- exact search-row parser binds `viewtopic.php?t=<topic>` to
  `download.php?id=<download>`, captures forum/category, S/L, bytes and Unix
  publish date;
- source-native forum text is used as primary type evidence when recognizable;
  exact .torrent file classification is the fallback and fixes ambiguous/unknown
  forum labels;
- the public .torrent is parsed before admission to compute the real info-hash
  and populate the complete file tree immediately;
- exact topic verification requires the same topic id and the same download id
  originally paired in the search row;
- the first post is isolated when possible and supplies the full release
  description plus quality/video/audio/subtitle fields;
- strict Video and Audio completeness uses the same rich-info contract as the
  other exact sources; non-media recognized types require a substantial exact
  release post;
- Application/MainWindow aggregate NNM-Club alongside Rutor, RuTracker.RU and
  MegaPeer, with the existing info-hash de-duplication;
- Torrent Details accepts only the verified concrete NNM-Club topic URL;
- Files can re-use the verified direct NNM .torrent URL before BEP 9 fallback.

Tests cover public-search form flags/Windows-1251 encoding, exact row parsing,
forum Audio classification, detailed Video extraction and rejection of a topic
whose download id does not match the search candidate.

Combined public exact-source set after Stage 19:
- Rutor
- RuTracker.RU public
- MegaPeer public
- NNM-Club public/no-login subset

Semi-private/private trackers remain intentionally excluded until a separate
credential/Keychain design is explicitly requested.


### Stage 20 — compile correctness and public-source URL hardening

Commit message: `fix: harden public source adapters`.

Pre-CI static review of the combined Stage 18/19 implementation found one
generated C++ escaping error and an unnecessarily narrow MegaPeer URL shape.

Fixed:
- corrected the HTML `&quot;` entity replacement string in the shared
  Windows-1251/source parsing utility;
- use an explicit U+FFFD replacement-character test rather than depending on a
  Qt enum spelling;
- added explicit QHash/QTime includes used by the MegaPeer date parser;
- MegaPeer exact search/download URL parsing now accepts both
  `/torrent/<id>/<slug>` / `/download/<id>/<slug>` and the valid compact
  `/torrent/<id>` / `/download/<id>` forms;
- exact detail-page download-id verification uses the same tolerant URL shape;
- added regression coverage for compact MegaPeer URLs.

No search-source scope changed: the combined no-login build remains Rutor +
public RuTracker.RU + public MegaPeer + public/no-login NNM-Club.


### Stage 21 — complete QJsonArray type at strict-source boundaries

Commit message: `fix: include QJsonArray in public source parsers`.

The Stage 19 macOS compiler reached the new source adapters and exposed a
Qt-specific compile issue hidden by forward declarations: calling
`QJsonValue::toArray().isEmpty()` requires the full QJsonArray definition.

Fixed:
- explicitly include QJsonArray in MegaPeerSource and NnmClubSource;
- explicitly include QJsonArray in their parser tests, which also inspect
  `audioTracks`;
- no runtime/search behavior changed.

This is a compile-only follow-up to the combined public-source implementation.


### Stage 22 — typed-search latency without weakening exact verification

Commit message: `perf: prioritize likely typed results before verification`.

Observed on macOS after the four public sources were enabled:
- `All types` could show an NNM-Club book quickly, while selecting `Books`
  sometimes waited a very long time;
- MegaPeer/NNM-Club typed clients still verified candidates in tracker order,
  meaning every candidate cost an exact .torrent download + parse + detail-page
  request before the type filter could reject it;
- Rutor's coarse `Other` bucket similarly mixes books, music, games and
  software.

Implemented safely:
- added a shared content-type *priority* score based on already-known
  source-native type plus strong title hints (PDF/FB2/EPUB, MP3/FLAC, BDRip,
  etc.);
- the score never admits a result and never replaces the exact source/.torrent
  verification path: it only changes which candidate is checked first;
- an already recognized `contentTypeEvidence=source-category` mismatch may be
  rejected before network-heavy verification because the tracker itself has
  already classified that row;
- NNM-Club, MegaPeer and Rutor queues are stable-sorted by that score for typed
  searches;
- NNM-Club therefore prioritizes obvious PDF/FB2/EPUB book rows even when the
  forum title itself is too generic to map to Books;
- All-types ordering is unchanged (all scores are zero when no type is selected).

Validation:
- regression test proves PDF/FB2/EPUB title hints prioritize Books;
- regression test proves only explicit source-category mismatch is eligible for
  early rejection.

Identity/completeness invariants are unchanged.


### Stage 23 — optional exact-source marks in Search Results

Commit message: `feat: show optional source marks on search rows`.

User-visible goal:
- make it obvious which exact public tracker supplied each torrent without
  adding another wide table column;
- allow the marker to be disabled.

Implemented:
- SearchResultModel exposes `SourceProviderRole` directly from verified
  `Torrent::info.sourceProvider`;
- each row tooltip now starts with the full source name (Rutor, RuTracker.RU,
  MegaPeer or NNM-Club);
- TorrentItemDelegate can draw a compact 16 px source badge immediately before
  the existing content-type icon:
  - R = Rutor
  - RT = RuTracker.RU
  - M = MegaPeer
  - N = NNM-Club
- badges use distinct tracker-specific colours and stay legible on selected rows;
- the marks are local vector rendering rather than remote favicons. This avoids
  extra web requests/privacy leakage and broken icons when a tracker has no
  stable favicon (Rutor currently does not expose one reliably);
- Filters popup contains `Show source marks`, enabled by default;
- the setting is persisted immediately as `search/showSourceMarks`; toggling
  it only repaints the result table and never re-runs a search;
- the existing content-type icon remains visible after the provider mark, so
  source and media type stay separate concepts.

No exact-source search, filtering, de-duplication or download behavior changed.


### Stage 24 — typed-search optimization is prioritization-only

Commit message: `fix: keep typed search optimization recall-safe`.

Review of Stage 22 tightened the safety guarantee:
- a source-native category is strong evidence, but our cross-tracker category
  mapper is intentionally coarse (for example audiobook/library forum names can
  straddle Audio vs Books);
- therefore even an apparent category mismatch must not be discarded solely as
  a latency optimization.

Changed:
- removed all pre-verification type rejection introduced by Stage 22;
- source category and title hints now influence queue order only;
- every candidate remains eligible to proceed through the same exact page,
  exact .torrent/info-hash and final content-type verification as before;
- a recognized mismatch receives a low priority score, not a rejection.

Result: typed Books/Audio/etc. should surface likely matches much earlier while
the optimization cannot reduce recall compared with the pre-Stage-22 search.


### Stage 25 — authenticated RuTracker current-site adapter

Commit message: `feat: authenticate RuTracker exact-source search`.

Why this stage exists:
- runtime testing of Run #84 showed only Rutor and NNM-Club results;
- re-checking current RuTracker integrations (Jackett/Prowlarr and the qBittorrent
  search plugin) confirmed that RuTracker is semi-private: search/topic metadata
  require a logged-in session;
- the previous adapter also targeted the obsolete root-level
  `rutracker.ru/tracker.php` shape, expected a magnet in the listing row and
  decoded the page as UTF-8. Current RuTracker uses `/forum/tracker.php`,
  Windows-1251 and resolves the info-hash from the authenticated topic page.

Implemented:
- Settings > Indexer now contains RuTracker username/password fields;
- credentials are intentionally kept out of `rats.json` and therefore out of
  the REST config API; this build stores them in the app-local QSettings
  preferences and masks the password field in the UI;
- Application loads those credentials at startup and updates the live client
  immediately when Settings is saved;
- RuTracker is included in a search only when both credentials are configured;
- the client POSTs `login_username`, `login_password`, `login=Login` and
  `redirect=index.php` to `https://rutracker.org/forum/login.php`, keeps the
  resulting cookie jar for the app session and retries authentication once if a
  later search response falls back to the login page;
- authentication success accepts either a `bb_session` cookie or RuTracker's
  logged-in page marker; captcha/browser-challenge failures are surfaced as an
  explicit provider error instead of silently producing zero rows;
- search now uses `https://rutracker.org/forum/tracker.php`;
- current `trs-tr-<id>` listing rows are parsed using their concrete
  `viewtopic.php?t=<id>` and `dl.php?t=<id>` provenance, without pretending
  that a public listing already contains a trustworthy info-hash;
- the exact authenticated topic page supplies the magnet/info-hash and must
  still match the original topic id before its release description is trusted;
- RuTracker Windows-1251 is decoded through the shared source decoder;
- typed search is recall-safe: forum/category evidence only changes verification
  priority; it is not an early rejection rule;
- source classification prefers the live forum label over stale hard-coded
  RuTracker forum-id tables;
- detail concurrency is reduced to 2 authenticated topic requests.

Security note for hand-off:
- password persistence is currently QSettings, not macOS Keychain. This choice
  keeps this functional test stage cross-platform and low-risk for build/runtime
  regressions, but the value is locally recoverable from app preferences.
  Keychain migration can be a later hardening stage after runtime acceptance.

Validation added:
- parser tests now use current RuTracker table/topic/download markup;
- tests prove that the listing carries no fake hash, the exact topic magnet
  completes identity, audio forum text classifies correctly, and a mismatched
  topic URL is rejected.

Next stage:
- redesign MegaPeer to the verified detail-page-first flow: exact page -> magnet
  info-hash -> .torrent only as a fallback, with lower request pressure and
  visible network/provider failures.

### Stage 26 — MegaPeer detail-first verification and lower request pressure

Re-check before implementation:
- current Jackett/Prowlarr-style MegaPeer definitions still treat the tracker as
  public and use the listing's concrete /torrent/<id> and /download/<id> links;
- current independent MegaPeer clients resolve the magnet from the exact detail
  page, so downloading a .torrent before opening that page is unnecessary on
  the normal path;
- broad crawler implementations pace MegaPeer very aggressively because they
  sweep whole categories. An interactive one-query search should not inherit
  30/60/90-second crawler sleeps, but it also should not fan out four
  detail/.torrent pairs at once;
- therefore the correct interactive flow is: listing -> exact detail page ->
  magnet/info-hash; use the paired .torrent only if that exact page does not
  expose a magnet.

Commits in this stage:
- `4670b50ebd94f37fa08f4ff4190b77ae99686efe`
  `fix: prefer MegaPeer detail magnet over torrent download`
- `3c214be14628520247326bb786f5c42aadd5b4d9`
  `docs: describe MegaPeer detail-first identity flow`
- `73b2c65df48ad42f7880a3bcecb0c0aa9c5577f4`
  `refactor: make MegaPeer verification detail-first`
- `6437c14abdd16c386e0dd48526d6773556d588f7`
  `fix: reduce MegaPeer request pressure and surface failures`
- `25111bed00779fff50c662220d4f833c66a71071`
  `test: cover MegaPeer detail-first identity and fallback`
- `10e8f7f31f9a77d7814868527d55bfe664465c1c`
  `fix: classify MegaPeer from full exact detail page`
- `91e766408cd46ccbd272be983700876264a05b09`
  `test: keep MegaPeer category outside release description`

Implemented:
- MegaPeer now fetches the exact detail page first;
- a magnet on that page supplies the info-hash directly and becomes the primary
  identity proof;
- the search-row topic id and paired download id still have to match the exact
  detail page before its metadata is trusted;
- the .torrent download is retained strictly as a fallback when the verified
  detail page has no magnet;
- the already-fetched detail body is reused after the fallback .torrent is
  parsed, avoiding a second detail-page request;
- normal successful candidates therefore use one post-listing request instead
  of two;
- category classification is read from the full exact detail page because current
  MegaPeer keeps the category table outside the release-description block;
- per-source detail concurrency is reduced from 4 to 2;
- provider/network failures are counted and, if MegaPeer produces no verified
  result, surfaced through the existing source-status path instead of silently
  looking like an empty tracker;
- typed category hints remain prioritization-only and final admission still
  depends on exact-source verification.

Validation added:
- detail-page magnet can create the hash from an initially hash-less search row;
- a paired .torrent hash remains accepted as the fallback when the exact page
  has no magnet;
- a wrong download id is rejected even when a plausible magnet is present;
- a magnet that conflicts with a pre-resolved fallback hash is rejected.

Handoff note:
- Stage 25 RuTracker authentication is inherited from commit
  `757bfa254a597b31c0537e676395fcee3956cbd5`;
- Stage 26 is on branch `stage26-megapeer-gentle`;
- before presenting a user build, run the full GitHub Actions test/build matrix
  and only use the macOS ARM artifact whose BUILD-REVISION.txt matches the branch
  head.

### Stage 27 — live-source reliability: RuTracker mirror failover, MegaPeer direct-access hardening, clickable table sorting

Why this stage exists:
- Stage 26 runtime testing still showed only Rutor/NNM results;
- the user confirmed a working logged-in RuTracker session specifically on
  `https://rutracker.net/forum/index.php`, while our client was hard-coded to
  `rutracker.org`;
- current RuTracker integrations (Jackett and the qBittorrent RuTracker plugin)
  still list both `rutracker.org` and `rutracker.net` as official mirrors
  and retry mirrors when one is unreachable;
- current MegaPeer references still use `https://megapeer.vip/browse.php`,
  Windows-1251 and `tr.table_fon`; Jackett's March 2026 no-results bug was the
  obsolete wildcard query, which our adapter does not use;
- current jacred-go notes that MegaPeer may now answer plain HTTP again but can
  still return 200 error/captcha bodies, so a valid-page marker and explicit
  anti-bot detection are required;
- the custom SearchHeaderView replaced Qt's stock table header. A freshly
  constructed QHeaderView has non-clickable sections by default, so the visible
  columns could not be sorted by mouse even though model sorting existed.

Commits in this stage:
- `d46092ec955cc8c5c2bac98a17884b964a17c896`
  `fix: add RuTracker official-mirror failover`
- `cb6f3e0326f82b875233e12ce12afbb0ecc5e40f`
  `fix: prefer reachable RuTracker mirror and expose challenges`
- `fa810dde43cd6848a8169d51aa6fd67a112a9939`
  `fix: harden MegaPeer direct requests and detect anti-bot pages`
- `43987bf0336681067c951fa6012b94124ad95986`
  `fix: stop rejecting exact RuTracker rows for parser sparsity`
- `2bdaf07730db62a076dc04679d7beb040c731352`
  `fix: stop hiding exact MegaPeer rows with sparse parsed metadata`
- `68ae482e52a6875f98c6b684761b07392ba1b0ef`
  `fix: make all search result headers sortable by click`
- `b37c1904568605bd6dbcb94d91f151327c1156ed`
  `test: keep sparse exact RuTracker releases visible`
- `a6de157e0af0fa98b837e8ea83a3b87e0d33bf4f`
  `test: keep sparse exact MegaPeer releases visible`

Implemented:
- RuTracker now prefers `rutracker.net` (the mirror verified by the user) and
  automatically falls back to `rutracker.org`; each mirror uses its own fresh
  cookie jar and the same current `/forum/login.php` + `/forum/tracker.php`
  paths;
- current upstream reality (rechecked 2026-09-27): the maintained
  qBittorrent RuTracker plugin warns that RuTracker enabled newer Cloudflare
  protection in July 2026 which a plain HTTP client cannot reliably bypass
  without a JS/browser engine. Mirror failover therefore improves ordinary
  reachability but is not claimed to defeat a managed Cloudflare challenge;
  this build detects that condition and reports it instead of misreporting an
  empty result set;
- login/search responses that are redirected back to login, challenged by
  captcha/anti-bot pages, or return unexpected non-tracker HTML now fail over to
  the next official mirror instead of silently returning zero rows;
- successful search/detail URLs stay on the mirror actually used, while exact
  topic-id and magnet/hash verification remains unchanged;
- MegaPeer requests now use a normal current Chrome-like UA plus browser-like
  navigation headers instead of the conspicuous `RatsSearch/2` UA;
- MegaPeer validates the current browse-page marker (`id="logo"`) and detects
  Cloudflare/anti-bot bodies even when the HTTP status is 200, surfacing a
  provider error rather than pretending the tracker is empty;
- exact-source completeness no longer requires our optional codec/audio field
  parser to recognize every label. RuTracker/MegaPeer rows are admitted when
  exact source identity is proven and a concrete release description is present;
  parsed quality/video/audio/subtitle fields remain enrichment only;
- Name, Size, Seeders, Leechers and Date headers are explicitly clickable and
  continue using SearchResultModel's local typed sort implementation.

Validation:
- new regression tests prove sparse-but-exact RuTracker and MegaPeer releases are
  retained instead of being discarded solely because optional technical labels
  were not parsed;
- full CI/build must pass before handing out the Stage 27 macOS ARM DMG.

Handoff:
- branch: `stage27-source-reliability`;
- parent accepted build: Stage 26 head
  `5cc64dd910a2a40d0a4bef7ae6fe506e03171099`;
- do not merge until the user confirms RuTracker/MegaPeer presence and clickable
  sorting on macOS Sequoia.

### Stage 28 — force deterministic mouse sorting for the custom macOS search header

Runtime finding:
- Stage 27 correctly set `sectionsClickable=true`, but the user confirmed on
  macOS Sequoia that clicking Name / Size / S / L / Date still did not reorder
  the result table;
- the remaining weak point was relying on QTableView/QHeaderView's implicit
  click-to-sort behavior after replacing the stock header with our custom
  `SearchHeaderView`.

Commit:
- `406f5a1fb8ffb02663d06fa9376c482cfbbefa4c`
  `fix: force deterministic header click sorting on macOS`

Implemented:
- `SearchHeaderView` now tracks a real left-button press/release on one header
  section and calculates the requested sort order itself;
- a short click on Name starts A-Z; a short click on Size, Seeders, Leechers or
  Date starts descending; repeated clicks toggle the direction;
- the custom header calls an explicit sort handler wired directly to
  `SearchResultModel::sort(column, order)`, so sorting no longer depends on
  Qt/macOS implicit header wiring;
- normal QHeaderView event handling is still called first, so resize behavior and
  native drawing remain intact;
- small drag detection prevents a column-edge resize gesture from being treated
  as a sort click;
- the visible sort indicator is updated to the exact order applied to the model.

Handoff:
- branch: `stage28-clickable-sorting`;
- parent: Stage 27 head
  `c631eb849b04024ba442694aa52242a71055dfc8`;
- after CI passes, validate on macOS Sequoia by clicking each of the five visible
  result headers twice and confirming both directions.

### Stage 29 — persist RuTracker authenticated sessions across launches; Kinozal feasibility audit

Runtime problem fixed:
- Stage 27/28 kept RuTracker cookies only in the process-local QNetworkCookieJar;
- therefore a successful authenticated session could work until Rats Search was
  replaced/restarted, then the next build had to perform a fresh login and could
  hit RuTracker's intermittent Cloudflare/login wall even with unchanged valid
  credentials;
- Stage 28 did inherit Stage 27's RuTracker source/auth code byte-for-byte; the
  regression observed after upgrading was session lifetime, not a lost source
  integration.

Commits in this stage:
- `afb6256c2508d669084e8f3fd57deef779012354`
  `feat: persist RuTracker authenticated session across launches`
- `89a49575fc5bd18f56460ca21b5f5842e0aea6d9`
  `fix: restore RuTracker cookies before re-authenticating`

Implemented:
- RuTracker now persists the authenticated cookie jar per official mirror in the
  same local RatsSearch QSettings store that already holds its account settings;
- all cookies issued for the successful session are serialized, not only
  `bb_session`, so any additional server/Cloudflare cookies received by this
  client survive an app update/restart as well;
- sessions are tagged with the configured username and are restored only for
  that same account;
- startup and mirror failover first try a restored session and only POST the
  username/password when no saved session is usable;
- a restored session is never blindly trusted: tracker.php must still return the
  real authenticated torrent table; redirect/login-form responses clear that
  mirror's stale persisted session and trigger one clean re-login;
- successful login and successful authenticated search both refresh the stored
  cookie snapshot so server-side cookie rotations survive the next restart;
- changing credentials deliberately clears all saved RuTracker sessions;
- Cloudflare/captcha challenges remain explicit provider errors. Persistence
  avoids unnecessary re-login but does not claim to solve a fresh managed JS
  challenge that the Qt HTTP stack never cleared.

Security note:
- persisted cookies are authentication credentials. They currently live in
  QSettings, consistent with the existing RuTracker password storage from Stage
  25. A later hardening pass can move both password and session material to
  macOS Keychain without changing the search protocol.

Kinozal feasibility audit (2026-09-27):
- current maintained Jackett definitions classify Kinozal as semi-private and
  use `https://kinozal.me/` and `https://kinozal.guru/`; `.tv` is legacy /
  temporarily unavailable;
- current login is still POST `takelogin.php` with `username` + `password`;
  successful sessions use `uid` + `pass` cookies;
- browse/search is still `browse.php` in Windows-1251 and exposes exact
  `details.php?id=<id>` links, title, size, seeders, leechers and date;
- exact `get_srv_details.php?action=2&id=<id>` returns the info-hash and file
  list, so exact release identity can be bound without relying on a title-only
  search result;
- Kinozal's own current video rules require concrete technical fields such as
  Quality, Video, Audio, translation/language and subtitles on release pages,
  so a concrete `details.php?id=<id>` parser can meet Rats Search's rich
  exact-source information standard rather than degrading it;
- however current independent tracker code measured Kinozal behind a Cloudflare
  managed challenge on 2026-09-26: `takelogin.php`, `login.php` and
  `browse.php` can answer 403/challenge. Reliable automation now needs a
  browser-earned clearance/session or equivalent browser-capable fallback;
- therefore Kinozal is technically a good data source but should NOT be added
  yet as a plain Qt-network provider. Doing so would recreate the silent-zero /
  intermittent-login failure we just fixed for RuTracker;
- safe integration plan: first add a reusable authenticated-session/import path
  capable of using a browser-cleared Kinozal `uid/pass` (+ clearance cookies
  when required), then implement listing -> exact details.php -> exact
  get_srv_details hash/files -> rich detail parser. Only emit a Kinozal row
  after those exact-source checks pass.

Handoff:
- branch: `stage29-rutracker-session-persistence`;
- parent: Stage 28 head
  `ae7b03608edd92c9ebb7dcf018d96944a5558044`;
- do not merge until macOS ARM CI passes and a restart test confirms that a
  previously successful RuTracker session continues working without a new login.


### Stage 30 — RuTracker browser-assisted login and same-browser verification

Commits:\n- `8987bcb2521128c01b2b15860c810c22457fa6af` — `feat: recover RuTracker with a native browser session on macOS`;\n- `docs: record Stage 30 implementation commit and trigger CI` — this documentation follow-up.

Parent: Stage 29 HEAD `97f2c6f3a4866d35e1909162a5a24a5adc64ba65`.
Branch: `stage30-rutracker-browser-session` (do not merge until the user's
macOS Sequoia runtime test succeeds).

User observation: normal browser login and direct torrent downloads work, but
Stage 29's non-browser HTTP login/search can encounter Cloudflare and CAPTCHA.

Implemented:
- preserve Stage 29's fast Qt path and its persisted sessions when the tracker
  accepts them; on failure of both official mirrors, macOS uses a native WebKit
  browser window for a user-assisted login/captcha/challenge;
- the WebKit default website data store persists its session across restarts;
- search and topic requests after browser login use the SAME WKWebView transport,
  rather than copying `cf_clearance` to a Qt HTTP client with a different TLS
  fingerprint; when browser mode is active, topics are serialized because one
  WKWebView can navigate only one page at a time;
- search rows still pass exact topic/magnet/info-hash validation before display;
- browser login cancellation, navigation failure and a three-minute timeout
  return an explicit provider error; no background service is installed;
- WebKit is compiled only for macOS; Windows/Linux retain Stage 29's path.

Limitations to validate on the actual Mac: WebKit must pass the tracker site's
current challenge; a working Chrome session is separate from WebKit's new
persistent store, so the first browser fallback can require one interactive
login. The stage is a test build until the user confirms login, real results,
restart persistence, sorting and downloads. If the site's challenge rejects
WebKit, do not claim success or merge; inspect the provider error and adapt.

Installation: quit Stage 29, drag Stage 30 over the app in Applications and
choose Replace. Keep preferences; no clean installation or reimport.

Stage 30 CI/package follow-up (commit message:
`ci: retry macOS DMG creation and provide signed ZIP fallback`):
- Run #36345412421 compiled macOS ARM successfully, passed tests, validated
  the arm64 bundle and passed code-signing verification; `hdiutil create`
  then failed with `Resource busy` on the shared macOS runner;
- retry transient `hdiutil` errors three times; if they persist, package the
  same signed app with macOS `ditto` and upload ZIP instead of losing a
  successfully built test binary;
- the fallback changes packaging only, not RuTracker behavior or source logic.

Stage 30 runtime hotfix (commit message:
`fix: retain RuTracker browser window and report blank pages`):
- user signed in through the Stage 30 browser window, then saw a completely
  white window with no RuTracker results; after closing it and starting another
  search, Rats Search crashed;
- supplied macOS crash report: `EXC_BAD_ACCESS` on main thread, `objc_msgSend`
  from `RuTrackerRuSearchClient::cancel()` during a subsequent `search()`;
  the Objective-C selector at the crash was `orderOut:`, so the reusable native
  window had become unsafe after close;
- the native NSWindow is now explicitly retained after close, and its red close
  button hides/cancels the pending browser login without destroying the reusable
  window or WebKit view;
- browser URLs now use Qt's fully encoded bytes when handed to NSURL, preserving
  query escapes across the Qt-to-WebKit boundary;
- if WebKit completes a page that is neither the requested real torrent page nor
  an interactive login/challenge page, hide the window and report the final URL
  as an explicit provider error instead of leaving a featureless white window;
- preserve the signed app's settings and persistent WebKit data store on upgrade.

Runtime acceptance still required on macOS: sign in if asked, search for a known
RuTracker release, confirm an `RT` result; run a second search after closing
an unsuccessful browser window; quit/relaunch and search again. Keep PR draft.

Stage 30 detail navigation hotfix (commit message:
`fix: preserve RuTracker detail URLs in browser callbacks`):
- user successfully signed in and the browser window closed, but repeated
  searches produced no RuTracker results;
- in the browser detail branch, `browser_->get(job.url, [job = std::move(job)]...`)
  used the same object in two function arguments whose evaluation order is
  unspecified; Clang can move `job` before evaluating `job.url`, so WebKit
  receives an empty detail URL and no verified releases reach the results;
- copy the detail URL before moving `job` into the callback;
- report when tracker search rows are found but every detail is rejected, so a
  zero-result browser search has a specific diagnostic. Runtime acceptance on
  the user's Mac is still necessary; keep the PR draft.

### Stage 31 — search-row identity after browser login

Commit message: `fix: distinguish RuTracker row numbers from topic identities`.

Evidence from the user's uploaded rats-search.log (2026-09-28 local time):
- 00:31:23 and 00:32:04: searches for two common Russian movie titles end
  with accepted=0, rejected=0, "no exact torrent rows";
- this localizes the observed failure BEFORE detail navigation. Stage 30's
  detail-URL move fix was not sufficient and must not be called the root cause;
- current logs do not contain the actual search DOM, so they cannot distinguish
  an empty website result from rows discarded by the parser.

Reproduced parser defect:
- parser required `tr#trs-tr-N` to have N equal to the topic ID in the link;
- the independent johnlepikhin/rutracker-api search_basic.html fixture at
  f1586eb7cb4a895c22903c3ce6972c071cf61324 uses row numbers 1/2 for topics
  5956108/42. The previous admission logic returns zero, the fix returns two;
- qBittorrent's nbusseneau RuTracker plugin extracts identity from data-topic_id,
  rather than requiring it to equal the HTML row ID;
- identify candidates by their exact official topic URL, cross-check explicit
  data-topic_id on the title link and row, and still verify the topic's magnet
  and description before emitting any result. DOM row IDs are presentation only;
- regression test includes sequential row numbers plus conflicting explicit
  topic IDs that must remain rejected.

Browser diagnostics/readiness:
- use DOM selectors for the actual tracker table, login marker and magnet;
  text mentions in scripts/styles no longer count as loaded page elements;
- log row/link counts and first three row/topic ID pairs (no credentials or
  cookies), plus parsed candidate count, to distinguish transport and parsing;
- JavaScript inspection errors are explicitly reported.

Runtime limitation: the supplied log confirms the failing stage, not the
specific HTML shape on the user's Mac. This fixes a reproduced defect and adds
the missing evidence if another case remains. Keep PR #15 draft until accepted.

### Stage 32 — current RuTracker search request after successful browser authentication

Evidence from the user's Stage 31 runtime log (2026-09-28):
- query `багровый прилив`:
  - WebKit reached `/forum/tracker.php`;
  - `table=true`;
  - `rows=1`;
  - `topicLinks=0`;
  - parsed candidates = 0;
- query `терминатор` produced the same shape:
  - `table=true`;
  - `rows=1`;
  - `topicLinks=0`;
  - parsed candidates = 0;
- therefore authentication/Cloudflare recovery succeeds and the failure occurs
  before parsing/detail verification: RuTracker itself returns an empty-result
  tracker table.

Re-check against current maintained integrations:
- the maintained qBittorrent RuTracker plugin performs search as
  `tracker.php?nm=<query>` and does not submit the old full tracker-form flag
  set;
- current Prowlarr likewise sends `nm=<query>` plus an optional explicit forum
  category list, not the fork's legacy `f[]=-1` / `prev_*` / `df/da/ds` /
  `tm/sns/srg` bundle;
- the fork's old URL builder was therefore over-specifying stale form state.
  On the user's live authenticated tracker page it yielded a valid table shell
  with one placeholder row and zero torrent links even for common titles.

Commits in this stage:
- `279f3a6797f210cd7b31ffb7b4be1c473325e93b`
  `fix: use current minimal RuTracker search query`
- `a20056dbcc17b854c8d8dda6019d62ae0966e2bb`
  `test: lock RuTracker search to current nm-only request`

Implemented:
- RuTracker search URL is now intentionally minimal:
  `/forum/tracker.php?nm=<query>`;
- removed all legacy search-form flags from the request path;
- source category extraction remains row-based;
- typed filtering remains client-side;
- visible result sorting remains local in SearchResultModel, so removing server
  sort flags does not remove the user's Name/Size/S/L/Date sorting;
- exact topic URL -> exact magnet/info-hash -> release description verification
  remains unchanged.

Regression test:
- asserts `nm` is the only query item;
- explicitly rejects accidental reintroduction of `f[]`, `prev_df`, `o`,
  and `s`.

Handoff:
- branch: `stage32-rutracker-current-query`;
- parent: Stage 31 / PR #15 head
  `83b6c99d7d7afa2206a4dcbf866becb11d1a36eb`;
- runtime acceptance requirement: on macOS Sequoia, authenticate if needed and
  search a common title such as `терминатор`. Expected browser diagnostics are
  now `rows > 1` and `topicLinks > 0`, followed by parsed candidates > 0 and
  at least one RT result after exact detail verification.

### Stage 33 — one RuTracker session on macOS: WebKit-only auth/search/details

Why this stage exists:
- Stage 32 still produced no visible RT rows on the user's Mac;
- the settings UI still showed a saved RuTracker username/password even though
  successful authorization was happening in the embedded WebKit browser;
- inspection confirmed two independent auth stacks were coexisting on macOS:
  QNetworkAccessManager + QSettings credentials/cookies, and a persistent
  WKWebView data store used as a Cloudflare/browser fallback;
- this made it unclear which session search was actually using and allowed a
  successful browser login to coexist with an unrelated HTTP credential state;
- replacing or "clean installing" the .app did not clear those fields because
  QSettings lives outside the application bundle.

Architecture chosen:
- on macOS, RuTracker now has exactly one transport/session: persistent WebKit;
- the same WKWebsiteDataStore owns login, Cloudflare clearance, tracker search
  and exact viewtopic pages;
- no RuTracker username/password is loaded, saved or replayed through Qt HTTP on
  macOS;
- non-macOS keeps the existing HTTP credential path for now.

Commits in this stage:
- `4a0edc4a4d775b7b7ed4dd413375c96e8841bb78`
  `refactor: make WebKit own the macOS RuTracker session`
- `a2a2d03eb0ffdb58b3e401b2a2a8bf991b3798dc`
  `feat: add persistent browser authorize and re-login flow`
- `ec8b5668c10e78f3e81bd60bcef48ab0e98d9223`
  `api: expose browser-only RuTracker re-login on macOS`
- `566b218f5f7cc2c5d40bb29eecd86e52490ccc0f`
  `refactor: make macOS RuTracker search browser-only`
- `f77b300fd9ab459db72d5e8fcc293f27b72239f8`
  `cleanup: stop loading RuTracker passwords on macOS`
- `c7c8dadfea959e3a95f7c9f085221443b2f47068`
  `ui: model RuTracker settings as browser session on macOS`
- `fc54c62ca14f23eebc975fec06a0424409ede992`
  `ui: replace macOS RuTracker password fields with browser session controls`
- `bc49471d8fad1e065f366c7f9a08ca8c941cf9fc`
  `fix: terminate browser mirror failover and guard re-login generation`

Implemented:
- the macOS client constructs WebKit immediately and enters browser mode
  permanently for RuTracker search;
- search no longer requires `rutracker/username` or `rutracker/password`;
- old plaintext RuTracker username/password preferences and old Qt-cookie
  snapshots are deleted on first launch of this stage;
- browser search that encounters a real login page keeps that same WebKit window
  interactive, and retries the tracker search after the authenticated DOM marker
  appears;
- exact topic verification also stays in WebKit, so no browser cookie or
  Cloudflare clearance is copied into QNetworkAccessManager;
- official mirror failover remains available inside WebKit and now terminates
  cleanly after all mirrors fail instead of looping;
- Settings > Indexer on macOS no longer displays username/password fields;
- it now explains the persistent browser-session model, shows session messages,
  and exposes one deterministic `Authorize / Re-login` action;
- that action removes only RuTracker website data from the app's WebKit store,
  opens a fresh embedded login page, and reports success when the logged-in DOM
  marker appears;
- asynchronous re-login callbacks are generation-guarded so a later search or
  cancellation cannot revive a stale authorization flow.

Important persistence behavior:
- WebKit uses `WKWebsiteDataStore.defaultDataStore`, so a successful embedded
  RuTracker session survives normal application replacement/update;
- deleting Rats Search.app alone does not clear the WebKit store or generic
  QSettings data; Stage 33 explicitly migrates away the obsolete RuTracker
  credential keys itself.

Search/data invariants preserved:
- Stage 32's current minimal `tracker.php?nm=<query>` request remains;
- source row identity still comes from exact topic URL / data-topic_id, never
  presentation row numbers;
- every emitted RT result still requires exact topic-page magnet/info-hash and
  release-specific metadata;
- result-table sorting and all Rutor/NNM/MegaPeer code are untouched.

Runtime acceptance:
1. install Stage 33 over the previous build;
2. Settings > Indexer must show `RuTracker browser session`, not username and
   password fields;
3. click `Authorize / Re-login` once and complete login in the embedded window;
4. search `терминатор`;
5. expected diagnostics: real tracker table with topicLinks > 0, parsed
   candidates > 0, then at least one RT result after exact detail verification;
6. quit and relaunch; repeat the search without re-entering credentials. The
   WebKit session should persist.

Handoff:
- branch: `stage33-rutracker-browser-only`;
- parent: Stage 32 head
  `911d5c985218ff04b963bf7beab5150051c12f2a`;
- keep the PR draft until macOS runtime acceptance.

### Stage 34 — preserve full RuTracker release titles with nested markup

Runtime finding:
- Stage 33 successfully restored visible RuTracker results on macOS after browser
  authorization;
- however RT rows showed truncated names such as `Багровый`, `В`,
  `Полицейская`, etc. while the same releases should have full topic titles;
- the search-row title was already parsed correctly; truncation happened later
  in `applyDetailPage()`, which overwrote that title from `#topic-title`;
- the old regular expression terminated on the first closing child tag
  (`</[^>]+>`), so nested markup such as
  `<b>Багровый</b> прилив / Crimson Tide ...` became only `Багровый`.

Commits in this stage:
- `37878df90d8daec54af88c8481d502be6ad8610b`
  `fix: preserve full RuTracker titles with nested markup`
- `ddaa0ae36e2a364dc09cd07af197d9f29af40277`
  `test: reproduce truncated RuTracker nested topic titles`
- `b8ef1513973ac2354654dd573ae6f2b617f154c5`
  `fix: heal stored names from authoritative source titles`
- `a6281bc8ffb82771dcc96db82d2c7a0e1afc6327`
  `test: repair previously stored truncated exact-source names`

Implemented:
- added a dedicated DOM-like `elementTextById()` extractor that finds the
  opening tag carrying `id="topic-title"` and closes only on that same outer
  tag type;
- nested `<b>`, `<span>` and similar formatting inside the title are removed
  only after the whole element body has been captured;
- a detail-page title is no longer allowed to replace an already complete
  search-row title with a suspiciously shorter value;
- authoritative exact-source reinsertion now also refreshes the stored torrent
  name, so RT rows already poisoned by an older truncated title heal on the next
  search instead of remaining truncated in the local index;
- exact source URL, topic ID, magnet/info-hash verification and all metadata
  enrichment remain unchanged.

Regression coverage:
- reproduces `<a id="topic-title"><b>Багровый</b> прилив / Crimson Tide ...`;
- asserts that the final torrent name is the complete
  `Багровый прилив / Crimson Tide (1995) BDRip 1080p`, never just
  `Багровый`.
- the Manticore integration test starts with a previously stored exact-source
  row named only `Багровый`, reinserts the verified full title, and asserts
  that the persisted/searchable row is repaired.

Handoff:
- branch: `stage34-rutracker-full-titles`;
- parent: Stage 33 head
  `d9967020c04c1eda57bbda40f9984780fa696804`;
- runtime acceptance: RuTracker remains visible after browser auth and every RT
  row shows the full release title instead of the first styled word.

### Stage 35 — restore exact RuTracker release button for current mirrors

Runtime finding:
- Stage 34 shows verified RT rows correctly, but the details panel does not show
  the same `Open exact <source> release` button that Rutor rows have;
- the RuTracker result itself already contains an exact verified `sourceUrl`
  and `sourceTopicId`;
- the button was hidden by a stale UI-only validator that accepted only the host
  `rutracker.ru`;
- the current macOS browser-only integration actually stores exact topic URLs on
  the active official mirrors `rutracker.net`, `rutracker.org` or
  `rutracker.nl`, so `verifiedExactSourceUrl()` returned invalid and the UI
  incorrectly treated an exact RT result as non-exact.

Commit:
- this Stage 35 commit atomically updates source validation, UI, regression tests
  and this handoff section.

Implemented:
- added `RuTrackerRuSource::isExactTopicUrl()` as the single validator for a
  concrete RuTracker topic URL;
- accepts current official mirrors `.net`, `.org`, `.nl` and legacy
  `.ru` snapshots for backward compatibility;
- requires the exact `/forum/viewtopic.php?t=<id>` shape;
- when `sourceTopicId` is available, the URL's topic ID must match it exactly;
- the details panel now uses that canonical validator instead of its obsolete
  `rutracker.ru`-only check;
- verified RT rows therefore render the same exact-source section and button as
  Rutor/MegaPeer/NNM-Club;
- button text is `Open exact RuTracker release` and opens the concrete
  `sourceUrl`, never a title search or generic tracker page;
- no search/auth/parser logic changed.

Regression coverage:
- verifies exact topic URLs for rutracker.net / .org / .nl and legacy .ru;
- rejects wrong topic IDs, unrelated hosts, tracker.php and URLs without a
  concrete topic id.

Handoff:
- branch: `stage35-rutracker-exact-release-link`;
- parent: Stage 34 head
  `c8068a491304ab28110d9754bdfcbeaa18db97e2`;
- runtime acceptance: select an RT row and confirm the details panel shows
  `Open exact RuTracker release`; clicking it must open that row's exact
  viewtopic.php?t=<sourceTopicId> page.

### Stage 36 — Kinozal exact-source integration with persistent WebKit auth

Pre-implementation availability re-check (2026-09-28):
- maintained Jackett definitions list current Kinozal mirrors
  `https://kinozal.me/` and `https://kinozal.guru/`; `.tv` is legacy;
- Kinozal remains Windows-1251 and semi-private;
- current login endpoint is still `takelogin.php`, but independent maintained
  integrations measured Cloudflare Managed Challenge / HTTP 403 on login and
  browse endpoints, so replaying credentials through plain Qt HTTP would repeat
  the old RuTracker failure mode;
- current live-capture fixtures confirm browse rows still expose one concrete
  `details.php?id=<id>`, full release title, size, seeders, leechers, date and
  category;
- current Jackett magnet mode and independent Kinozal clients still resolve the
  exact info-hash through
  `get_srv_details.php?id=<id>&action=2`; that response also exposes the
  concrete file list.

Architecture:
- macOS Kinozal follows the proven Stage 33 RuTracker design: one persistent
  WKWebsiteDataStore owns Cloudflare clearance, login, search, concrete detail
  pages and server-details identity requests;
- mirror order is `.me` first, `.guru` fallback; `.tv` is deliberately
  excluded;
- no Kinozal username/password is stored by Rats Search on macOS;
- non-macOS builds keep Kinozal disabled for now rather than pretending a
  fragile HTTP login works.

Commit:
- this Stage 36 commit atomically adds the parser, WebKit transport, async search
  client, Application/UI wiring, tests and this handoff.

Exact-source flow:
1. browser search `browse.php?s=<query>...`;
2. preserve the complete listing anchor text verbatim as the displayed release
   title;
3. open that row's exact `details.php?id=<id>` in the same browser session;
4. capture release-specific page text and technical fields without shortening
   the listing title;
5. open exact `get_srv_details.php?id=<same id>&action=2`;
6. require a valid 40-hex info-hash; import the endpoint's concrete file list;
7. only then set `sourceVerified=true` and allow the row into Search Results.

Implemented:
- new `KinozalSource` parser:
  - current mirror URL/search builder with Windows-1251 query encoding;
  - full-title listing parser;
  - size/S/L/date/category parsing;
  - exact `details.php?id` validation;
  - detailed exact-page metadata capture;
  - exact info-hash and file-list parsing from `get_srv_details.php`;
- new persistent macOS `KinozalBrowser`:
  - hidden search/detail/server-detail navigation;
  - interactive Cloudflare/login recovery in the same WKWebView;
  - explicit session clearing that removes Kinozal records only;
- new `KinozalSearchClient`:
  - `.me -> .guru` search mirror failover;
  - sequential candidate verification because one WebView owns one navigation;
  - typed-search prioritization only; exact admission remains source-proven;
  - explicit provider errors instead of silent zero on browser failures;
- Application owns/cancels/exposes Kinozal;
- Search Results includes Kinozal as an exact source on macOS;
- new `KZ` source badge and Kinozal tooltip/display name;
- details panel validates exact Kinozal IDs and automatically renders
  `Open exact Kinozal release` for verified rows;
- Settings > Indexer gains a separate
  `Kinozal browser session -> Authorize / Re-login` block.

Title/data guarantees:
- the displayed title is the full listing anchor text; no media-manager title
  normalization is applied;
- the detail page never replaces a complete listing title with a shorter
  nested-markup fragment;
- every emitted Kinozal result has one exact details URL and an info-hash
  resolved for that same numeric Kinozal ID;
- detailed description comes from that concrete release page, never from a
  generic movie lookup.

Regression coverage:
- current mirror search URL and Windows-1251 path;
- full long/nested Kinozal listing title remains complete;
- size, S/L, date, category and exact ID parsing;
- detail-page enrichment preserves the full title;
- server-details response resolves hash + file list + piece size;
- exact URL validation accepts only current `.me/.guru` and matching IDs,
  explicitly rejecting legacy `.tv` and unrelated hosts.

Handoff:
- branch: `stage36-kinozal-browser-source`;
- parent: Stage 35 head
  `1109f6aeee041ae5f64b9ed40fe83dc28509dee8`;
- runtime acceptance on macOS Sequoia:
  1. Settings > Indexer shows Kinozal browser session;
  2. authorize once;
  3. search a common movie;
  4. expect `KZ` rows with full titles;
  5. select a KZ row: rich exact-source description + file list + info hash;
  6. `Open exact Kinozal release` opens that row's exact
     `details.php?id=<sourceTopicId>`;
  7. quit/relaunch and repeat without re-entering credentials.

### Stage 37 — fix authenticated Kinozal searches that silently return zero KZ rows

Runtime finding from the Stage 36 macOS ARM build:
- Kinozal browser authorization succeeds and the Settings session control works;
- repeated searches such as `терминатор` still show Rutor/RuTracker/NNM results
  but no `KZ` rows;
- repeating `Authorize / Re-login` does not change the outcome.

Current-site re-check before changing code (2026-09-28):
- Kinozal itself reports that legacy `kinozal.tv` is temporarily unavailable,
  while current mirrors `kinozal.me` and `kinozal.guru` are operating;
- current browse listings on the working mirrors contain real release rows,
  including Terminator releases, so the observed zero is not explained by a
  global Kinozal outage;
- current Jackett definitions still use the same `browse.php` row layout and
  current mirrors;
- Kinozal's own/current helper scripts obtain
  `get_srv_details.php?id=<id>&action=2` as a same-origin AJAX request from a
  normal tracker page;
- an actively maintained independent parser documented a production failure mode
  on 2026-09-27 where a stale/guest/Cloudflare response looked like a successful
  empty Kinozal run unless the client proved the listing/session explicitly.

Root causes in Stage 36:
1. the exact info-hash endpoint was opened as a new top-level WKWebView
   navigation. That is not Kinozal's normal request shape and loses the
   same-page AJAX/Referer context;
2. if that response contained no exact info-hash, Stage 36 counted the candidate
   only as an ordinary reject, so all KZ rows could disappear without a visible
   provider error;
3. browse-page success was proved only by host/path/title, allowing a branded
   guest/error shell to be mistaken for a legitimate empty search;
4. a successful authorization/search on the fallback `.guru` mirror was not
   remembered; the next search restarted from `.me`.

Commit:
- this Stage 37 commit fixes the transport and diagnostics atomically and updates
  this handoff.

Implemented:
- `KinozalBrowser::fetchText()` now executes an in-page same-origin
  `fetch()` through WebKit's async-JavaScript API while the WebView remains on
  the exact `details.php?id=<id>` page;
- the request carries the persistent WebKit cookies/Cloudflare state and an XHR
  marker, matching the site's normal AJAX context;
- `get_srv_details.php?id=<same id>&action=2` is no longer loaded as a
  top-level page;
- a server-details response without `Инфо хеш` / `Info hash` is now an
  explicit provider failure rather than a silent rejected row;
- exact-detail parser/identity failures are also counted as provider failures, so
  an all-rejected Kinozal run becomes visible in the final source status;
- browse-page acceptance now requires a current official mirror plus a branded
  page and positive listing/session evidence (logged-in marker, concrete
  `details.php?id=` rows, or an explicit zero-results message);
- if a page visibly contains concrete release links but the parser produces zero
  candidates, the client names that parser failure and tries the fallback mirror;
- the mirror that actually succeeds is remembered for subsequent searches;
- exact-source identity remains unchanged: a KZ row is emitted only after the
  exact detail page and the exact same numeric ID's 40-hex info-hash are proved.

Why repeated re-login did not help Stage 36:
- authorization and search-result admission are separate steps;
- the user's session could be valid while every candidate was later discarded at
  the incorrectly transported server-details step.

Handoff:
- branch: `stage37-kinozal-ajax-details`;
- parent: Stage 36 head
  `3153145836f12a13eecfba04da92be2bc7fc13a4`;
- runtime acceptance on MacBook Air M1 / macOS Sequoia:
  1. authorize Kinozal once if necessary;
  2. search `терминатор`;
  3. at least one valid Kinozal release should appear with a `KZ` badge when
     Kinozal returns matching releases;
  4. selecting it must show the exact release data and file list;
  5. if the site/Cloudflare blocks a stage, the bottom source status must name
     the Kinozal failure instead of silently showing zero KZ rows;
  6. repeat the search and relaunch the app to verify the working mirror/session
     remains usable.

### Stage 38 — preserve Kinozal response bytes and stop Settings blocking WebKit auth

Runtime evidence from the Stage 37 macOS ARM build:
- search `терминатор` still produced Rutor/RuTracker/NNM rows but no visible
  `KZ` rows;
- pressing Kinozal `Authorize / Re-login` opened the native WebKit/Cloudflare
  window behind the Settings dialog;
- because Settings was running through `QDialog::exec()`, it was modal and the
  user had to close Settings before the WebKit window could receive keyboard
  input.

Second Stage 37 transport bug:
- Stage 37 correctly moved `get_srv_details.php?id=<id>&action=2` into an
  in-page same-origin `fetch()`, matching current Kinozal browser helpers;
- however it then called JavaScript `response.text()` and passed the resulting
  Unicode string back to C++;
- Kinozal remains a Windows-1251 tracker and maintained integrations explicitly
  tolerate either UTF-8 or CP1251 server-details responses;
- once WebKit/Fetch had already decoded a CP1251 fragment as text, C++ no longer
  had the original bytes and the `Инфо хеш` label could be irreversibly
  damaged before `decodeTrackerText()` ran;
- Stage 37 additionally pre-rejected any response whose already-decoded text did
  not literally contain `Инфо хеш` or `Info hash`.

Current-site compatibility evidence checked before this change:
- current Kinozal userscripts call the exact endpoint with same-origin GET from a
  details page and extract the info-hash from the first `<li>`;
- current Jackett's Kinozal magnet definition also treats the first list item as
  the authoritative hash field;
- therefore exact identity does not need to depend on the human-readable
  Cyrillic label, only on the same trusted endpoint/id plus one valid 40-hex
  token in that authoritative first item.

Implemented:
- WebKit `fetchText()` now reads `response.arrayBuffer()`, transports it to
  Objective-C++ as base64, decodes it back to the original raw bytes, and only
  then hands it to the existing tracker decoder;
- no JavaScript text decoding occurs before C++ sees the server-details payload;
- `KinozalSource::applyServerDetails()` still prefers the explicit
  `Инфо хеш / Info hash` label, but falls back to a 40-hex token in the first
  `<li>`, matching current Kinozal/Jackett structure;
- the client no longer performs a duplicate pre-parser Cyrillic-label check:
  the exact-source parser itself decides whether identity is proven;
- Settings is now shown modelessly instead of through a stack
  `QDialog::exec()`, so native WebKit authorization windows can receive input;
- clicking RuTracker or Kinozal browser authorization temporarily hides Settings
  while the interactive native browser step is running, then restores it on
  success/cancel/failure;
- strict-source completion now records each provider's accepted count and keeps a
  12-second final status such as `Kinozal 4`; this distinguishes a tracker that
  produced zero verified releases from one whose verified hashes were later
  hidden by the existing cross-source hash deduplication rule;
- logs now contain one `[ExactSource]` line per provider with accepted,
  rejected and error fields.

Regression coverage:
- Kinozal server-details parsing now has a case where the human-readable hash
  label is deliberately unusable but the authoritative first `<li>` still
  contains the exact 40-hex info-hash;
- existing exact URL/id, full title, file-list and mirror tests remain unchanged.

Identity/persistence invariants deliberately preserved:
- one concrete `details.php?id=<id>` and the same numeric id's exact info-hash
  remain mandatory before a KZ row is emitted;
- cross-source search rows are still deduplicated by info-hash for now, because
  the database currently stores one authoritative source snapshot per hash.
  Stage 38 only exposes per-provider accepted counts so a future multi-source
  provenance design can be based on evidence rather than guessing.

Handoff:
- branch: `stage38-kinozal-browser-bytes`;
- parent: Stage 37 head
  `9f38d72613fe28c2a6b933548b843d1d0eb38624`;
- runtime acceptance on MacBook Air M1 / macOS Sequoia:
  1. open Settings > Indexer and press Kinozal `Authorize / Re-login`;
  2. Settings should disappear and the Kinozal/Cloudflare window must accept
     keyboard input immediately, without manually closing Settings;
  3. after successful authorization Settings may reappear;
  4. search `терминатор` and wait for the final exact-source status;
  5. inspect the temporary `accepted by source` counts, especially Kinozal;
  6. if `Kinozal > 0` but no KZ badge is visible, the remaining issue is
     cross-source same-hash provenance/deduplication rather than authentication
     or server-details transport;
  7. if a KZ row appears, selecting it must still show the exact release link,
     detailed description and exact hash/file metadata.

### Stage 39 — preserve Kinozal detail URL before moving search job

Commit message: `fix: preserve Kinozal detail URL before callback capture`.

Static audit of Stage 38 after repeated reports of no KZ rows found the same
C++ argument-evaluation defect previously fixed for RuTracker in Stage 30:
`browser_->get(job.detailUrl, [job = std::move(job)]...)` reads and moves
`job` in two separate function arguments. C++ does not guarantee their order;
Clang may move the job first and then pass an empty QUrl to WebKit. Kinozal
marks each attempted detail as a request failure, never reaches the same-ID
AJAX hash endpoint, and therefore emits no verified KZ releases.

Copy `job.detailUrl` to a local QUrl before moving the job into the callback.
The existing exact detail ID, server-details info-hash and strict-completeness
checks remain mandatory. This fixes a proven defect in the code path, but the
user's Stage 38 runtime log is not yet available; do not claim that it was the
only failure until macOS Sequoia runtime verification.

Build from Stage 38 HEAD `77d2c233dbe9db08f885d3d703291c7c64c4dc5e`;
keep the Stage 39 PR draft and do not merge before user acceptance.
