# Nylon architecture

This document defines Nylon's intended long-term architecture. Current implementation status is called out separately so agents do not mistake the target for already-shipped behavior.

## 1. System layers

```text
ORIGINAL SOURCE
ISO / WIM / ESD / SWM / expanded Windows image
    |
    +--> source/media identity + container/image metadata
    |
    v
SELECTED WINDOWS IMAGE
    |
    v
PREPARED WORKING VIEW
verified extraction/apply + immutable local cache
    |
    v
RAW COLLECTION
Nylon collectors -> image.db
    |
    v
SEMANTIC LIFT
deterministic + bounded local-model normalization -> image.map.db
    |
    v
VISUALIZATION
native Atlas -> read-only image.db + image.map.db (+ disposable layout cache)
    |
    v
EXPORT / DEEP ANALYSIS
safe readable evidence bundle -> larger reasoning model
    |
    v
FUTURE MODIFICATION
reproducible stripping plan -> modified image -> fresh Nylon scan -> verification/diff
```

The architecture deliberately separates original-source evidence, prepared working state, raw structural evidence, derived semantic interpretation and visualization.

## 2. Original source and media context

The original ISO/container/image is the provenance anchor.

For ISO/container input, record enough information to establish exactly what was audited and to reconstruct media context where useful:

```text
ISO/container identity + SHA-256
install image/container identity (WIM/ESD/SWM)
install container hash where practical
available image/index catalog metadata
selected index/name/edition/build/architecture/language
useful ISO setup/boot/recovery structure
preparation tool/version/policy
```

The selected Windows image receives exhaustive deep collection. Other indexes should be cataloged and identified, but they do not automatically receive their own exhaustive semantic audit unless explicitly selected.

For an already-expanded image, unavailable source-media facts remain explicitly unavailable.

### 2.1 Source identity

Derived artifacts need a robust audit identity, not merely a filename.

The long-term audit identity should bind at least:

- source/container hash or an explicit weaker identity when no container is available;
- selected image index/identity where applicable;
- raw schema version;
- collector versions/policies that affect evidence interpretation;
- preparation policy/version when the prepared tree is used.

A derived map/layout/export must refuse or invalidate incompatible source identities rather than silently reusing stale state.

Path + size + timestamp may be a useful short-term invalidation hint but is not a sufficient long-term content identity by itself.

## 3. Prepared working cache

The existing persistent cache strategy is valuable because expensive ISO/WIM preparation should happen once per source identity and selected image.

The prepared cache is an efficient **working payload view**, not automatically the complete original metadata truth.

Current preparation intentionally uses wimlib flags including:

```text
--no-acls
--no-attributes
--rpfix
```

That means the prepared tree alone cannot support claims of exact original ACL/security-descriptor fidelity, file-attribute fidelity, or every original reparse semantic.

If future coverage requires information intentionally transformed/omitted by preparation, collect it from an authoritative source representation (for example container/WIM metadata or another safe read-only mechanism) or record it as not exposed/unsupported.

Do not "fix" this by reintroducing host ACL/TrustedInstaller problems into the working cache without a deliberate architectural decision.

## 4. `image.db` — raw structural evidence database

`image.db` is the deterministic, queryable structural representation of the selected offline image and its source context.

It must remain useful without an LLM.

### 4.1 Principles

- source image remains read-only;
- stage writes and publish complete database state safely;
- preserve original-source/cache provenance;
- exact collector versions/status/coverage accounting;
- failures/unsupported/redactions are first-class data;
- compact domain-specific tables when generic graph duplication is expensive;
- stable/canonical identities in addition to local integer row IDs;
- streaming/bounded collection;
- no execution/registration of image payloads;
- long-path-safe and reparse-safe traversal;
- schema/version compatibility is explicit.

### 4.2 Do not blindly generalize legacy generic tables

The current database foundation still contains generic concepts such as objects/properties/edges/file metadata while optimized registry ingestion uses dedicated registry tables.

Before adding whole-image collectors, determine which generic tables are active, reusable, legacy or transitional.

