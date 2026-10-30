# Nylon product goal

## Mission

Nylon exists to create an exhaustive, evidence-backed structural dump of an offline Windows image, normalize that dump into a readable semantic map with a small local model, and expose the complete result through an unrestricted native Atlas viewer.

The final downstream use is deeper analysis by a larger model to design and verify an aggressively stripped Windows image while retaining only explicitly chosen functionality.

## Canonical pipeline

```text
Windows ISO / expanded image
    |
    v
Immutable prepared image cache
    |
    v
Nylon exhaustive collectors
    |
    v
image.db
raw structural truth
    |
    v
small local semantic lifter
    |
    v
image.map.db
readable normalized whole-image map
    |
    v
Atlas native viewer
full controllable visual exploration
    |
    v
portable readable export / evidence bundle
    |
    v
larger reasoning model
    |
    v
dependency/removal plan
    |
    v
stripped-image implementation + verification
```

## What `image.db` should eventually represent

`image.db` is not intended to store duplicate copies of all Windows payload bytes. The immutable expanded image cache already contains those bytes. `image.db` should contain exhaustive structural facts, identities, metadata, provenance, and relationships, with stable references back to payload files where appropriate.

The full collector set should cover, as applicable to the exact image:

- image / edition / WIM metadata
- global filesystem inventory
- file identities, sizes, timestamps and useful attributes
- hardlink identities and relationships where obtainable
- registry hives, keys and values
- CBS packages and servicing metadata
- COMPONENTS/CBS registry evidence
- MUM and catalog metadata
- WinSxS component directories and manifests, including non-XML servicing formats when decoded
- optional features
- Windows capabilities / FoDs
- AppX / MSIX / provisioned packages
- SystemApps and package manifests
- services, drivers and svchost group relationships
- scheduled tasks and task XML
- DriverStore, INF packages and device/driver relationships
- PE files (EXE/DLL/SYS and other PE types) and useful static metadata
- Authenticode/signature metadata where statically available
- COM, AppID, interfaces, TypeLib, WinRT and shell integrations
- startup/logon mechanisms
- boot, BCD and recovery configuration
- high-signal text/configuration formats (XML, JSON, INI, MOF, INF, manifests, policy/config files, setup/unattend data, etc.)
- certificates/trust configuration where represented in the offline image
- security/network/update/diagnostics/telemetry configuration surfaces
- cross-source ownership, dependency and reference relationships
- explicit records of parse failures, unsupported formats, missing sources and contradictions

This list is a minimum architectural target, not permission to ignore other high-value evidence discovered in the image.

## Three layers of truth

### 1. Payload truth

The immutable expanded Windows image cache contains the actual selected-image payload.

### 2. Structural truth — `image.db`

Deterministic collectors record what exists and how it is structured. This layer must remain useful without an LLM.

### 3. Semantic truth — `image.map.db`

The small local model and deterministic recognizers convert raw facts into readable semantic entities while preserving exact source references and uncertainty.

The semantic layer must never overwrite or replace raw evidence.

## Coverage definition

"100%" is not one vague global number.

Each collector must maintain a measurable accounting invariant. Examples:

- filesystem: every enumerated object accounted for
- registry: every imported key/value represented
- PE: every identified PE file either parsed or explicitly marked failed/unsupported
- task collector: every discovered task file represented
- servicing: every discovered package/manifest/catalog represented, including explicit unsupported-format states

A whole-image completion state may only be reported when all required collectors have either:

1. completed their coverage invariant, or
2. recorded an explicit unresolved/unsupported/failure state for every remaining discovered object.

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
UNRESOLVED
```

Never infer `NOT_PRESENT` from a failed or unsupported query.

## Relationship confidence

Ownership/dependency/reference edges should preserve evidence class where useful, for example:

```text
DIRECT
AUTHORITATIVE_METADATA
DERIVED
INFERRED
UNRESOLVED
```

An inferred name resemblance must never be presented as equivalent to authoritative package/component ownership.

## Role of the small model

The local small model exists to make deterministic evidence readable and navigable. It should perform bounded semantic lifting such as:

- neutral classification
- concise naming
- grouping
- summarization
- grounded entity extraction

It should not be asked to rediscover facts that deterministic code already knows, manufacture graph bookkeeping, or decide what may safely be removed from Windows.

The preferred performance path is zero-token deterministic normalization wherever authoritative structure is sufficient, with model inference reserved for genuinely ambiguous evidence.

## Role of the larger model

The later larger model consumes the cleaned semantic map plus provenance and performs expensive reasoning about:

- component purpose
- dependency and reverse-dependency structure
- shared vs exclusive ownership
- boot/logon/shell/network/servicing relevance
- supported vs forced removal paths
- retained-function requirements
- risk/confidence
- removal ordering

The larger model must be able to trace important conclusions back into raw Nylon evidence.

## Final stripped-image objective

The eventual modification stage should produce the smallest practical Windows image consistent with an explicit retained-function contract.

Removal should proceed from dependency and ownership evidence rather than arbitrary debloat lists. Supported servicing/package removal should be distinguished from residual cleanup, and direct payload deletion should require stronger ownership evidence than simple naming or proximity.

Every modification should be reproducible and followed by verification against the retained-function contract.
