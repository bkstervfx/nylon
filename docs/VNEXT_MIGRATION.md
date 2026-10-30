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

## Current milestone M1: raw foundation

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

## Risks/gaps

No raw resume yet. Original ADS/EA/security/reparse fidelity/catalog missing. Namespace
success alone cannot prove whole-image domain/semantic completion.