Do not duplicate millions of raw domain rows into a generic graph solely for conceptual neatness. Prefer dedicated compact tables for high-volume data and build cross-source relationship indexes only where useful.

### 4.3 Foundational inventory first

The likely high-leverage foundation for whole-image work is a single canonical namespace/filesystem inventory that later collectors can reference rather than repeatedly walking the entire tree.

A filesystem/source object record should preserve, where exposed:

```text
stable/local identity
image-relative path
original path spelling
normalized comparison key
type (file/directory/reparse/etc.)
size
timestamps
attributes/security metadata source + availability
reparse tag/target metadata where safe and exposed
hardlink/file identity where exposed
source/cache provenance
parse/discovery state
```

Do not blindly follow reparse points. Record the reparse object itself and resolve relationships only with safe bounded logic.

Later PE/manifest/task/INF/AppX/etc. collectors should consume the indexed inventory instead of launching independent global recursive scans when the same discovery work can be reused.

### 4.4 Collector architecture

Collectors should be independent enough that one unsupported domain does not destroy unrelated evidence, but coordinated enough to reuse foundational discovery.

Each collector should expose at least:

```text
collector identity/version
input/audit identity
start/end/checkpoint state
discovered object count
represented object count
parsed count
partial/unsupported/failed/redacted/unresolved counts
coverage invariant
error records
provenance
```

Suggested collector states:

```text
PENDING
RUNNING
COMPLETE
COMPLETE_WITH_GAPS
FAILED
STALLED
```

An object-level state may include:

```text
PRESENT
NOT_PRESENT
QUERY_FAILED
ACCESS_DENIED
NOT_EXPOSED
UNSUPPORTED_FORMAT
PARSE_FAILED
PARTIALLY_PARSED
REDACTED_SENSITIVE
UNRESOLVED
```

### 4.5 Desired collector families

```text
source/media metadata
filesystem namespace + source metadata
registry
servicing/CBS
WinSxS/components
features/capabilities
AppX/MSIX/SystemApps
services/svchost
tasks
drivers/DriverStore/INF
PE metadata/signatures
COM/WinRT/shell
startup/logon configuration
boot/recovery/BCD
configuration/manifests
certificates/trust surfaces
cross-source ownership/reference/dependency edges
```

The exact implementation can combine collectors when doing so avoids duplicated work, but coverage accounting should stay understandable per evidence family.

## 5. Static-image boundary

Nylon's offline image audit represents shipped/static composition and default registrations.

Do not use the host system to fill gaps with runtime facts.

Out-of-scope as authoritative image facts include:

```text
running PIDs/process state
active TCP/UDP sockets
live WFP/firewall runtime filters
current service runtime state
live hardware/PnP enumeration
post-install user SIDs/per-user package state
activation state
runtime event history that is not actually present as image payload
host-installed VMware/OEM state
```

If a file or configuration artifact for one of these systems exists in the image, inventory the artifact. Do not infer the live state it would create after installation.

This separation is essential for later comparison of:

```text
SHIPPED_IN_IMAGE
CREATED_BY_SETUP
CREATED_AT_FIRST_BOOT
RUNTIME_ONLY
HARDWARE_SPECIALIZED
USER_CREATED
THIRD_PARTY_ADDED
REMOVED_OR_TRANSFORMED_DURING_SETUP
```

## 6. Sensitive evidence policy

Exhaustive structure does not require credential extraction.

Sensitive sources such as SAM, SECURITY, private-key stores or credential-bearing configuration require explicit handling.

Rules:

- never intentionally extract password hashes, secrets, tokens or private keys merely for completeness;
- collect safe structural metadata only when it serves the architecture;
- mark omitted sensitive payload explicitly (`REDACTED_SENSITIVE` or equivalent);
- raw sensitive blobs must not be sent to the semantic model by default;
- safe portable exports for larger/external models must exclude secrets by default;
- the coverage manifest records deliberate redaction so omissions cannot masquerade as absence.

The current registry implementation predates this complete policy and should be audited against it before whole-image export is treated as safe.

## 7. Performance model

The primary optimization rule is:

> Avoid unnecessary work rather than merely making unnecessary work faster.

