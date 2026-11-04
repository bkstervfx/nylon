# Nylon validation notes

This file records important real-world observations that are not recoverable from source/CI alone.

These are **observations**, not universal guarantees. Revalidate after architectural/runtime changes and do not present old measurements as current performance without a new run.

## 1. Known real source-image observation

A previously exercised ISO had:

```text
ISO SHA-256:
4f59662a96fc1da48c1b415d6c369d08af55ddd64e8f1c84e0166d9e50405d7a

Selected image index:
2

Image name:
Windows 11 IoT Enterprise LTSC 2024
```

This identifies that observed source/run only. Do not assume a new ISO is identical because its display name matches.

## 2. Real registry scan observation

On that image, an optimized registry scan completed with approximately:

```text
359,615 registry keys
632,014 registry values
11 hives
```

The persistent ISO cache was reused on the observed run.

These counts prove that the dedicated registry collector handled a large real image; they do not prove whole-image collection because registry is only one source family.

## 3. Real semantic-plan observation

A real registry semantic plan on the observed database produced:

```text
13,474 work units
991,629 raw registry sources
```

An earlier deterministic/preclassification pass mapped approximately:

```text
3,488 / 13,474 work units
312,238 / 991,629 raw sources
31.4874%
```

before model-backed work.

These values are historical performance/workload observations. New planner/classifier versions can legitimately change the work-unit count while preserving source-accounting invariants.

## 4. Real local-model / GPU observation

Known development hardware:

```text
Windows x64
AMD Radeon RX 9070 XT
16 GB VRAM
```

The pinned Qwen3.5-4B Q8_0 model and llama.cpp Vulkan server were observed loading far enough on this physical GPU for Nylon to reach local model-backed semantic requests.

That observation established that the basic RX/Vulkan/model HTTP path was real, not merely synthetic CI.

It did **not** prove a complete end-to-end semantic map on the final current runtime.

## 5. Important model-contract history

An earlier real run produced ontology/JSON validation failures. Root-cause analysis found that the request used the wrong structured-output shape for the exact pinned llama.cpp server build.

For llama.cpp `b11146`, the expected shape uses a nested JSON schema object equivalent to:

```text
response_format
  type = json_schema
  json_schema
    schema = ...
```

rather than placing `schema` directly beside `type`.

A later request-contract revision also removed model-generated relationship bookkeeping/local IDs and made Nylon reconstruct deterministic IDs/containment edges itself. This reduced output complexity and removed a class of referential-validation failures that JSON Schema could not naturally enforce.

Do not regress this by asking the model to manufacture graph bookkeeping that deterministic code can own.

## 6. Current semantic runtime caveat

The active optimized build currently composes the performance runtime from `semantic/semantic_v4.cpp` while retaining request-contract compatibility with the grounded-entity v3 contract.

CI exercises planner/preclassification/resume/HTTP/validation/hierarchy behavior with a synthetic local fake-model server.

Therefore CI success proves those pipeline invariants on the fixture but does **not** prove:

- actual llama.cpp grammar generation for every real prompt;
- Qwen semantic quality;
- sustained throughput on the RX 9070 XT;
- full completion of the approximately 10k model-backed work units from the historical real image;
- whole-image semantic coverage beyond imported registry evidence.

A fresh physical end-to-end run is required before making those claims.

## 7. Viewer observation

The browser viewer was successfully observed showing the current registry-oriented Raw/Atlas/Progress interface after stale-build marker issues were fixed.

The current Atlas remains a transitional browser representation of semantic work derived from registry data. Its existence does not prove the final native whole-image Atlas architecture.

## 8. Validation rule

When a new milestone is implemented, record the strongest validation actually performed and keep these levels separate:

```text
COMPILES
SYNTHETIC_CI_PASSES
REAL_IMAGE_PASSES
REAL_HARDWARE_PASSES
FULL_TARGET_COVERAGE_INVARIANTS_PASS
```

Do not promote a result to a stronger level than was actually observed.


## 9. Local scanner-flow refactor validation - November 4, 2026

Starting revision: `86f70cb46de06827ff7ca818b40b5f1cfe3bcf15` (M9, raw schema v7).
The first structural change extracts command orchestration and audit lifecycle from
`main.cpp` into `src/scan`, without moving or rewriting the collectors. One related
error-handling correction rejects missing/null/duplicate registry-accounting rows and
query failures rather than reporting them as successful collection with gaps.

After the factory reset, the existing portable bootstrap restored its unchanged pins:
LLVM-MinGW `20261020`, SQLite `3.54.1`, 7-Zip `26.05`, wimlib `1.14.5`.
LF-only batch files initially failed with a missing `download_verified` label;
`.gitattributes` now requires CRLF for batch files and owned C++ source/headers.
An isolated CPython `3.13.9` embeddable runtime was restored under the local Nylon tools
directory for tests. Its archive SHA-256 matched the official download metadata:
`91d828c2da3a029b41699e918674a0cb379c02cf20dab9c501306885f837402a`.
This was a digest comparison, not independent verification of a Sigstore signature.
No dependency pins were upgraded and no global Python installation was changed.

Observed locally before changing M9 C++:

- `build.cmd`, `build-semantic.cmd`, `build-viewer.cmd`: passed compilation.
- `test-foundation.cmd`, `test-catalog.cmd`, `test-references.cmd`,
  `test-controlsets.cmd`, `python tests/source_binding.py`: passed their synthetic checks.

Observed locally after the scanner-flow change:

- `build.cmd`: passed compilation with the existing warning/error flags.
- All five existing regression commands above: passed again.
- `test-scan.cmd`: passed the native accounting-error checks and synthetic scan tests.
  These cover invalid arguments, fresh v7 output, integrity/foreign-key checks,
  unchanged source payloads, preservation of published evidence when input/runtime
  preparation or final replacement fails, and successful publication on retry.
- `python tests/scan.py --baseline build/baseline-nylon.exe`: the saved unchanged M9
  executable and the rebuilt scanner produced matching console output, schema and
  contents of all 36 tables on the same synthetic expanded image. Only per-run audit
  IDs, run timestamps and filesystem access times were normalized. A separate repeat
  using the unchanged executable twice also changed the root directory access time;
  other filesystem fields matched in that control.
- Changed C++ was reviewed against Skeet's entry point, context routines, Win32
  helpers, thread implementation and header layout. The existing compiler rejects
  Skeet's MSVC integer suffixes under the project's flags; standard literals preserve
  that compatibility without changing compiler flags.

These are local compilation/synthetic observations. No remote CI run was performed.
The previously recorded Windows ISO, audit database and prepared image cache were
unavailable after recovery. No real-image scan, real-model/GPU run, stripping,
reconstruction, installation, VM boot or retained-feature validation was performed.
Historical observations above remain historical; M10 has not started.
