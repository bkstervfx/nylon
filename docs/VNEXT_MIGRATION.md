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
not whole-image evidence or deliberate omissions. M3 replaced writer path/size/time
binding; the transitional browser still lacks that identity gate. Real-model and
native-viewer validation remain outstanding.

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

## M5 — retained install-image catalog

- Purpose/prerequisite: retain original install-container XML already exposed by ISO
  preparation, using M1 identity and accounting; do not reapply unchanged payloads.
- Scope: bounded, DTD-prohibited XML parsing; exact catalog bytes; all exposed image
  indexes and repeated leaf metadata with original paths/order; selected-index binding;
  immutable new-cache retention with hash; explicit availability on legacy warm caches
  and expanded trees. Preserve the existing preparation/cache implementation.
- Non-scope: direct WIM/ESD/SWM input support, per-file original metadata extraction,
  full setup/boot media inventory, model/UI work, or claiming source_media complete.
- Completion invariant: every retained catalog has a parse outcome; when PRESENT each
  index is unique and represented, the selected index exists, and every extracted leaf
  is retained with provenance. Parse failures/unresolved selection remain explicit;
  exact XML is retained independently of projection. Missing/invalid retained metadata
  cannot become absence or invalidate unrelated namespace evidence. Old caches remain
  untouched and explicitly NOT_EXPOSED. source_media remains partial with known gaps.
- Compatibility: raw schema v5, fresh audit only; exact-hash semantic invalidation stays
  unchanged. Existing immutable cache format 1 accepted without catalog; new optional
  catalog/hash pair must agree. No in-place cache/database upgrades.
- Required validation: supported builds, existing foundation/binding fixtures, catalog
  Unicode/entities/repeats/unknown fields and malformed/DTD/depth/size fixtures, missing
  selected index, availability/accounting corruption refusal, legacy warm ISO run.
- Implemented: Windows XmlLite with DTD prohibited, null resolver and depth 64;
  64 MiB byte limit, exact XML/hash plus ordered leaf/attribute projection. New caches
  retain XML/hash before atomic publication. Read-locked warm loading never edits a
  cache; invalid retained XML remains raw evidence with a parse failure. Publication
  rehashes/reparses XML and compares every image, selected flag, field and ordinal;
  original metadata availability and partial source-media counters/gaps are gated.
- Verification (2026-10-30): fresh scanner/semantic builds; foundation, catalog and
  source-binding fixtures passed. Catalog tests exercise synthetic cold apply/retention
  followed by warm reuse with no additional tool calls, legacy warm caches unchanged,
  Unicode/numeric entities/repeated/unknown fields, malformed/DTD/depth/size cases,
  missing selected index, hash failures and 12 publication-corruption cases. Test-only
  SQLite connections were explicitly closed after a minimal reproducer showed the
  context manager commits but does not release Windows file handles. No guard weakened.
- Real evidence: build/real-m5-iso/output/image.db passed strict publication/integrity/FK
  checks: schema 5, same source hash/index and 122,063 namespace entries. Legacy catalog
  correctly NOT_EXPOSED; source_media PARTIAL and 14 families PENDING. Existing manifest
  unchanged. Separately, pinned 7-Zip extracted only sources/install.wim into ignored
  build/real-m5-catalog under a stable original-ISO lock; wimlib extracted XML, parser
  accepted all 3 indexes. XML SHA-256:
  4dea266ecb803488350380a724b2c736d099ef76b7f0e6a419c644f470c15f9e.
  No payload apply, original-cache mutation, real-model claim or remote CI claim.
- Scoped M5 invariant satisfied by the observed tests; source-media family and overall
  migration remain incomplete. Next: source-identity-bound metadata-only catalog reuse
  for legacy warm caches, without applying payloads or editing those caches.

## M6 — legacy-cache metadata-only catalog recovery

- Purpose/prerequisite: M5 `82ec18c` proves retention/accounting but legacy real caches
  still expose no catalog. Recover original metadata without changing their payloads.
- Scope: separate immutable source-hash-bound catalog sidecar, populated from install
  containers only using existing pinned tools. Reuse it across selected indexes. Do
  not call wimlib apply; do not update legacy manifests or mutate existing sidecars.