Apply this consistently:

- immutable source/cache identity -> skip repeated extraction;
- foundational namespace inventory -> skip repeated global filesystem walks;
- persisted collector plans/checkpoints -> skip repeated discovery/planning;
- deterministic parsers -> skip LLM inference;
- namespace recognizers -> skip model calls for known structures;
- streaming readers -> avoid giant intermediate representations;
- batch inserts -> avoid per-row transactions;
- prepared statements/direct joins/indexes -> avoid repeated graph reconstruction;
- content hashes only where they add provenance/relationship value;
- parse each raw artifact once when possible and persist reusable facts;
- cache derived semantic/layout state and invalidate only affected branches;
- bounded worker pools/queues -> exploit CPU parallelism without unbounded RAM;
- keep expensive local model state resident across work/retries when useful;
- render/query by viewport/LOD rather than loading every object into UI widgets.

Coverage must not be reduced to achieve speed. If an artifact is not deeply parsed, it still needs an explicit represented state.

### 7.1 Resume and long-running collection

Whole-image collection can be long enough that raw scanning itself should eventually be resumable.

A resumable staging audit should:

- bind to the exact source/audit identity;
- record collector/checkpoint versions;
- skip collectors already proven complete;
- continue safe partial collectors where supported;
- invalidate only incompatible collector outputs;
- atomically publish a final audit state;
- never mix evidence from different source images.

Current all-or-nothing staging behavior may remain appropriate for short collectors, but future long collectors should not force hours of repeated work after interruption.

## 8. Parser and untrusted-input safety

Treat scanned image content as untrusted input even when the expected source is Microsoft media.

Never execute or register image payloads.

Specifically do not:

```text
LoadLibrary/execute scanned binaries
run scanned scripts
register COM servers
start/register services or drivers
run scheduled tasks
install packages/AppX
mutate BCD/registry/image state during audit
perform online certificate revocation checks
```

Parsers must use:

- bounds-checked offsets/lengths;
- explicit maximum nesting/depth/size rules;
- streaming or bounded reads for large objects;
- no external XML entities/network resolution;
- robust namespace-aware XML handling where XML is actually the format;
- explicit parse failures rather than best-effort invented facts.

Path handling must support long Windows paths and preserve evidence when names cannot be losslessly converted to the normalized text representation. Encoding failures become explicit gap records, not silently dropped files.

## 9. Schema and identity strategy

### 9.1 Canonical identities

Use stable domain identities where practical:

```text
relative filesystem path + file/hardlink identity where exposed
service name
CBS package/component full identity
feature/capability identity
AppX family/full name
scheduled task path
CLSID/AppID/IID/TypeLib IDs
INF/DriverStore package identity
BCD object identity
certificate thumbprint where appropriate
```

Preserve original values plus normalized comparison forms. Avoid embedding host-specific absolute cache paths into canonical identities.

### 9.2 Local IDs versus stable IDs

SQLite integer row IDs are efficient local references. They are not sufficient by themselves for:

- stable exports;
- before/after comparison;
- derived artifact binding;
- long-lived Atlas coordinates across compatible rebuilds.

Where needed, add a canonical/stable key alongside local row IDs.

### 9.3 Schema/version compatibility

Version at least:

```text
raw database format/schema
collector family/version
parser version where output semantics change
semantic map format
planner/ontology/prompt/model/request contract
cross-source linker version
Atlas layout algorithm/version
export format/version
```

A derived artifact must know exactly which input version/identity produced it.

## 10. Cross-source graph model

Do not force raw high-volume evidence into one generic graph, but provide a normalized cross-source relationship layer for high-value relationships.

Examples where evidence supports them:

```text
service -> executable
service -> ServiceDll
service -> svchost group
service -> service/group dependency
driver service -> SYS file
driver -> INF / DriverStore package
scheduled task -> executable/script
COM class -> server binary
COM class -> AppID
interface -> proxy/type library
AppX/SystemApp -> manifest/payload
CBS package -> MUM/catalog/component
component -> manifest/directory/file/registry declaration
file -> hardlink identity
file -> PE metadata
file/catalog -> signer
feature/capability -> package/component
boot entry -> boot file/device
```

