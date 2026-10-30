# Historical audit notes

This file preserves useful engineering evidence from the predecessor Windows-image audit work that informed Nylon.

It is **not** a statement of current Nylon implementation or current-image coverage.

Historical paths, counts and parser observations must be revalidated against the exact image Nylon is scanning. The purpose of this file is to keep known-good investigation surfaces and known failed approaches from being forgotten when development moves between agents/tools.

## 1. Historical target context

The predecessor image-audit work focused on Windows 11 IoT Enterprise LTSC 2024 / `IoTEnterpriseS` in the Windows 11 26100 family. One historical baseline used build `26100.1742`.

Do not assume a current source image has the same UBR/build solely because the edition name matches. Verify identity from the source container plus offline image evidence.

Useful identity evidence included:

```text
Windows\System32\Config\SOFTWARE
  Microsoft\Windows NT\CurrentVersion\EditionID
  Microsoft\Windows NT\CurrentVersion\CurrentBuild
  Microsoft\Windows NT\CurrentVersion\UBR
```

Also preserve source/container image catalog metadata, architecture and language evidence.

## 2. Evidence philosophy that worked

The old work established several principles that should remain:

- no single source such as DISM is the sole truth;
- preserve raw evidence independently from normalized interpretation;
- a failed/unsupported query is not `NOT_PRESENT`;
- ownership edges carry evidence source/class/confidence;
- thematic classification is not ownership proof;
- physical hardlink/file ownership is not automatically proof that a component is exclusively removable;
- static image composition and live runtime state are separate datasets;
- optimize with streaming/bounded buffers rather than giant in-memory graphs;
- stable compare-friendly identifiers are valuable for later source-vs-runtime/source-vs-stripped diffs.

Historical evidence states included concepts equivalent to:

```text
PRESENT
NOT_PRESENT
QUERY_FAILED
ACCESS_DENIED
NOT_EXPOSED
UNRESOLVED
```

Nylon extends this with explicit parse/unsupported/redaction states.

Historical relationship classes included:

```text
DIRECT
AUTHORITATIVE_METADATA
DERIVED
INFERRED
UNRESOLVED
```

## 3. Known useful source locations

These locations repeatedly produced high-value static evidence in the predecessor audit. They are discovery hints, not a substitute for global filesystem inventory.

### Registry / identity

```text
Windows\System32\Config\SOFTWARE
Windows\System32\Config\SYSTEM
Windows\System32\Config\COMPONENTS
Windows\System32\Config\DEFAULT
```

The historical design deliberately avoided credential collection from SAM/SECURITY. Nylon's current registry collector has different behavior, so sensitive-hive handling requires an explicit reviewed policy.

Offline control-set consumers should inspect:

```text
SYSTEM\Select\Default
SYSTEM\Select\Current
SYSTEM\Select\LastKnownGood
```

where present and resolve actual `ControlSet00X` paths. Do not assume the live `CurrentControlSet` alias exists in an offline hive.

### Services / svchost

Service and driver-service state is rooted in the selected offline control set under `Services`.

Useful fields included:

```text
DisplayName
ImagePath
Start
Type
ErrorControl
Group
ObjectName
DependOnService
DependOnGroup
RequiredPrivileges
ServiceSidType
FailureActions
TriggerInfo
Parameters\ServiceDll
Parameters\ServiceMain
```

Svchost grouping evidence included:

```text
SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost
```

Important relationship examples:

```text
service -> ImagePath executable
service -> ServiceDll
service -> svchost group
service -> service/group dependency
```

### CBS / servicing

High-value sources included:

```text
Windows\servicing\Packages\*.mum
Windows\servicing\Packages\*.cat
Windows\WinSxS\Manifests
Windows\WinSxS\Catalogs
Windows\WinSxS
COMPONENTS hive
CBS/package registry state
```

A desired evidence chain was:

```text
CBS package
 -> MUM/catalog
 -> component identity
 -> component/servicing manifest
 -> WinSxS component directory
 -> payload files / registry declarations
```

Names alone were explicitly considered insufficient ownership proof.