- Non-scope: original per-file metadata, setup/boot collection, direct container inputs,
  payload cache policy changes, semantic/UI changes or whole-image completion.
- Invariant: legacy image cache bytes remain unchanged; recovered XML and every index
  enter M5 accounting; repeated compatible runs reuse sidecar without extraction/apply;
  missing tools, extraction failure, corrupt sidecar and races remain explicit states
  and cannot erase unrelated evidence. Existing declared-catalog failures stay failures.
- Validation: synthetic recovery/reuse/tool-call trace, unavailable tools, extraction
  failures, sidecar identity/hash corruption, concurrent-publication-compatible behavior,
  full raw/binding regressions, real legacy ISO recovery and strict DB gate.
- Compatibility: raw schema remains v5; new catalog sidecar has independent versioned
  purpose/source/tool/hash manifest; old image caches remain accepted and untouched.
- Implemented: catalogs-v1/<ISO SHA-256> sidecar, atomic immutable publication, stable
  manifest/XML locks and source/tool/hash validation. Existing declared metadata errors
  are not masked. Successful fresh XML observations survive optional cache-publication
  failure with explicit diagnostic detail. Source-directory guard extracted unchanged
  for image readers; publishing/sidecar-parent readers allow child writes but deny
  deletion/reparse replacement. Entry readers retain the stronger read-only guard.
- Diagnosed with a minimal Windows reproducer: FILE_SHARE_READ on a directory blocks
  child publication (error 32); allowing FILE_SHARE_WRITE permits publication while
  still denying directory replacement. This applies only to writable sidecar parents;
  image-read directory protection was not weakened. Concurrent recovery fixtures passed
  after separating these lock policies, including repeated full catalog-suite runs.
- Verification (2026-10-30): fresh scanner and semantic builds, foundation/catalog/binding
  suites passed. Catalog suite covers recovery without apply, cross-index reuse,
  concurrent publication, unavailable tools, tool failures, sidecar hash/source mismatch,
  reparse parent refusal, malformed metadata, cache-publication failure retaining original
  evidence, and explicit/automatic index paths. No remote CI claim.
- Real ISO recovery: build/real-m6-iso/output/image.db passed the strict publication gate
  and SQLite integrity/FKs; source hash/index unchanged; 3 images, 32 fields each, exact
  XML hash matches M5's independently extracted catalog. Original image-cache manifest
  unchanged; recovery extracted install container metadata only, never applied payloads.
  Subsequent fresh build/real-m6-final/output/image.db passed the same checks: 122,063
  namespace entries, 3 images/96 catalog fields, PRESENT catalog, source_media PARTIAL,
  14 families PENDING. Sidecar XML hash and write time unchanged during reuse. Protected
  main still clean at e5f99e6. Overall migration remains incomplete.
- Scoped M6 invariant satisfied by those observations. Next dependency: deterministic
  offline reference parsing/namespace lookup before service/task/driver relationships.

## M7 — conservative offline reference boundary

- Purpose/prerequisite: M1 namespace and M6 catalog are usable; no active src resolver
  exists (source search). Domain collectors need evidence-backed paths without host
  expansion, basename guessing or accidental reparse traversal (critical edge case 4).
- Scope: versioned reusable path/quoted-command parser, explicit offline Windows root
  and relative-base context; inventory-only lookup with parent discovery/encoding states,
  case ambiguity and reparse barriers. Reuse the namespace comparison-key algorithm.
- Non-scope: service/task/driver collector completion, command-wrapper/module semantics,
  unknown variables/drives/NT-device mappings, inferred dependencies, model/UI work.
- Invariant: raw UTF-16 reference retained; PRESENT requires exactly one matching FILE
  through non-reparse directory ancestors; NOT_PRESENT requires complete discovery and
  encoding accounting of the relevant parent. Query failure/ambiguity/incomplete ancestry
  never become absence. No image/host payload is opened and no raw DB row is modified.
- Required validation: Unicode/long paths, known and unknown variables, explicit relative
  bases, quoting/unquoted ambiguity, malformed input, root escapes, absent/partial parents,
  reparses/case collisions/query errors; real-audit known references with unchanged DB hash;
  supported scanner build and existing fixture regressions.
