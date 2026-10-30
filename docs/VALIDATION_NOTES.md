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