### AppX / SystemApps

Useful locations/evidence included:

```text
Windows\SystemApps
Program Files\WindowsApps
ProgramData\Microsoft\Windows\AppRepository
AppxManifest.xml
provisioned-package metadata
```

Important manifest data included identities, dependencies, applications/executables, extensions, protocols, COM/WinRT declarations, background tasks and file associations.

SystemApp directory names should be bound to package identity only when manifest/provisioning evidence supports the relationship.

### Scheduled tasks

Stock task files were successfully treated as offline files rather than through live Task Scheduler COM:

```text
Windows\System32\Tasks
```

Useful fields included task path/URI, principal, logon/run level, triggers, actions, executable, arguments, working directory, conditions and settings.

### COM / AppID / interfaces / TypeLib

Useful offline SOFTWARE locations included:

```text
SOFTWARE\Classes\CLSID
SOFTWARE\Classes\AppID
SOFTWARE\Classes\Interface
SOFTWARE\Classes\TypeLib
SOFTWARE\WOW6432Node\Classes\CLSID
SOFTWARE\WOW6432Node\Classes\AppID
SOFTWARE\WOW6432Node\Classes\Interface
SOFTWARE\WOW6432Node\Classes\TypeLib
```

High-value relationships included:

```text
CLSID -> InprocServer32 DLL
CLSID -> LocalServer32 EXE
CLSID -> AppID
AppID -> LocalService
Interface -> ProxyStub CLSID
Interface -> TypeLib
```

Then cross-reference backing files to filesystem/WinSxS/component/package/service evidence.

### Drivers / INF / DriverStore

Useful locations included:

```text
Windows\System32\DriverStore\FileRepository
Windows\INF
Windows\System32\drivers
```

Useful INF/package relationships included provider/class/ClassGuid/DriverVer/catalog/service/hardware IDs/compatible IDs/CopyFiles/AddService/binary paths.

A desired relationship chain was:

```text
INF
 -> driver service
 -> SYS binary
 -> DriverStore directory
 -> catalog
 -> servicing/component evidence where available
```

### Filesystem

High-value roots included, but were not intended as a replacement for a complete namespace inventory:

```text
Windows\System32
Windows\SysWOW64
Windows\SystemApps
Windows\servicing
Windows\WinSxS
Windows\INF
Windows\System32\DriverStore
Windows\System32\drivers
Windows\System32\Tasks
Program Files
Program Files (x86)
ProgramData
Users\Default (where useful/safe)
```

The prior design explicitly required:

- do not blindly follow reparse points;
- record reparse entries distinctly;
- use long-path-safe handling;
- capture path/type/size/timestamps/attributes where authoritative;
- preserve version/company/product/description/original filename metadata where useful;
- do not copy all Windows payload bytes into the evidence output.

### Hashing strategy

The prior audit favored hashing high-value metadata/relationship participants rather than blindly hashing every payload.

High-value hash candidates included:

```text
MUM
CAT
WinSxS manifests
INF
AppxManifest.xml
task definitions
important hive files
service binaries / ServiceDlls
task executables
COM/WinRT servers
driver binaries
SystemApp/AppX executables involved in relationships
```

### PE metadata / signatures

Useful static PE facts included:

```text
machine architecture
PE subtype/subsystem
timestamp/header metadata
version information
company/product/description/original filename
imports/delay imports where useful
embedded manifest presence
hash
path
```

Do not load or execute the binaries to inspect them.

For Authenticode/catalog evidence, the prior design required offline-only behavior: no CRL/OCSP/network revocation traffic. Lack of online revocation data must not be mislabeled as an invalid signature.

## 4. Most important known parser failure: WinSxS DCM/PA30

The historical analysis counted approximately:

```text
WinSxS manifests discovered: 18,590
ordinary XML successfully parsed: 389
binary/non-XML DCM/PA30-like population: approximately 18,201
```

Those numbers belong to the historical build and are not current Nylon counts.

The critical lesson is architectural:

> A `.manifest` extension does not prove the file is ordinary XML.