Every important edge should distinguish:

```text
relationship type
source identity
target identity
evidence source
evidence class
confidence/parse state
```

Evidence classes can include:

```text
DIRECT
AUTHORITATIVE_METADATA
DERIVED
INFERRED
UNRESOLVED
```

Ownership and dependency are different graphs. Thematic subsystem classification is not ownership proof.

Reverse-dependency lookup must be efficient because later removal analysis depends heavily on answering "what depends on this?".

## 11. Servicing / WinSxS architecture

Servicing evidence is one of the most important domains for safe stripping.

Build independent evidence from multiple sources rather than treating any single API/view as complete:

```text
CBS/package state
COMPONENTS hive
Windows\servicing\Packages\*.mum
catalogs
Windows\WinSxS\Manifests
WinSxS component directories
filesystem/hardlink evidence
feature/capability relationships
```

A desired evidence chain is:

```text
CBS package
 -> MUM/catalog
 -> component identity
 -> servicing manifest
 -> component directory
 -> payload files/registry declarations
```

Do not infer package ownership from names alone.

### 11.1 Binary servicing-manifest risk

Historical work on the target Windows 11 family observed a large population of WinSxS `.manifest` files that were DCM/PA30/binary servicing data rather than ordinary XML. An XML-only mapper decoded only a small fraction.

Therefore:

- format-detect before XML parsing;
- retain/hash/inventory unsupported binary manifests;
- record exact parse state;
- do not mark components absent because their manifest could not be decoded;
- do not authorize forced residual deletion based on incomplete servicing ownership.

See `docs/HISTORICAL_AUDIT_NOTES.md`.

## 12. Registry architecture

The existing direct binary REGF parser is a valuable working subsystem.

Whole-image evolution should preserve its strengths:

- direct offline read-only parsing;
- no host `reg load` mutation;
- iterative/bounded traversal;
- raw value bytes plus decoded text when safe/valid;
- long-path-aware hive discovery;
- explicit dirty/invalid hive failure rather than silently interpreting inconsistent data;
- batched dedicated tables.

Future work should distinguish:

- required stock hives;
- optional/user/service-profile/BCD/Amcache hives;
- globally discovered REGF candidates;
- sensitive hives/values subject to redaction policy;
- dedicated domain interpretation such as BCD rather than treating every hive only as generic registry semantics.

Offline control-set consumers must resolve actual `ControlSet00X` state from `Select` evidence rather than assuming a live `CurrentControlSet` alias.

## 13. Semantic lifting — `image.map.db`

`image.map.db` is derived/reproducible. `image.db` stays immutable.

### 13.1 Semantic pipeline

```text
image.db
   -> source/domain work planner
   -> deterministic parsers/recognizers/preclassification
   -> bounded ambiguous work units only
   -> local small model
   -> strict schema + allowed-source validation
   -> semantic nodes/edges/source links
   -> deterministic cross-source/hierarchy reconstruction
```

### 13.2 Zero-token path

A zero-token path is any work unit normalized without an LLM because deterministic evidence already establishes its structural meaning.

Zero-token must never mean `drop the evidence`.

Raw source objects remain represented and linked. Zero-token optimization affects inference cost, not source accounting.

### 13.3 Semantic accounting versus semantic understanding

These must remain separate metrics.

A semantic source link can prove that a raw object is assigned to a semantic group/work unit. It does **not** prove that:

- every byte was exposed to the model;
- every field was decoded;
- the model label is correct;
- a neutral deterministic label has high semantic specificity.

Track unresolved/unknown/neutral classifications separately so `100% source coverage` is never advertised as `100% semantic understanding`.

### 13.4 Model role

The model should return compact grounded semantics, not graph bookkeeping.

Prefer deterministic code for:

```text
IDs
containment edges
exact source ownership
known Windows namespace classification
hierarchy reconstruction
source/reference validation
```

Use the model only for bounded ambiguity that benefits from language understanding.

Never send sensitive credential-bearing blobs merely to improve semantic labeling.

### 13.5 Derived-map binding

