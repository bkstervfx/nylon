# vNext migration ledger

Implementation status only; canonical requirements remain unchanged.

## Baseline and audit gate

- Base/initial HEAD: `e5f99e66ed7c9c29c461578806f479bc73021825`.
- Worktree: `C:/Users/Administrator/.t3/worktrees/nylon/t3-dd60cab3`;
  branch `t3/nylon-vnext-migration`; initial tree clean; protected main separate.
- Phase A read all canonical documents, traced active builds/callers/schemas/CI and
  recent history, without edits. Self-review found no canonical contradiction/blocker.

| Subsystem | Implementation state | vNext disposition | Evidence at gate |
|---|---|---|---|
| ISO/cache | ACTIVE_PARTIAL | REUSE_AFTER_EXTRACTION_OR_CLEANUP | media::prepare/verify_prepared_cache; SOURCE_ONLY, RECORDED_REAL_IMAGE |
| REGF/bulk writer | ACTIVE_MATURE_FOR_CURRENT_SCOPE | REUSE_AFTER_EXTRACTION_OR_CLEANUP | hive.cpp, registry::scan_all, bulk_writer.cpp; synthetic workflow definitions, RECORDED_REAL_IMAGE |
| Platform/hash/staging | ACTIVE_PARTIAL | REUSE_AS_IS | platform/*, database/staging.cpp, main.cpp publication; SOURCE_ONLY |
| Generic graph | TRANSITIONAL | REFERENCE_ONLY | database.cpp; active registry path uses dedicated hive/key/value tables |
| Registry semantics | ACTIVE_PARTIAL | REUSE_AFTER_EXTRACTION_OR_CLEANUP | build-semantic -> semantic_v4 including semantic.cpp, preclassify_v2/postprocess_v2; synthetic workflow definitions, recorded partial model/hardware runs |
| Older semantic entrypoints | HISTORICAL_OR_INACTIVE | REFERENCE_ONLY | v2/v3 not directly built; semantic.cpp remains active shared composition |
| Browser Atlas | TRANSITIONAL | DELETE_AFTER_VNEXT_PARITY | build-viewer -> generated atlas/delta composition; not native Atlas |

Missing: shared inventory, robust derived binding, per-family raw accounting, most
non-registry collectors/relationships, native Atlas, safe export, modification/re-scan.
SAM/SECURITY raw ingestion violates the sensitive-evidence boundary. Required-hive
errors abort publication, optional discovery skips some permission failures, semantic
fingerprint uses path/size/time. Browser whole-image labels overstate current scope.
These are implementation gaps, not grounds to weaken canonical requirements.

Gate reviewed dependency order, stale generic abstractions, prototype coupling,
unnecessary rewrites, premature AI/UI and validation overclaims. Historical results
are not fresh evidence. Requested database-design skill is unavailable; no extra skills.

## Dependency sequence

1. Audit/source identity, dedicated raw schema, canonical namespace and accounting.
2. Inventory-fed registry, sensitive omissions, independent failures, robust derived binding.
3. Source/catalog/authoritative metadata; servicing, task, AppX, INF, PE/config collectors.
4. Evidence-classed cross-source ownership/references/dependencies and reverse indexes.
5. Whole-evidence semantic planning, bounded safe inference and source accounting.
6. Native Atlas parity, retire browser; safe sharded export.
7. Separate modification and source/target retained-function verification.

## M1: raw foundation (checkpoint 49b5f87)

- Purpose/prerequisite: share discovery before domain parsing; audit gate passed.
- Scope: run-specific identity with source/index/schema/policies, dedicated entries,
  exact UTF-16 paths/comparison keys, working-view metadata, no-follow reparses,
  collector runs/gaps and required-family status.
- Non-scope: source metadata fidelity, payload hashing, domain parsing, whole-image
  completion, semantic/UI redesign, image modification.
- Invariant: each enumerated entry has one canonical row with parent/provenance;
  reparses represented, never followed; failed enumeration persists a gap preventing
  discovery-complete. Representation and discovery checked separately. Cache metadata
  never substitutes for original metadata silently.
- Identity: ISO SHA-256 anchors original media, not cache content. Expanded-image
  source identity is explicitly weaker/run-scoped. Derived binding must identify exact DB.
- Compatibility: additive raw schema v3, fresh staged rebuild, retain registry tables;
  no in-place mutation/upgrade of existing audit evidence.
- Validation required: supported scanner build; deterministic namespace fixtures,
  Unicode/long paths/hardlinks/reparse loops/escapes/query failures; count/parent/gap
  invariants and SQLite integrity; registry regression. Real-image run only if available.
- Verification (2026-10-30): supported build.cmd exited 0; test-foundation.cmd exited 0.
  Fixtures exercise empty namespace, repeat-run identity, Unicode/invalid UTF-16, long
  paths, hardlinks, junction loops/escapes, unsafe root, sharing failure, ACL denial,
  explicit depth gap, SQLite integrity/FKs/count/parent state and synthetic REGF keys/values.
  CI now invokes the same fixture command; no remote CI run claimed.
- Fresh real prepared-tree observation: standalone namespace harness scanned the locally
  available index-2 cache recorded under ISO hash 4f59662a96fc1da48c1b415d6c369d08af55ddd64e8f1c84e0166d9e50405d7a.
  122,063 discovered/represented, 23,716 directories, 98,345 files, 2 reparses;
  discovery/representation invariants passed; no namespace gaps; integrity/FKs passed.
  Wall time 7.89s on this run, not a performance comparison. Harness treated this as an
  expanded tree; manifest was read but original ISO/content fidelity was not revalidated.
- Diagnosis: FILE_READ_ATTRIBUTES-only directory handles bypass Windows sharing checks.
  A minimal access-mask probe reproduced it; guards now request FILE_LIST_DIRECTORY too.
  The blocked-directory fixture failed before and passed after that focused correction.
- M1 checkpoint: the commit containing this ledger update (resolve with git log).
- Next milestone M2: inventory-fed registry, sensitive omission, independent failure
  accounting and cryptographic raw-to-map binding. Preserve existing REGF parser/bulk writer.
  Define its exact scope/invariant before implementation; do not claim whole-image completion.

## M2: inventory registry (locally verified; prerequisite M1 at 49b5f87)

- Purpose: consume shared inventory for registry discovery without losing independent evidence.
- Scope: stream file-header probes from fs_entries, preserve probe/error accounting and
  canonical hive links, omit SAM/SECURITY and unreviewed hives, isolate per-hive failure,
  remove redundant registry directory walks. Preserve REGF parsing and batched writes.
- Non-scope: credential extraction, universal export safety, new semantic ontology/UI,
  authoritative original WIM metadata, recovery/replay of dirty hives.
- Invariant: every inventory file probe or required missing-hive query has an explicit
  terminal result; each parsed hive links to its filesystem source; known sensitive and
  unreviewed hive payloads are never imported; a failed hive cannot leave orphaned partial
  rows or erase namespace/other-hive evidence. Missing is inferred only after complete
  namespace discovery; otherwise unresolved. Required failures publish a partial audit.
- Compatibility: fresh raw schema v4, registry policy versioned; existing raw audits not
  upgraded. Use disk-backed SQLite rollback journaling for per-hive savepoints (OFF cannot
  guarantee rollback); staging remains unpublished until integrity/accounting passes.
- Verification required: supported build; fresh fixture tests for redaction, arbitrary-path
  REGF probes, missing/dirty/malformed hives, partial-hive rollback, namespace denial,
  parent/source/count invariants and registry semantic regression. Real-image raw run
  if available, excluding sensitive payload ingestion. Derived-binding work follows.
- Verification (2026-10-30): build.cmd and test-foundation.cmd exited 0. Fixtures prove
  canonical source links; sensitive/unreviewed omission; dirty/missing source accounting;
  rollback of a malformed hive after >256 keys/values forced real batch writes; namespace
  denial does not erase unrelated evidence and cannot establish missing-hive absence;
  name-based value omission; SQLite integrity/FKs. Portable CI startup expectations now
  assert explicit partial publication instead of the obsolete abort-on-invalid-hive rule.
- Fresh prepared-tree scanner observation: 24.32s; filesystem 122,063/122,063;
  registry probe/query accounting 98,348/98,348 (98,345 files plus 3 absent optional paths).
  4 stock hives parsed, 2 sensitive sources omitted, 51 unreviewed sources omitted,
  3 optional sources absent, 98,288 non-REGF probes. 335,063 imported keys, 597,504 imported
  values and 3,198 explicit sensitive-name omissions. Integrity/FKs and source/accounting
  publication invariants passed. This is an expanded prepared-cache observation, not a
  fresh original-media identity/fidelity validation or proof of secret-free export.
- Checkpoint: the commit containing this update. Next dependency: cryptographic raw/map
  binding and refusal of incompatible classifier/linker input, retaining semantic behavior.

## Risks/gaps

No raw resume yet. Original ADS/EA/security/reparse fidelity/catalog missing. Namespace
success alone cannot prove whole-image domain/semantic completion.
Conservative omission is not universal secret detection. Safe external export remains
unimplemented. Existing semantic source accounting covers only imported registry rows,
not whole-image evidence or deliberate omissions. Path/size/time map binding remains
until the next milestone. Real-model and native-viewer validation remain outstanding.

## M3: exact derived binding (checkpoint bf8b56a; prerequisite M2 at 3709c2d)

- Purpose: prevent resumable semantic output from attaching to a different raw audit.
- Scope: reuse streaming SHA-256/platform read locks; fingerprint immutable finalized raw
  DB bytes; check binding before classifier/linker mutations; accept compatible registry
  tables in schema v4 while retaining synthetic schema-v2 regression. Version build marker.
- Non-scope: whole-image semantic planner, model/ontology changes, export, native viewer.
- Invariant: raw changes invalidate the map even with unchanged path/size/timestamp;
  byte-identical relocated raw input remains compatible; unfinalized WAL/journal input
  is refused; source is held read-only against concurrent replacement/writes; auxiliary
  writers refuse a mismatched map before any map update. Semantic coverage still means
  imported registry-source accounting only, never omitted values or whole-image coverage.
- Compatibility: fingerprint version changes invalidate derived maps, not raw evidence.
  Runtime keeps its existing derived rebuild policy; standalone auxiliary tools refuse
  incompatible maps. No third-party pin/model/request-contract change.
- Validation required: build all active semantic targets; existing synthetic planner,
  preclassifier, resume, fake-local-HTTP and hierarchy invariants; same-size/time mutation,
  relocation, dirty-sidecar and mismatched-writer refusal fixtures; v4 raw planning smoke.
- Verification (2026-10-30): build-semantic.cmd built all three actual active targets;
  tests/source_binding.py passed synthetic planning/preclassification/resume/local fake
  HTTP/grounded-request-shape/hierarchy and exact imported source-reference accounting;
  equal-size/equal-time raw mutation invalidates maps, identical relocated raw input
  remains compatible, auxiliary mismatch and nonempty WAL input are refused before map
  writes, v4 audit IDs are recorded, sensitive/unreviewed hive input is refused.
  test-foundation.cmd also passed after this integration; no remote CI success claimed.
- Fresh real v4 raw planning: 12,623 jobs, 932,567 imported registry sources. Separate
  preclassifier added 3,382 deterministic units; deterministic-only resume reused the
  persisted plan and committed 4,459 jobs / 340,717 imported sources. No real model calls.
  Independently recomputed streaming raw SHA-256 and audit ID matched map metadata;
  map integrity_check passed. This does not prove whole-image semantic accounting,
  omitted-value interpretation, real-model quality or complete semantic resolution.
- Checkpoint: the commit containing this update. Next: source/media catalog and original
  metadata availability, then inventory-fed deterministic domain collectors. Browser
  identity/reachability/native parity and safe export remain explicitly outstanding.

## M4: publication-gate regression (locally verified)

- Prerequisite: M1/M2 foundation and M3 checkpoint bf8b56a.
- Purpose/scope: review found verify() checks filesystem accounting but not preservation
  of every required collector row or all recorded quality-counter/gap consistency. Tighten
  that gate before adding new collectors; otherwise a future regression could erase a
  required pending family without failing publication. Verify missing-source queries from
  inventory only, not fresh unguarded image path probes.
- Non-scope: new collector families, source/media fidelity, semantic expansion or UI.
- Invariant: every required family has one run bound to this audit; filesystem counters,
  error states and parent paths agree with represented rows/gaps; registry probe coverage
  and all nine stock-source queries remain accounted. Corrupted fixtures must be refused.
- Compatibility: stricter verification of the existing v4 contract, no schema change.
- Required validation: reproduce acceptance of a deleted required-family row, then test
  refusal after the fix; corrupted counters/gaps/parentage; reparse-ancestor absence;
  supported scanner build, full foundation and semantic binding fixtures.
- Root cause reproduced: a fixture deleting the required appx collector row was accepted
  by the old verify() gate (test failed). Centralized required-family identities and
  verified each run's presence/audit/status; the same negative fixture is now refused.
- Verification (2026-10-30): fresh build.cmd and test-foundation.cmd passed; negative
  fixtures also reject inconsistent filesystem/registry counters, hidden encoding gaps,
  invalid parent paths and missing file probes even after counts are adjusted. Junction
  ancestors yield unresolved stock-hive queries, never false absence or followed payloads.
  tests/source_binding.py passed again; fresh verify-only checks passed on the prior
  prepared-tree audit and the new ISO-backed audit. No remote CI claim.
- Fresh original-ISO-backed run: source at the local Downloads/windows.iso independently
  hashed to 4f59662a96fc1da48c1b415d6c369d08af55ddd64e8f1c84e0166d9e50405d7a;
  scanner held its own stable source lock and rehashed it, selected index 2, verified and
  reused the existing cache. Audit publication passed the stronger gate; 122,063 namespace
  entries, 98,348 registry probes/stock queries, 4 parsed hives, 53 omitted sources and
  3,198 name-policy omissions. Integrity/FKs passed; audit PARTIAL, 15 families PENDING.
  Wall time 12.42s on this warm-cache run is not a comparison/performance guarantee.
  Original ACL/attribute/stream/reparse fidelity and full media catalog remain uncollected.
- Generated evidence/logs remain ignored under build/real-m4-iso and build/real-m2;
  none are committed. Source images, original media and protected main were not modified.
- Next dependency: source/media catalog retention and authoritative metadata availability,
  then indexed deterministic domain collectors. This is not migration completion.
