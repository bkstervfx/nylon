# Nylon architecture

This document defines the intended long-term architecture. Current implementation status is noted separately so agents do not mistake the target for already-shipped behavior.

## 1. System layers

```text
SOURCE
Windows ISO / expanded image

PREPARATION
verified extraction + selected-image apply
immutable expanded image cache

RAW COLLECTION
Nylon collectors -> image.db

SEMANTIC LIFT
bounded deterministic + local-model normalization -> image.map.db

VISUALIZATION
native Atlas viewer -> read-only image.db + image.map.db

EXPORT / DEEP ANALYSIS
readable evidence bundle -> larger reasoning model

FUTURE MODIFICATION
reproducible stripping plan -> modified image -> verification
```

## 2. Source and preparation

The source image is treated as evidence.

Preparation should remain loss-minimizing while avoiding unnecessary Windows servicing/mount dependencies. The existing persistent cache strategy is valuable because expensive ISO/WIM preparation should happen once per source identity and selected image.

The expanded image cache is payload storage. Do not duplicate entire payload contents into SQLite merely to claim coverage.

Where preparation intentionally omits information (for example ACL/security-descriptor fidelity or selected attributes), that limitation must be represented explicitly in coverage/provenance rather than forgotten.

## 3. `image.db` — raw structural database

`image.db` is the deterministic, queryable structural representation of the selected offline image.

### Principles

- read-only source handling
- append/build in staging, publish atomically
- collector-specific tables when generic graph representation would be too expensive
- bulk ingestion
- stable source IDs
- exact provenance
- explicit collector status and coverage accounting
- failures are data
- no LLM required to interpret raw facts

### Collector architecture

Collectors should be independent enough that one unsupported area does not destroy unrelated evidence. Each collector should expose:

```text
collector identity/version
start/completion status
discovered object count
represented object count
failed/unsupported/unresolved counts
coverage invariant
provenance
```

Desired collector families include:

```text
image/media metadata
filesystem
registry
servicing/CBS
WinSxS/components
features/capabilities
AppX/MSIX/SystemApps
services
scheduled tasks
drivers/DriverStore/INF/devices
PE metadata/signatures
COM/WinRT/shell
boot/recovery/BCD
configuration/manifests
certificates/trust surfaces
cross-source relationships
```

Do not force every domain into one generic object/property schema if a dedicated compact table is materially faster.

## 4. Performance model

The primary optimization rule is:

> Avoid unnecessary work rather than merely making unnecessary work faster.

Apply this consistently:

- immutable source/cache identity -> skip repeated extraction
- persisted plans -> skip repeated planning
- deterministic parsers -> skip LLM inference
- namespace recognizers -> skip model calls for known structures
- streaming readers -> avoid giant intermediate representations
- batch inserts -> avoid per-row transactions
- direct joins/indexes -> avoid repeated graph reconstruction
- content hashes only when they add value; do not blindly hash giant payload sets
- parse each raw artifact once when possible and persist reusable facts
- cache derived layout/state and invalidate only affected branches

Coverage must not be reduced to achieve speed. If an artifact is not deeply parsed, it still needs an explicit represented state such as unsupported/unresolved.

## 5. Semantic lifting — `image.map.db`

`image.map.db` is derived and reproducible. `image.db` stays immutable.

### Semantic pipeline

```text
image.db
   -> deterministic planner
   -> deterministic recognizers/preclassification
   -> bounded ambiguous work units only
   -> local small model
   -> schema/source validation
   -> semantic nodes/edges/source links
   -> deterministic cross-source/hierarchy reconstruction
```

### Zero-token path

A zero-token path is any work unit normalized without an LLM because deterministic evidence already establishes its structural meaning.

Zero-token must never mean "drop the evidence." Raw source objects remain represented and linked.

### Model role

The model should return compact grounded semantics, not graph bookkeeping. Prefer deterministic code for:

- IDs
- containment edges
- exact source ownership
- hierarchy reconstruction
- known Windows namespace classification

Use the model only for ambiguity that benefits from language understanding.

## 6. Atlas native viewer

The final Atlas viewer is a native Windows application, not a website.

### Technology

```text
C++20
Win32             window, input, lifecycle
Direct3D 11       primary world/Atlas renderer
Direct2D          vector overlays and UI primitives
DirectWrite       labels/text
SQLite            direct read-only database access
```

The long-term viewer should not require:

```text
browser
localhost HTTP server
JavaScript
Node.js
Electron
Tauri
Qt
WebView2
```

The existing browser viewer is transitional/prototype code and may remain useful while native Atlas is built.

### Core UI principle

Atlas represents the whole collected Windows image in one navigable coordinate space.

There is no arbitrary node cap. "Performance" is achieved with representation and level-of-detail, not by hiding inaccessible data.