The previous hierarchy mapper attempted XML parsing across the population and therefore recovered only a small fraction of modern component declarations.

Future Nylon servicing work must:

- format-detect first;
- preserve/inventory/hash unsupported manifests;
- record parse state;
- research/decode the actual binary servicing format or corroborate ownership from another authoritative source;
- reject structurally invalid decodes;
- never infer component absence from a parser failure.

Before forced residual deletion of WinSxS-owned payload/registry data, servicing ownership must be strong enough to establish exclusive ownership and absence of retained dependencies.

## 5. Other historical collector/parser failures to avoid

### Service dependency auxiliary output

A previous dedicated dependency export was effectively empty because of collector logic even though the main service table retained hundreds of dependency values.

Lesson: validate normalized secondary tables against the authoritative raw source counts; do not treat successful file creation as collector correctness.

### Task trigger normalization

Some parsed task triggers degraded to generic/empty trigger types while the original task files were still present and valid.

Lesson: preserve source task definitions and distinguish `source captured` from `normalization complete`.

### PowerShell formatting serialization

Some runtime preference/state outputs were lost or made unreliable because human-formatted PowerShell output was captured instead of structured data.

Lesson: collector output should be structured/deterministic; human formatting is not an evidence API.

### Version/name heuristics

The component store contained mixed servicing revisions, making filename/version heuristics unreliable.

Lesson: follow actual servicing identities and authoritative relationships instead of deciding ownership from naming patterns.

### AppX `NonRemovable`

`NonRemovable=True` is a servicing/runtime constraint, not proof of metaphysical impossibility; conversely, bypassing that flag is not evidence of safety.

Lesson: dependency/ownership analysis precedes removal decisions.

## 6. Static-image versus runtime boundary

The predecessor image-baseline design explicitly avoided pretending to collect live/runtime-only facts such as:

```text
running process/PID state
loaded runtime modules
active TCP/UDP endpoints
live WFP/firewall filters
live PnP hardware enumeration
MachineGuid generated after installation
post-install user SIDs/per-user package registration
activation state
host VMware Tools state
```

Nylon should preserve this boundary.

The offline image is about:

```text
SHIPPED STATIC COMPOSITION
OWNERSHIP
DEPENDENCIES
DEFAULT REGISTRATION / CONFIGURATION
```

A future installed-runtime baseline can be compared separately if needed.

## 7. Compare-friendly identities

The old design found value in stable identifiers such as:

```text
service name
package identity
component identity
AppX family/full name
task path
CLSID/AppID/IID
INF/package identity
image-relative file path
```

Avoid making host-specific mount/cache paths the canonical identity.

A later source-vs-runtime or source-vs-stripped diff should be able to classify facts such as:

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

## 8. Performance/reliability lessons

The predecessor design called for:

```text
streaming output
bounded buffers
bounded hashing workers
iterative registry walks
no giant all-image graph in RAM
progress reporting
resume support
atomic finalization
collector-specific status/gaps
```

Nylon's newer registry implementation already demonstrated an even stronger principle: remove duplicate generic work entirely when a dedicated compact representation is faster.

Future collectors should similarly share foundational discovery rather than independently walking the entire image over and over.

## 9. Removal-analysis lesson

The predecessor analysis separated at least three concepts that must not be collapsed:

```text
servicing/ownership graph
runtime/reference/dependency graph
network/policy behavior
```

For example, a component can be structurally core while its network behavior is handled separately.

The removal pipeline should prefer supported servicing first. Direct residual file/registry deletion is a later, higher-evidence operation and must be blocked when exclusive ownership or retained reverse-dependencies remain unresolved.

## 10. What this document is not

This document does not prove:

- current Nylon collector completeness;
- that the current source image is the historical 26100.1742 build;
- that every listed location exists on every Windows SKU;
- that old collector code should be copied into Nylon;
- that DISM or any other old tool choice is the preferred Nylon implementation;
- that historical counts apply to a new scan.

Use it to preserve lessons, source-location knowledge and failure history. Verify implementation choices against current code, the exact target image and authoritative technical documentation when needed.
