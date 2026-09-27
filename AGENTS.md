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
