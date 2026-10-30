# Nylon

Nylon is a read-only C++20 auditor for offline Windows images.

> **Current behavior vs product target:** this README primarily documents commands and behavior that exist today. The canonical long-term product goal and architecture are defined in `docs/PRODUCT_GOAL.md` and `docs/ARCHITECTURE.md`; agent guardrails are in `AGENTS.md`. Do not infer that target collectors/native Atlas are already implemented from those documents, and do not infer that the current registry/browser vertical slice is the final product architecture.

Current implementation covers the project foundation, a selected-image working-namespace
inventory, direct binary registry-hive auditing and portable Windows ISO preparation.
This is not yet exhaustive whole-image domain collection.

The raw schema v4 foundation records `audit_identity`, `collector_runs`, `fs_entries`,
`evidence_gaps` and `source_metadata_availability`. Filesystem discovery/representation
are separate from parsing and from all other required collector families, which remain
pending until migrated. Paths preserve exact UTF-16 identity plus readable/comparison
forms where conversion succeeds. Reparse entries are recorded without following targets.
Working-view metadata is not original source metadata; uncollected original fields are
explicitly `NOT_EXPOSED`. Expanded-tree source identity is explicitly weaker/run-scoped,
not a content hash. Existing audits require a fresh staged scan, not an in-place upgrade.

Registry discovery now consumes that inventory rather than recursively walking image
directories again. Every file-header probe and stock-hive query is accounted in
`registry_sources`; parsed hives link to filesystem entries using image-relative paths.
SAM/SECURITY payloads are not opened, user/arbitrary discovered hives are omitted by
default pending a reviewed policy, and sensitive-name omissions are recorded in
`registry_omissions` without retained raw value bytes. This conservative label policy is
not proof that arbitrary configuration is secret-free or that a portable export is safe.
Malformed/dirty/missing hives no longer erase unrelated collected evidence. Successful
publication can therefore be a `PARTIAL` audit with explicit collector gaps; it is not
whole-image completion. Raw staging uses rollback journaling to isolate failed hives.

After `build.cmd`, `test-foundation.cmd` runs deterministic namespace and registry fixtures.

## Build

No Visual Studio, MSVC, CMake, Ninja or PowerShell script is required.

Run from Command Prompt:

```cmd
build.cmd
```

On the first build, `build.cmd` automatically downloads pinned open-source build/runtime dependencies and verifies their identities before use. LLVM-MinGW, SQLite, 7-Zip, wimlib and their verified download archives are stored persistently under:

```text
%LOCALAPPDATA%\Temp\nylon\tools\
```

Later builds reuse those tools after validating the applicable cached identities. Deleting or recloning the Nylon repository therefore does not force the toolchain or media runtimes to be downloaded again.

The repository can be placed anywhere. Nylon does not depend on `C:\nylon` or on a fixed image directory.

## Run

Nylon accepts either an expanded offline Windows image or a Windows ISO.

```cmd
bin\nylon.exe scan D:\path\to\expanded-image
bin\nylon.exe scan D:\path\to\Windows.iso
```

If the ISO contains multiple Windows images, Nylon lists the available indexes instead of guessing. Re-run with the required index:

```cmd
bin\nylon.exe scan D:\path\to\Windows.iso --index 6
```

## Semantic map

After a scan, Nylon can build a derived human-readable semantic map from the immutable audit database using a pinned local model:

```cmd
semantic.cmd
```

Or target another audit database:

```cmd
semantic.cmd D:\audits\image.db
```

