# Critical whole-image edge cases

This document collects architectural edge cases that can produce a technically successful scan but an unsafe or misleading dependency/removal model if ignored.

These are requirements to account for, not claims that current Nylon already implements them.

## 1. Prepared-tree metadata is not sufficient by itself

The prepared image cache is intentionally optimized for safe inspection and currently omits/transforms some source metadata.

Where the source format exposes them and they matter to the evidence model, account for:

```text
security descriptors / ACLs / ownership
original file attributes
reparse tags + source targets
alternate data streams / named streams
extended attributes
sparse/compression metadata
hardlink identity
source-provided content/resource identity
timestamps and their provenance
```

Do not assume the applied cache preserves every one of these fields.

When the WIM/container exposes useful metadata or content hashes/resource identities cheaply, prefer using that authoritative information over re-reading/hashing every expanded payload solely to reconstruct facts the source already knows.

Distinguish:

- NTFS hardlink identity;
- WIM/resource deduplication/content equality;
- two different files that happen to hash identically.

They are not interchangeable ownership relationships.

## 2. Namespace coverage must include objects parsers cannot consume

A foundational filesystem/source inventory should account for every discovered namespace entry before specialized parsers decide whether they understand it.

A PE parser failing must not cause the file to disappear from filesystem coverage. An unsupported manifest must remain a manifest/file object with parse state.

Specialized collectors should attach results to canonical source objects rather than create disconnected duplicate identities whenever practical.

## 3. Reparse points

Do not recursively follow unknown reparse points while globally walking the image.

Record the reparse entry itself, its tag and source-exposed target data where safe.

Remember that the current prepared-cache `--rpfix` policy may transform target text relative to original image metadata. If exact original reparse semantics matter, obtain them from an authoritative source representation.

A reparse target is a relationship, not permission to escape the selected-image namespace or inspect the host filesystem.

## 4. Windows path and command-line normalization

Cross-source relationships frequently contain strings that are not simple relative filenames.

Examples include:

```text
%SystemRoot%\System32\foo.dll
%windir%\System32\foo.exe
\SystemRoot\System32\drivers\foo.sys
System32\foo.exe
"C:\Windows\System32\foo.exe" -arg
rundll32.exe foo.dll,EntryPoint
svchost.exe -k Group -s Service
NT-style paths / device prefixes
environment-variable references
quoted command lines with arguments
```

Build deterministic resolvers for common authoritative Windows forms rather than using naive substring/path splitting.

Preserve:

- original raw string;
- parsed executable/module/reference string;
- normalized image-relative candidate;
- resolution status;
- ambiguity/confidence;
- evidence source.

Do not pretend an ambiguous command line resolved to one file merely because a matching basename exists.

Windows path comparisons are normally case-insensitive, but preserve original spelling. Avoid host-specific absolute cache paths as canonical identity.

WOW64/System32/SysWOW64 semantics must be treated as offline-image architecture facts, not by blindly applying the host process's filesystem redirection behavior.

## 5. Indirect resource strings

Service/display/description and other Windows metadata can use indirect strings such as references into PE resources.

Where readability matters, resolve these deterministically from the offline image when safe and practical, while preserving the original indirect string and the resolved value as separate evidence.

Do not execute/load arbitrary image binaries merely to resolve resources.

## 6. PE dependency semantics

A raw PE import is not automatically a hard removal dependency.

Preserve distinctions including:

```text
normal import
delay import
optional/dynamically loaded relationship when directly evidenced
API-set contract import
forwarded export
SxS/manifest-mediated relationship
unresolved import
```

Modern Windows uses API-set contract names (`api-ms-win-*`, `ext-ms-*`). Do not report them as simply "missing DLLs" when an API-set schema/host mapping can be recovered from the image.

If API-set resolution is implemented, preserve both the original contract import and resolved host mapping with provenance.

Do not infer `LoadLibrary`-style dynamic dependencies from strings alone without evidence.

## 7. Side-by-side and servicing identity

WinSxS/component identities are not ordinary folder names.

Do not use filename/directory-name similarity as authoritative package ownership.

Keep separate:

```text
servicing/package ownership
component identity
physical component directory
hardlink/file identity
runtime reference/dependency
```

A payload can be physically associated with a component and still be shared or required by retained components.

## 8. Nested containers and opaque payloads

The selected image can itself contain important containers, for example recovery images, CABs, MSIX/AppX packages or other structured payloads.

Every container is at minimum a filesystem object. For high-value container types, record container metadata/identity and parse their manifests/indexes where doing so materially improves ownership/dependency reasoning.

Examples that may warrant dedicated handling include:

```text
Windows recovery / WinRE image containers
CAB/servicing packages
AppX/MSIX bundles/packages
catalog files
font/resource containers where relevant
```

