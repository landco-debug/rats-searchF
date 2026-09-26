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
