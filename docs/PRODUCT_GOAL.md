# Nylon product goal

## Mission

Nylon exists to create an exhaustive, evidence-backed structural account of a selected offline Windows image, normalize that evidence into a readable derived semantic map with deterministic logic plus a small local model, and expose the complete collected result through an unrestricted native Atlas viewer.

The final downstream use is deeper analysis by a larger model to design and verify an aggressively stripped Windows image while retaining only explicitly chosen functionality.

The word **exhaustive** refers to evidence accounting: every discovered in-scope artifact must be represented as successfully parsed, deliberately redacted, unsupported, failed, unresolved, or otherwise explicitly accounted for. It does not mean Nylon may claim semantic understanding where evidence or parsers are incomplete.

## Canonical pipeline

```text
Original ISO / WIM / ESD / SWM / expanded image
    |
    +--> source/media identity + metadata
    |
    v
Selected Windows image
    |
    v
Immutable prepared working cache
    |
    v
Nylon exhaustive collectors
    |
    v
image.db
raw structural evidence + provenance + coverage state
    |
    v
deterministic normalization + small local semantic lifter
    |
    v
image.map.db
derived readable semantic interpretation + exact source links
    |
    v
Atlas native viewer
full controllable visual exploration
    |
    v
safe portable readable export + coverage/gap manifest
    |
    v
larger reasoning model
    |
    v
dependency/removal plan
    |
    v
stripped-image implementation
    |
    v
re-scan + retained-function verification + source/target diff
```

## Static-image scope

Nylon's primary image audit answers:

> What did this exact selected Windows image ship, how is it structured, and what evidence connects its components?

It is not a live-machine inventory tool.

Do not use host/runtime state as evidence about the offline image. Current PIDs, active sockets, running service state, live PnP devices, current firewall/WFP runtime filters, post-install user state, activation state and similar runtime facts belong to a separate runtime baseline if one is ever needed.

Files that physically exist in the offline image remain filesystem evidence, but their existence must not be misrepresented as proof of live runtime behavior.

## Source-media scope

For ISO/container input, Nylon should preserve enough source-media evidence to bind the audit to the exact media and understand the selected image in context:

- ISO/container SHA-256 and relevant identity
- install container identity (WIM/ESD/SWM) and hash where practical
- catalog metadata for available images/indexes
- selected image index/name/edition/build/architecture/language evidence
- useful ISO/setup/boot/recovery structure and metadata where it affects understanding or reconstruction of the final media

The **selected Windows image** receives exhaustive deep collection. Other WIM indexes are cataloged/identified unless the user explicitly selects them for their own full audit. Nylon must not silently multiply scope by fully expanding every index.

For an already-expanded image, unavailable source-media facts must be represented as unavailable rather than inferred.

## Prepared cache is a working view, not the original truth

The immutable expanded cache exists to make repeated local inspection fast and safe.

It is not automatically a lossless representation of every source metadata field. The current preparation path intentionally uses wimlib options including:

```text
--no-acls --no-attributes --rpfix
```

Therefore ACL/security-descriptor fidelity, original file attributes and some reparse semantics cannot be claimed solely from the prepared tree. Whole-image coverage that requires those fields must obtain them from an authoritative source representation or explicitly record that they are not exposed/collected.

The original source/media remains the ultimate provenance anchor; the cache is the efficient working payload view.

## What `image.db` should eventually represent

`image.db` is not intended to store duplicate copies of all Windows payload bytes. The source media and immutable prepared cache already contain the payload.

`image.db` should contain exhaustive structural facts, identities, metadata, parse state, provenance, coverage state and relationships, with stable references back to source/cache artifacts where appropriate.

The collector set should cover, as applicable to the exact selected image:

- source media / image / edition / WIM metadata
- global filesystem namespace inventory
- files, directories, reparse entries and other discovered filesystem objects without blindly following reparses
- useful file metadata exposed by the authoritative source (size, timestamps, attributes/security metadata where obtainable, file identity/hardlink relationships where obtainable)
- registry hives, keys and values subject to the sensitive-data policy
- CBS packages and servicing metadata
- COMPONENTS/CBS registry evidence
- MUM and catalog metadata
- WinSxS component directories and manifests, including explicit non-XML/unsupported servicing formats until decoded
- optional features
- Windows capabilities / FoDs
- AppX / MSIX / provisioned packages
- SystemApps and package manifests
- services, driver services and svchost group relationships
- scheduled tasks and task definitions
- DriverStore, INF packages and device/driver declarations present in the image
- PE files (EXE/DLL/SYS and other PE types) and useful static metadata
- Authenticode/catalog/signature metadata where statically available without online trust assumptions
- COM, AppID, interfaces, TypeLib, WinRT and shell integrations
- startup/logon configuration shipped in the image
- boot, BCD and recovery configuration
- high-signal text/configuration formats (XML, JSON, INI, MOF, INF, manifests, policy/config files, setup/unattend data, etc.)
- certificates/trust configuration represented in the offline image, without exporting private key material
- security/network/update/diagnostics/telemetry configuration surfaces
- cross-source ownership, dependency and reference relationships
- explicit records of parse failures, unsupported formats, missing sources, deliberate redactions and contradictions

This list is a minimum architectural target, not permission to ignore other high-value static evidence discovered in the image.

## Evidence layers

### 1. Original-source evidence

The ISO/WIM/ESD/SWM or supplied expanded image is the provenance anchor. Record identities/hashes sufficient to establish which source was audited.

### 2. Prepared working view

The immutable expanded cache is the efficient local payload view used by collectors. Its preparation policy and known fidelity losses must be recorded.

### 3. Structural evidence — `image.db`

Deterministic collectors record what was discovered, what was parsed, what failed, how objects relate, and where every fact came from. This layer must remain useful without an LLM.

### 4. Derived semantic interpretation — `image.map.db`

Deterministic recognizers and the small local model convert raw facts into readable semantic entities while preserving exact source references, confidence and uncertainty.

This layer is derived interpretation, not infallible truth. It must never overwrite or replace raw evidence.

### 5. Atlas representation

Atlas displays and navigates the evidence/model. Rendering choices do not define or reduce collection coverage.

## Coverage model

`100%` is not one vague global number.

Track at least these distinct concepts:

1. **Discovery coverage** — did the collector enumerate its entire authoritative namespace/source?
2. **Representation coverage** — is every discovered object recorded in `image.db`, including explicit failures/unsupported states?
3. **Parse coverage** — how many represented objects were deeply decoded versus unsupported/failed/partially parsed?
4. **Relationship coverage** — which ownership/reference/dependency edges are authoritative, derived, inferred or unresolved?
5. **Semantic source accounting** — is every raw source assigned/referenced by the semantic work system?
6. **Semantic resolution quality** — how much evidence received useful specific semantics versus neutral/unknown grouping?
7. **Viewer reachability** — can every represented object be reached by query/search/drill-down even when LOD/pagination avoids drawing it simultaneously?

Examples of collector invariants:

- filesystem: every enumerated namespace entry accounted for, with reparses recorded but not blindly followed
- registry: every intentionally in-scope discovered key/value represented or deliberately redacted with reason
- PE: every identified PE either parsed or explicitly marked failed/unsupported
- task collector: every discovered task definition represented
- servicing: every discovered package/manifest/catalog represented, including explicit unsupported-format states
- semantic layer: every in-scope raw source represented by semantic ownership/accounting, separately from semantic specificity

A whole-image completion state may only be reported when all required collector families have either:

1. completed their discovery/representation invariant, or
2. recorded an explicit unresolved/unsupported/failure/redaction state for every remaining discovered object.

Unknown is acceptable. Silent omission is not.

## Evidence states

Use explicit states such as:

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

Never infer `NOT_PRESENT` from a failed, inaccessible or unsupported query.

## Relationship confidence

Ownership/dependency/reference edges should preserve evidence class where useful, for example:

```text
DIRECT
AUTHORITATIVE_METADATA
DERIVED
INFERRED
UNRESOLVED
```

An inferred filename resemblance, directory proximity, thematic family classification or hardlink association must never be presented as equivalent to authoritative servicing/package ownership.

Ownership and dependency are distinct relationship types. A file can be owned by one component while being depended on by many others.

