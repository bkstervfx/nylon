# Nylon agent instructions

This file is the short, mandatory orientation for any coding agent working in this repository.

Before making architectural changes, read:

- `docs/PRODUCT_GOAL.md`
- `docs/ARCHITECTURE.md`

## Non-negotiable product goal

Nylon is an exhaustive, read-only, offline Windows-image dumper and semantic mapper. It is not a registry-only product.

The intended pipeline is:

```text
Windows ISO / expanded image
    -> exhaustive Nylon evidence collection
    -> image.db (raw structural truth)
    -> local small-model semantic lifting
    -> image.map.db (readable whole-image semantic truth)
    -> Atlas native viewer (unrestricted whole-image exploration)
    -> export for a larger reasoning model
    -> evidence-backed stripped-image design and verification
```

The final purpose is to understand an exact Windows image deeply enough to design the smallest practical stripped image while preserving explicitly retained functionality.

## Coverage rules

- Never call registry-only coverage "full image coverage".
- A collector may declare complete only when its own source-accounting invariant passes.
- Preserve failures and uncertainty explicitly (`PRESENT`, `NOT_PRESENT`, `QUERY_FAILED`, `ACCESS_DENIED`, `NOT_EXPOSED`, `UNRESOLVED`, or equivalent). Never convert a failed query into absence.
- Do not sacrifice raw-source coverage for semantic speed.
- Deterministic classification may replace model inference only when it preserves the raw evidence and does not invent unsupported meaning.
- The small model normalizes/labels evidence. It is not the authority for raw facts and is not the final removal-decision engine.
- Keep exact provenance from semantic entities back to raw evidence.

## Performance rules

Optimize primarily by avoiding unnecessary work:

- parse once and persist facts;
- reuse immutable caches;
- do not rescan/replan unchanged evidence;
- prefer deterministic parsers/recognizers over LLM calls when structure is authoritative;
- batch SQLite work;
- stream large datasets;
- avoid duplicating payload bytes already present in the immutable image cache;
- keep model/runtime resident when useful rather than restarting it;
- recompute only invalidated derived state;
- never add an arbitrary node/data cap merely to make the UI faster.

## Viewer rules

The browser viewer is a prototype, not the final product architecture.

The target Atlas viewer is a native Windows application:

- C++20
- Win32 window/input
- Direct3D 11 world renderer
- Direct2D overlays/UI
- DirectWrite text
- SQLite opened read-only
- deterministic hierarchical layout and level-of-detail
- spatial indexing / viewport-driven queries
- no continuous force physics
- no always-running render loop when the scene is idle
- no browser, HTTP server, JavaScript, Electron, Tauri, Qt, or WebView dependency in the final viewer
- no arbitrary node limit; all collected evidence must remain reachable through zoom, search, drill-down, or raw inspection

Stable coordinates are preferred so the same component remains in the same place across sessions.

## Current implementation reality

Do not confuse the target architecture with what is already implemented.

At present, Nylon's deeply implemented source collector and semantic vertical slice are registry-focused. The current semantic completeness invariant is exhaustive over imported registry keys/values, not over the entire Windows image. Future work must add whole-image collectors and corresponding coverage invariants.

The current web Atlas/progress viewer may be used during transition, but new long-term viewer work should target the native Atlas architecture in `docs/ARCHITECTURE.md`.

## Known historical lesson

Earlier audit work discovered that most modern WinSxS `.manifest` files in the target Windows 11 build were not ordinary XML; treating them as XML left a large DCM/PA30 servicing-format gap. Do not repeat that assumption. Record unsupported formats honestly and build authoritative servicing/component ownership only from evidence that is actually decoded or independently corroborated.

## Safety of modification work

Collection and semantic mapping are read-only by default. Do not modify the source Windows image during audit/map phases.

When later implementing stripping/modification, removal decisions must be based on evidence-backed dependency/ownership relationships. Do not directly delete files merely because their names appear related to a removable feature.