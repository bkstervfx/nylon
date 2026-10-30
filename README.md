# Nylon

Nylon is a read-only C++20 auditor for offline Windows images.

Current implementation covers the project foundation, direct binary registry-hive auditing and portable Windows ISO preparation.

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

The source `image.db` is never modified. `image.map.db` is a derived, reproducible semantic representation and records exact source references back to the audit evidence. Work is partitioned into bounded units, validated before commit, and resumable. Known Windows structures are normalized deterministically where possible; the local model is used for bounded semantic lifting rather than whole-image chat or threat scoring.

In the viewer, choose **Semantic** to see the mapping pass as a dense work field. Each square is a real semantic work unit:

```text
gray   pending
blue   currently processing
green  validated and committed
red    failed
```

Hovering a square shows the exact registry branch and source-object count. Clicking it opens the committed semantic output or the actual failure details. The percentage is calculated from raw registry keys/values owned by completed work units, so it represents source coverage rather than an estimated animation.

The initial local configuration is pinned to Qwen3.5-4B Q8_0 through llama.cpp Vulkan with four parallel inference slots. `semantic/model.manifest` records the model/runtime identities and semantic schema versions.

## Viewer

Open the raw audit viewer without running the semantic pass with:

```cmd
view.cmd
```

The viewer and semantic progress UI are served only on `127.0.0.1`. The audit database is opened read-only.

## ISO handling and persistent cache

Nylon does not mount the ISO and does not use DISM or WIMGAPI mounting. It extracts the ISO as an archive with the local verified 7-Zip runtime, then applies the selected WIM/ESD/SWM image with wimlib using:

```text
--no-acls --no-attributes --rpfix
```

This intentionally avoids carrying Windows security descriptors, TrustedInstaller ownership and file attributes from the Windows image into Nylon's working copy.

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