Do not create uncontrolled recursive extraction explosions. Container expansion needs explicit type handling, depth/size limits and coverage state. An opaque container remains represented even if its internals are unsupported.

## 9. Registry transaction/recovery state

The current registry parser deliberately refuses dirty primary hives requiring transaction-log recovery rather than interpreting inconsistent state.

For whole-image coverage, preserve the distinction between:

```text
clean hive parsed
hive present but dirty/recovery required
log/recovery artifacts present
unsupported recovery
parse failure
```

Do not mutate the source hive to recover it.

If future read-only log replay/recovery is implemented, it must produce a derived logical view with provenance and leave original hive/log evidence untouched.

A required-hive failure should not erase already collectible evidence from unrelated future collector families; the final audit status can be incomplete/with gaps rather than silently absent.

## 10. Search/index architecture at whole-image scale

"No limits" does not mean linear scans of millions of rows for every interaction.

Build query indexes appropriate to the data:

```text
canonical path/name lookup
case-normalized keys
GUID/service/package/task identities
relationship/reverse-relationship indexes
text/search index for human-readable fields where useful
spatial index for Atlas layout
```

Search results may be paginated/streamed. Pagination is not a coverage cap as long as all matches remain reachable.

Avoid duplicating the full evidence dataset merely to power search.

## 11. Atlas LOD must preserve identity

At low zoom, one visual aggregate can represent thousands of evidence objects. That aggregate is a rendering/layout object, not a replacement semantic/source identity.

Zooming/drilling/searching must eventually expose every underlying represented object.

Do not merge unrelated source identities permanently merely because the visual cluster is convenient.

Stable layout IDs should derive from stable semantic/source keys and layout version rather than transient database row order where practical.

## 12. Semantic work-unit truncation

Bounded model prompts are necessary for performance and context limits, but prompt truncation must not become evidence loss.

For every work unit, distinguish:

- all raw sources owned by/accounted to the unit;
- the bounded subset/representation actually exposed to the model;
- deterministic facts available outside the prompt;
- unresolved evidence not semantically interpreted.

If a work unit is split because of size, preserve parent/context relationships and source accounting across all chunks.

`100% semantic source accounting` must never be interpreted as `every raw byte was sent to the model`.

## 13. Large-model export must be sharded, not silently truncated

The downstream larger model should not depend on one enormous text blob fitting into one context window.

Prefer a deterministic export bundle such as:

```text
manifest / source identity / versions
coverage + gaps + redactions
high-level semantic index
entity/component JSONL shards
relationship/dependency shards
reverse-dependency indexes
raw-evidence reference index
optional human-readable summary
```

Use stable IDs across shards.

A consuming model/agent can analyze the bundle incrementally and request raw evidence by stable reference.

Never silently omit the tail of the map because of context/token limits. If an export view is intentionally filtered, its manifest must say so.

## 14. Export safety and trust boundary

The large-model export may leave the local machine depending on the user's workflow.

Therefore export policy is stricter than local raw evidence storage:

- exclude credential/secrets/private keys by default;
- avoid dumping arbitrary binary blobs;
- include redaction/gap records;
- preserve hashes/stable references so raw local evidence can be retrieved later;
- preserve provenance/evidence class so model summaries cannot erase uncertainty.

The local small model is also not permission to expose secret-bearing raw blobs unnecessarily.

## 15. Before/after comparison

The final stripping workflow needs deterministic comparison between source and modified audits.

Do not compare by transient row IDs.

Use canonical identities and classify changes such as:

```text
expected removal
unexpected removal
modified evidence
new evidence
unchanged evidence
relationship removed/added
coverage regression
parser/version incompatibility
```

A parser/schema upgrade between source and target audits can create apparent changes. Comparison must record version compatibility before treating them as image modifications.

## 16. Modification must never mutate the evidence source

Future stripping work should create a separate modification workspace/output image.

Do not alter:

- original ISO/WIM/ESD/SWM;
- source audit database;
- immutable evidence cache in place.

The modified output receives its own identity and fresh audit.

## 17. Current transitional UI wording

The current browser viewer contains wording such as `Full image atlas` even though the current semantic source is registry-focused.

Treat that as transitional/prototype wording, not proof of current whole-image coverage. A current-state audit should flag misleading completion/coverage labels. Long-term native Atlas labels must be driven by actual collector coverage state.

## 18. Validation edge cases

Tests should include adversarial/synthetic fixtures for cases such as:

```text
very long paths
large registry values
empty/binary registry values
invalid UTF-16 / encoding failures
reparse loops/escapes
malformed PE headers
oversized or deeply nested XML
XML external-entity attempts
binary/non-XML .manifest files
dirty registry hives
ambiguous command lines
API-set imports
bundle/nested-container size/depth limits
interrupted collector/resume
schema/parser version invalidation
viewer device loss / resize / high DPI
```

These tests complement, not replace, real-image validation.