The first run creates/resumes `image.map.db`, opens the local browser viewer immediately, and then installs the pinned local inference runtime/model only when they are not already cached. The model and runtime live under `%LOCALAPPDATA%\Temp\nylon\tools\semantic\` and are reused on later runs.

The source `image.db` is never modified. `image.map.db` is a derived, reproducible semantic representation and records source references back to the audit evidence. Work is partitioned into bounded units, validated before commit, and resumable. Known Windows structures are normalized deterministically where possible; the local model is used for bounded semantic lifting rather than whole-image chat or threat scoring.

The current semantic pipeline is registry-focused. Its source-accounting invariant covers imported registry keys/values; that must not be described as full Windows-image semantic coverage.

In the current viewer, choose **Progress** to see the mapping pass as a dense work field. Each square is a semantic work unit:

```text
gray   pending
blue   currently processing
green  validated and committed
red    failed
```

Hovering a square shows the registry branch and source-object count. Clicking it opens the committed semantic output or failure details. The percentage is calculated from raw registry keys/values owned by completed work units, so it represents current registry-source accounting rather than estimated animation or whole-image completion.

The initial local configuration is pinned to Qwen3.5-4B Q8_0 through llama.cpp Vulkan with four parallel inference slots. `semantic/model.manifest` records the model/runtime identities and semantic schema versions.

## Viewer

Open the current raw/semantic viewer without running the semantic pass with:

```cmd
view.cmd
```

The current viewer is a **transitional browser-based implementation**. It is useful for the existing registry/semantic vertical slice, but the canonical final Atlas architecture is the native Win32 + D3D11 + Direct2D + DirectWrite design in `docs/ARCHITECTURE.md`.

The current viewer and semantic progress UI are served only on `127.0.0.1`. The audit database is opened read-only.

## ISO handling and persistent cache

Nylon does not mount the ISO and does not use DISM or WIMGAPI mounting. It extracts the ISO as an archive with the local verified 7-Zip runtime, then applies the selected WIM/ESD/SWM image with wimlib using:

```text
--no-acls --no-attributes --rpfix
```

This intentionally avoids carrying Windows security descriptors, TrustedInstaller ownership and file attributes from the Windows image into Nylon's working copy.

Accordingly, the expanded cache is an immutable **working payload view**, not a lossless source for every original filesystem metadata field. Future whole-image collectors that need ACL/security-descriptor, original-attribute or other transformed source metadata must obtain it from an authoritative source representation or mark it unavailable/unsupported; they must not infer it from the prepared cache.

For an ISO input, Nylon holds a read/share lock on the source while it is being hashed and prepared. The ISO is streamed through SHA-256, and final expanded image caches are keyed by that SHA-256 plus the selected image index. They are stored outside the repository under:

```text
%LOCALAPPDATA%\Temp\nylon\cache\images\<iso-sha256>\index-<N>\
    image\
    manifest.txt
```

Temporary ISO preparation work is also kept outside the repository under:

```text
%LOCALAPPDATA%\Temp\nylon\temp\
```

This means deleting `C:\nylon`, downloading a fresh copy of the repository, and rebuilding does not delete the expensive expanded ISO cache. A fresh checkout can reuse a valid SHA-256/index cache before 7-Zip or wimlib are needed.

A cache is built in a temporary staging directory, validated, marked read-only and atomically renamed into its final location. Nylon treats a finalized cache as immutable and only reads from it. It never updates an existing finalized entry in place.

Before scanning a finalized ISO cache, Nylon verifies its manifest identity and preparation policy, including the ISO hash, image index, extractor/wimlib versions and WIM apply flags. It also verifies that the published cache tree still carries Nylon's read-only marker. Incomplete, incompatible or modified cache entries are rejected instead of silently reused or repaired.

The read-only cache policy deliberately does not install restrictive ACLs or TrustedInstaller-style ownership, since those would recreate the access and servicing-protection problems the prepared image is meant to avoid. On Windows, the read-only file attribute is enforced for files but is not an ACL-style write barrier for directories; Nylon therefore combines the attribute marker with its own immutable-cache policy and validation rather than adding deny ACLs.

Offline registry files are mapped read-only. Nylon rejects reparse-point traversal for image roots and registry paths, validates REGF base blocks and cell topology, and refuses dirty primary hives that require transaction-log recovery instead of silently interpreting inconsistent data.

The repository itself keeps source code, `build`, `bin`, and `output`. The audit database is written to:

```text
output\image.db
```

Persistent Nylon state is kept under `%LOCALAPPDATA%\Temp\nylon`, so removing the repository does not remove cached Windows images or verified portable tools.