The current implementation uses a source fingerprint and semantic version fields. Long term, bind the map to the robust audit identity described earlier rather than relying on path/size/timestamp as the primary identity.

Semantic compatibility must include all versions whose changes can alter committed meaning.

## 14. Atlas native viewer

The final Atlas viewer is a native Windows application, not a website.

### 14.1 Technology

```text
C++20
Win32             window/input/lifecycle
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

The existing browser viewer is transitional/prototype code and may remain useful only while native Atlas is built.

### 14.2 Core UI principle

Atlas represents the whole collected Windows image in one navigable conceptual space.

There is no arbitrary node/data cap.

Performance comes from representation and query scheduling:

```text
hierarchy
spatial indexing
viewport culling
level-of-detail
pagination/batching
label suppression at low zoom
GPU instancing/batching
lazy inspector/raw-evidence queries
```

All underlying evidence remains searchable/reachable even when it is not simultaneously rendered.

### 14.3 Stable deterministic layout

Do not use continuous force-directed physics for Atlas.

Layout should be deterministic, persistable and versioned. The same semantic component should remain in the same place across sessions when its relevant source hierarchy has not changed.

A disposable derived layout cache may contain fields such as:

```text
source/map identity
layout algorithm version
semantic stable ID
layout level
x/y/width/height
cluster/parent
bounds
```

Recompute only invalidated branches when practical.

Do not store presentation-only state in `image.db` or mutate semantic evidence merely to support the UI.

### 14.4 Spatial/LOD pipeline

```text
image.map.db + image.db
   -> semantic/hierarchy index
   -> persistent layout coordinates
   -> spatial index
   -> viewport query
   -> LOD selection
   -> compact GPU instance buffers
   -> D3D11 draw
```

Do not create one heavyweight Win32/UI object per database row.

### 14.5 Rendering lifecycle

No perpetual render loop is required.

Render on meaningful state changes:

```text
pan
zoom
selection
resize
semantic progress/update
short intentional transition/animation
```

Then sleep.

Idle CPU/GPU use should be near-zero apart from OS/window bookkeeping.

### 14.6 Concurrent semantic progress

The native viewer should be able to open while semantic lifting is in progress without repeatedly rescanning/retransmitting the entire map.

Use a cheap version/change-sequence or incremental status mechanism so the viewer can query only changes after initial load.

The semantic writer remains authoritative; the viewer stays read-only.

### 14.7 Native robustness

The viewer architecture must account for ordinary Windows-native concerns without adopting a heavyweight framework:

- per-monitor DPI/scaling;
- keyboard focus/text input;
- resize/swap-chain recreation;
- D3D device-loss recovery;
- WARP/software fallback when useful;
- DirectWrite label measurement/caching;
- accessible predictable keyboard navigation for core actions;
- no dependency on continuous GPU availability for access to raw evidence.

### 14.8 Interaction model

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

### 14.9 Inspector

Selection should expose readable semantics plus provenance and direct access to raw evidence.

A user should be able to move from:

```text
component
 -> package/service/task/file relationship
 -> raw file/registry/task/manifest evidence
