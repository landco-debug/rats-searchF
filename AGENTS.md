# AGENTS.md

## Project handoff policy

- Hardware target: MacBook Air M1, macOS Sequoia.
- Work from the user's fork: `landco-debug/rats-searchF`.
- Keep every functional change in Git commits.
- Before continuing an existing feature, read the relevant handoff document under `docs/`.
- Do not assume a green CI run means the user-visible behavior is accepted.
- Do not merge experimental branches unless the user explicitly accepts the behavior.

## Handoff: exact torrent release information — 2026-09-26

**Status: current rich-metadata approach is a functional dead end and must not be continued blindly.**

Canonical handoff:
- `docs/HANDOFF_EXACT_TORRENT_RELEASE_INFO_RU.md`

Git state at handoff:
- stable baseline: `master@fd72202acda7aba181726abd911e6e68f7183b8e`
- experimental branch: `fix/rich-torrent-metadata`
- PR: #4
- last functional commit before handoff: `67e94fed161f96634834fbd4e6d05a5b03c2690d`
- handoff document commit: `ecae1420ecc750322e1563c9f965ceca4d640a9f`
- PR #4 is mergeable and CI-green, but **rejected by the user on functionality**.

What failed:
- runtime guessing of a release page from info-hash/title/size;
- generic tracker search links;
- exact-hash links to trackers that do not index that hash;
- accumulating more public metadata sites;
- generic movie APIs used as a substitute for release-specific metadata.

Important discovery for the next chat:
- legacy `legacy/background/strategies/rutor.js` used a P2P-distributed sidecar database `hash -> Rutor topic id`;
- when the sidecar file was absent it called `p2p.file('rutor')`;
- this provenance mechanism is fundamentally different from the current C++ post-hoc search approach;
- next investigation should focus on restoring that architecture or persisting source provenance at indexing time.

Recommended next step:
1. Read the canonical handoff completely.
2. Inspect `legacy/background/strategies/rutor.js` and `legacy/background/p2p.js`.
3. Trace the current native spider/indexing/P2P path and find where source tracker/topic provenance is lost.
4. Prefer a clean new branch from `master@fd72202...`.
5. Cherry-pick only proven reusable pieces from PR #4 (parser/tests/UI geometry fixes) after the new architecture is chosen.

User acceptance requirement:
- information must belong to the **selected concrete torrent release/file**;
- if a source URL is shown, it must open the **concrete matched release page**;
- no generic search-result link counts as a fallback.