At low zoom, draw major semantic domains/clusters. As the user zooms, resolve clusters into subsystems, components, entities, and finally raw evidence. All underlying data remains searchable and reachable.

### Stable deterministic layout

Do not use continuous force-directed physics for the Atlas.

Layout should be deterministic and persistable. The same semantic component should remain in the same place across sessions when its relevant source hierarchy has not changed.

Derived layout may live in dedicated tables/database and include fields such as:

```text
semantic_id
layout_level
x
y
width
height
cluster/parent
bounds/version
```

Recompute only invalidated branches when possible.

### Spatial/LOD pipeline

```text
image.map.db
   -> semantic/hierarchy index
   -> persistent layout coordinates
   -> spatial index
   -> viewport query
   -> LOD selection
   -> compact GPU instance buffers
   -> D3D11 draw
```

Do not create one heavyweight UI widget per database object.

The world renderer should favor compact GPU instances/batches. Labels and inspector text should be drawn only when useful at the current scale.

### Rendering lifecycle

No perpetual animation loop is required.

Render on:

```text
pan
zoom
selection
resize
semantic progress change
short intentional transition/animation
```

Then sleep.

Idle Atlas CPU/GPU use should be near-zero apart from OS/window bookkeeping.

### Interaction model

Primary controls should stay sparse and predictable:

```text
drag            pan
wheel           zoom
click           select
double click    focus/drill
Ctrl+F          search
Alt+Left        history back
Home            full-image view
Esc             clear/focus-up
```

The map should dominate the screen. Avoid dashboard/card-heavy UI.

### Inspector

Selection should expose readable semantic information plus provenance and direct access to raw evidence. A user should be able to move from:

```text
component
 -> package/service/task/file relationship
 -> raw file/registry/task/manifest evidence
```

without leaving the Atlas mental map.

## 7. Whole-image hierarchy

The top-level Atlas is generic, not registry-centric. A reasonable conceptual shape is:

```text
Windows Image
├── Servicing
├── Runtime
├── Applications
├── Drivers / Devices
├── Configuration
├── Security / Trust
├── Networking
├── Boot / Recovery
├── Users / Profiles
└── Raw Evidence
```

Exact hierarchy should be evidence-driven and may evolve, but registry is an evidence layer rather than the root definition of the product.

## 8. Cross-source graph

High-value relationships include, where evidence supports them:

```text
service -> executable
service -> ServiceDll
service -> svchost group
driver service -> SYS file
driver -> INF / DriverStore package
scheduled task -> executable/script
COM class -> server binary
AppX package -> manifest/payload
CBS package -> MUM/catalog/component
component -> files/registry
file -> hardlink identity
file -> PE metadata
file -> signer
feature/capability -> package/component
boot entry -> boot file/device
```

Relationships should preserve confidence/evidence class.

## 9. Known servicing-format risk

Do not assume WinSxS `.manifest` files are ordinary XML. Historical work on the target Windows 11 family found that most were binary DCM/PA30-like servicing data and an XML-only mapper covered only a small subset.

Until a format is actually decoded or corroborated from another authoritative source, retain the artifact and mark its parse state honestly. Do not infer package/component ownership merely from filenames.

## 10. Export for larger-model reasoning

The final map should support a compact portable export that is substantially more readable than the raw image dump while retaining stable references back to raw evidence.

The export should emphasize:

- semantic entities
- hierarchy
- dependencies/reverse dependencies
- ownership
- source confidence
- retained raw references
- unresolved/failed areas
- coverage manifest

The purpose is to avoid feeding a larger model millions of low-level rows when deterministic preprocessing and the small model can first turn them into tens of thousands of meaningful, grounded structures.

## 11. Current implementation status

As of this architecture document:

- ISO/selected-image preparation and persistent cache exist.
- direct offline registry collection is deeply implemented and optimized.
- registry semantic planning/lifting and exact imported-registry source accounting exist.
- the current Atlas/browser UI is transitional.
- full filesystem/PE/servicing/task/AppX/etc. collection has not yet been implemented as complete first-class collectors.
- native Atlas has not yet replaced the web viewer.

Agents must preserve this distinction in documentation, UI labels and completion claims.

## 12. Definition of done for the product vision

The product vision is not complete until:

1. the selected Windows image is exhaustively inventoried across required collector families;
2. each collector has explicit coverage accounting and unresolved states;
3. cross-source relationships are reconstructed with provenance/confidence;
4. semantic lifting covers the full collected image, not only registry evidence;
5. Atlas can navigate the full semantic/raw image without arbitrary data limits;
6. the native viewer is stable, deterministic, performant and largely idle when not interacting;
7. a portable readable export can be handed to a larger reasoning model;
8. later modification work can trace removal decisions back to exact source evidence and verify the resulting image against a retained-function contract.