```

without losing the Atlas mental map.

## 15. Whole-image hierarchy

The top-level Atlas is generic, not registry-centric.

A conceptual starting shape is:

```text
Windows Image
├── Servicing
├── Runtime Configuration
├── Applications
├── Drivers / Devices
├── Filesystem / Payload
├── Configuration
├── Security / Trust
├── Networking
├── Boot / Recovery
├── Users / Profiles
└── Raw Evidence / Gaps
```

Exact hierarchy should be evidence-driven and may evolve. Registry is an evidence layer rather than the root definition of the product.

## 16. Export for larger-model reasoning

The final map should support a compact, deterministic, safe portable export that is substantially more readable than the raw image dump while retaining stable references back to raw evidence.

The export should emphasize:

- semantic entities/hierarchy;
- ownership/dependencies/reverse dependencies;
- evidence source/class/confidence;
- stable raw references;
- coverage/gap state;
- unsupported/failed parses;
- deliberate sensitive-data redactions;
- source/audit identity;
- collector/schema/parser/model versions.

Do not force a larger model to ingest millions of redundant raw rows when deterministic preprocessing can normalize them first, but do not hide unresolved evidence merely to make the export smaller.

Raw databases remain available for follow-up queries.

## 17. Modification/verification architecture

Modification is a separate future phase and must not leak destructive behavior into the collector/mapper.

The removal planner should distinguish:

```text
supported servicing/package removal
AppX/provisioning removal
service/task/registration cleanup
configuration cleanup
forced residual filesystem/registry cleanup
```

Forced residual cleanup requires stronger evidence than naming/proximity and should be blocked when servicing ownership or retained reverse-dependencies are unresolved.

After a modified image is produced:

1. run Nylon again against the modified image;
2. compare canonical identities/coverage with the source audit;
3. verify expected removals;
4. flag unexpected removals/new unresolved gaps;
5. validate the explicit retained-function contract using appropriate offline and later runtime tests.

A smaller WIM/ISO is not itself a success criterion.

## 18. Validation and CI semantics

Validation claims must match the evidence.

### Compilation

Proves the tested translation units compile/link under the tested toolchain.

### Synthetic smoke CI

Proves only the synthetic fixtures/invariants exercised by the workflow.

A fake OpenAI-compatible server does not prove:

- actual llama.cpp structured-output grammar behavior;
- semantic quality of Qwen output;
- real Vulkan/GPU throughput;
- target-image completion.

### Real-image validation

Required to prove actual collector behavior/performance on the selected image.

### Real-hardware validation

Required for claims about GPU/runtime/device behavior.

Benchmark meaningful stages on representative images and record counts/wall-time/peak memory where useful. Do not infer performance solely from synthetic fixtures.

## 19. Current active build/code reality

As of this architecture document:

- `build.cmd` is the primary portable scanner build path;
- `build-semantic.cmd` builds the active optimized semantic runtime from `semantic/semantic_v4.cpp` plus its current classifier/linker composition;
- older semantic translation units remain in the repository and must not be assumed active solely from filename;
- `build-viewer.cmd` currently generates/builds the transitional browser Atlas composition from the viewer wrapper sources;
- ISO/selected-image preparation and persistent cache exist;
- direct offline registry collection is deeply implemented and optimized;
- registry semantic planning/lifting and imported-registry source accounting exist;
- current web Atlas/progress UI is transitional and registry-work-unit-centric;
- full filesystem/PE/servicing/task/AppX/etc. collection has not yet been implemented as complete first-class collectors;
- native Atlas has not yet replaced the web viewer.

Agents must preserve these distinctions in documentation, UI labels and completion claims.

## 20. Historical audit knowledge

`docs/HISTORICAL_AUDIT_NOTES.md` records valuable source locations, evidence relationships, prior collector lessons and known failures from the predecessor audit work.

Use it as design input, especially to avoid repeating known parser mistakes, but verify every new implementation against current source media and authoritative documentation where needed.

Historical counts/results are not current Nylon coverage claims.

## 21. Definition of done for the product vision

The product vision is not complete until:

1. the exact source/selected image identity is recorded robustly;
2. the selected Windows image is exhaustively inventoried across required collector families;
3. source metadata lost/transformed by the prepared cache is either collected authoritatively or explicitly marked unavailable;
4. every collector has discovery + representation accounting and explicit gap/redaction states;
5. cross-source ownership/reference/dependency relationships are reconstructed with provenance/evidence class;
6. semantic lifting covers the full collected image, not only registry evidence;
7. semantic source accounting is distinguished from semantic resolution quality;
8. sensitive credential/secret material is not unnecessarily collected or exported;
9. Atlas can navigate/query the full semantic/raw image without arbitrary data limits;
10. the native viewer is stable, deterministic, performant and largely idle when not interacting;
11. a safe portable readable export can be handed to a larger reasoning model with coverage gaps preserved;
12. long-running collection/lifting can resume without repeating expensive compatible work;
13. modification work can trace removal decisions back to evidence and retained reverse-dependencies;
14. the modified image is re-scanned and compared against the source audit and retained-function contract.