- Compatibility: parser version offline-reference-v1; no new persisted schema/table or
  collector-status change. Future domain collectors must persist raw provenance/results.
- New evidence requires strengthening the old publication gate: a focused corruption
  fixture changed comparison_key and the existing gate accepted it. M7 lookup depends
  on those indexes, so the gate now streams original UTF-16 identities and checks exact
  readable/comparison projections and encoding states. This is a newly demonstrated
  invariant gap, not a restart of M1/M4 or a source/schema reinterpretation.
- Focused parser fixture also reproduced lexical '..' normalization erasing a reparse
  barrier and falsely resolving a file. Parent traversal now stays UNRESOLVED rather
  than being collapsed. Mixed-separator offline bases are normalized consistently;
  their prior false-NOT_PRESENT case was reproduced before the fix.
- Verification (2026-10-30): fresh scanner/semantic builds and foundation, catalog,
  reference and semantic-binding suites passed. New comparison-key, odd UTF-16 and
  readable-path corruption fixtures are rejected. Existing Unicode/invalid-UTF-16,
  long-path and registry rollback/redaction fixtures still pass. Read-only reference
  checks on build/real-m6-final/output/image.db and a fresh build/real-m7-iso/output/image.db
  resolve the observed kernel file and retain both reparse barriers as unresolved;
  exact DB hashes unchanged. Fresh M7 scan and strict publication verification passed.
- Scoped helper/gate invariant satisfied by those observations. No service/task/driver
  collector completion, general shell-command parsing, performance comparison, remote
  CI or whole-image completion is claimed. Next: offline control-set selection evidence
  as the prerequisite to service-definition/relationship collection.

## M8 — offline control-set evidence

- Purpose/prerequisite: parsed SYSTEM registry provenance and M7 reference boundary;
  canonical architecture requires Select evidence, never a guessed CurrentControlSet.
  Source search found no active offline control-set resolver to reuse.
- Scope: persist every direct ControlSet* candidate under the parsed SYSTEM root; typed
  Current/Default/LastKnownGood query outcomes and every matching raw value reference;
  preserve all Select/root-source ambiguity and relevant omission states. Support the
  standard positive ControlSetNNN/DWORD form, explicitly flag other formats unsupported.
- Non-scope: completed services/svchost collector, choosing one implicit active set,
  runtime service state, service definitions/dependencies, model or UI work.
- Invariant: all discovered candidates and three role queries represented with exact
  raw provenance; PRESENT selections require one parsed DWORD and one real target key;
  failed/missing hives, duplicate sources/Select keys/values/targets, wrong types, redactions
  and absent targets remain distinct. Publication recomputes and compares the projection.
  Services stays PARTIAL with an explicit remaining-definition/relationship gap.
- Validation: source/type/range/duplicate/redaction/target/absence fixtures, corruption
  refusal, active scanner integration, full regressions and real SYSTEM Select evidence.
- Compatibility: fresh raw schema v6; registry tables unchanged, semantic registry reader
  accepts v6 with existing exact raw-hash invalidation. No in-place audit/cache upgrade.
- Implemented: active scanner calls the domain collector after registry commit, in its
  own transaction. Compact service_control* tables bind SYSTEM source decisions, parsed
  root, all supported/unsupported ControlSet* candidates, Select keys, raw value matches
  and three role outcomes. Counts/quality and the remaining-services gap are gated.
- Root causes diagnosed: active registry stock paths use uppercase comparison identities,
  which a mixed-case-only synthetic source fixture initially concealed; discovery now
  compares the known ASCII stock path case-insensitively and tests both spellings.
  SQLite TEMP-table DROP was blocked by active verifier SELECT cursors, including callers'
  main-database cursors (minimal reproducer confirmed). Verification now reuses
  connection-local TEMP scratch tables rather than performing conflicting DROP DDL;
  recomputed projections still compare exactly and never modify main/raw rows.
- Verification (2026-10-30): fresh scanner/semantic builds and foundation, catalog,
  reference, control-set and source-binding suites passed. Control-set fixtures cover
  source failures, missing/duplicate roots/Select keys/values/targets, exact DWORD types,
  range/unsupported forms, omissions, genuine absence, and projection/counter corruption.
  Active scanner malformed-hive and reparse-ancestor tests retain three QUERY_FAILED
  role outcomes, not false absence. Semantic registry compatibility/invalidation passed
  for versions 4/5/6; no whole-evidence semantic completion claim.