## Stable identity and provenance

The final evidence model must support exact source binding and later before/after comparison.

Where practical, use canonical identities such as:

- image-relative normalized path plus preserved original spelling
- service name
- package/component full identity
- task path
- AppX package family/full name
- CLSID/AppID/IID/TypeLib identity
- INF/package identity
- stable file/hardlink identity where exposed

Local SQLite row IDs are implementation details and must not be the only long-term identity for evidence that must survive schema/layout changes or participate in comparisons.

`image.map.db`, Atlas layout caches and exports must record which exact `image.db`/audit identity and schema/collector versions they were derived from. A derived artifact must not silently attach to a different audit because its pathname happens to match.

## Sensitive-data policy

Exhaustive structural accounting does not require collecting credentials or secrets.

Nylon must not intentionally extract or export:

- password hashes
- authentication secrets/tokens
- private keys
- credential material
- unrelated personal secrets

Sensitive hives/areas such as SAM and SECURITY require an explicit policy. Structural presence and safe metadata may be recorded, but credential-bearing values should be redacted/omitted and the omission represented explicitly rather than silently ignored.

The semantic model should not receive sensitive raw material merely because it exists locally.

The portable export intended for a larger/external model must be safe by default: sensitive raw blobs are excluded, while the export records that redaction occurred so coverage is not falsely overstated.

## Role of the small model

The local small model exists to make deterministic evidence readable and navigable. It should perform bounded semantic lifting such as:

- neutral classification
- concise naming
- grouping
- summarization
- grounded entity extraction

It should not be asked to rediscover facts that deterministic code already knows, manufacture graph bookkeeping, or decide what may safely be removed from Windows.

The preferred performance path is zero-token deterministic normalization wherever authoritative structure is sufficient, with model inference reserved for genuinely ambiguous evidence.

Zero-token means no LLM call, not no evidence. Source coverage and semantic specificity remain separate metrics.

## Role of the larger model

The later larger model consumes the cleaned semantic map plus provenance, coverage gaps and selected raw evidence. It performs expensive reasoning about:

- component purpose
- dependency and reverse-dependency structure
- shared vs exclusive ownership
- boot/logon/shell/network/servicing relevance
- supported vs forced removal paths
- retained-function requirements
- risk/confidence
- removal ordering

The larger model must be able to trace important conclusions back into raw Nylon evidence and must see unresolved/gap information instead of being handed a falsely complete narrative.

## Portable export objective

The export should be much smaller and more readable than a literal dump of millions of low-level rows, without throwing evidence away.

It should favor deterministic machine-readable forms such as normalized JSON/JSONL/tables plus a human-readable summary, with:

- semantic entities
- hierarchy
- ownership/reference/dependency edges
- reverse dependencies
- evidence class/confidence
- stable source references
- coverage manifest
- parser failures/unsupported areas
- deliberate redactions
- source/audit identity and tool/schema/model versions

The raw databases remain authoritative and queryable behind the compact export.

## Performance objective

The fastest correct architecture is the one that avoids unnecessary work.

Prefer:

- one foundational filesystem/source inventory feeding later collectors
- streaming traversal and bounded memory
- persistent facts and resumable collector checkpoints
- deterministic parsing before model inference
- selective/relationship-driven hashing rather than indiscriminate payload hashing
- compact domain-specific tables rather than duplicating everything into a generic graph
- incremental derived-state invalidation
- no repeated full-image scans when an indexed fact already exists

Speed must never be achieved by silently excluding objects.

## Final stripped-image objective

The eventual modification stage should produce the smallest practical Windows image consistent with an explicit retained-function contract.

Removal should proceed from dependency and ownership evidence rather than arbitrary debloat lists. Supported servicing/package removal should be distinguished from residual cleanup, and direct payload deletion should require stronger ownership evidence than simple naming or proximity.

The source audit and the stripped image must be comparable. Every modification should be reproducible and followed by a fresh Nylon scan/semantic pass and verification against:

- the retained-function contract
- expected removals
- unexpected removals
- unresolved dependency/ownership changes
- boot/servicing integrity evidence that can be checked offline

Creation of a smaller image is not itself proof of correctness.