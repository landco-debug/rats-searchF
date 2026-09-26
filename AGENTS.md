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