- Fresh real run build/real-m8-iso/output/image.db passed strict publication, reference,
  control-set and integrity/FK checks: schema 6, same original ISO hash/index 2, 122,063
  namespace entries, 3 catalog indexes, one ControlSet001, all three Select roles PRESENT
  and linked to its actual raw key. Services PARTIAL (5/5 units, one remaining-scope gap),
  source_media PARTIAL, registry COMPLETE_WITH_GAPS, 13 families PENDING. Independent
  verify-only control-set pass left the exact raw DB SHA-256 unchanged. No remote CI,
  real-model/hardware, native Atlas or whole-image completion claim.
- Scoped M8 invariant satisfied by those observations. M9 service definitions/config
  and references are not implemented. Read-only next-scope inspection found 688 direct
  candidate keys under SYSTEM/ControlSet001/Services; only 644 have Type/Start, and 631
  have ImagePath (629 REG_EXPAND_SZ, 2 REG_SZ). These are observed raw counts, not proof
  that all candidates are valid services or that their references resolve.

## Context checkpoint / exact resume

- This checkpoint contains verified M8 implementation; `git rev-parse HEAD` identifies
  its final commit. Previous implementation checkpoint: 8f0a61067214c7ca338398b86fa2f96bb5097d18.
- Completed local scoped milestones: M1 49b5f87, M2 3709c2d, M3 bf8b56a, M4 1096fe0,
  M5 82ec18c, M6 586235b, M7 8f0a610, M8 in this checkpoint. Phase A is already complete;
  do not restart it. Product migration is NOT complete; no architectural blocker known.
- Next milestone M9 is NOT implemented or formally defined: service-definition inventory,
  typed configuration and evidence-backed references using all discovered control sets,
  not only the Current role. Define its scoped invariant/compatibility/tests before edits.
- Exact next inspection: src/services/control_sets.cpp and its raw source links;
  src/evidence/references.hpp/.cpp; src/registry/bulk_writer.cpp schemas/indexes;
  src/main.cpp registry -> control-set -> publication sequence; tests/control_sets.py and
  tests/foundation.py. Discovery should use indexed raw parent IDs, not directory rescans
  or substring matching against nested DriverDatabase/other Services keys.
- Preserve malformed/unknown service candidates and all raw key/value/omission links.
  M7 intentionally leaves unquoted commands ambiguous, unknown bases/drives/variables and
  parent traversal unresolved; do not turn these into guessed file/dependency edges.
- Latest real audit: build/real-m8-iso/output/image.db (schema 6), scan.log alongside;
  strict gate/integrity/FKs passed. Source ISO remains Downloads/windows.iso with hash
  recorded above, selected index 2. Existing image payload cache was not upgraded/applied;
  metadata-only sidecar is cache/catalogs-v1/<ISO-sha> under the persistent runtime root.
- Commands: cmd /d /c .\build.cmd; cmd /d /c .\build-semantic.cmd;
  cmd /d /c .\test-foundation.cmd; cmd /d /c .\test-catalog.cmd;
  cmd /d /c .\test-references.cmd; cmd /d /c .\test-controlsets.cmd;
  python tests/source_binding.py. Verify real audit with
  .\bin\nylon-foundation-test.exe --verify .\build\real-m8-iso\output\image.db,
  .\bin\nylon-references-test.exe --real <same-db>, and
  .\bin\nylon-controlsets-test.exe --verify <same-db>.
- Unfinished: service definitions/svchost/relationships, 13 PENDING families, source-media
  remainder and original per-file metadata, direct WIM/ESD/SWM input, raw resume,
  whole-evidence semantics, native Atlas, safe export, modification/re-scan/retained-
  function verification. Registry-only planning/maps under build/real-m2 belong to that
  older exact audit; never attach them to the schema-6 audit. Browser identity gating
  remains an explicit gap. No remote CI or real-model/hardware claim.
- Protected main remains clean at e5f99e6; no pushes, PRs, merges or source-image edits.
