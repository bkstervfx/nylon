# Nylon

Nylon is a read-only C++20 auditor for offline Windows images.

Current implementation covers the project foundation, direct binary registry-hive auditing and portable Windows ISO preparation.

## Build

No Visual Studio, MSVC, CMake, Ninja or PowerShell script is required.

Run from Command Prompt:

```cmd
build.cmd
```

On the first build, `build.cmd` automatically downloads pinned open-source build/runtime dependencies into the repository's local `tools` directory and verifies their identities before use. This includes LLVM-MinGW, SQLite, 7-Zip and wimlib. Later builds reuse the local tools.

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

## ISO handling and cache

Nylon does not mount the ISO and does not use DISM or WIMGAPI mounting. It extracts the ISO as an archive with the local verified 7-Zip runtime, then applies the selected WIM/ESD/SWM image with wimlib using:

```text
--no-acls --no-attributes --rpfix
```

This intentionally avoids carrying Windows security descriptors, TrustedInstaller ownership and file attributes from the Windows image into Nylon's working copy.

The ISO itself is opened read-only and streamed through SHA-256. Final expanded image caches are keyed by the ISO SHA-256 and selected image index:

```text
cache\images\<iso-sha256>\index-<N>\
    image\
    manifest.txt
```

A cache is built in a temporary staging directory, validated, marked read-only and atomically renamed into its final location. Nylon treats a finalized cache as immutable and only reads from it. It never updates an existing finalized entry in place. If an entry is incomplete or its manifest does not match the ISO hash and image index, Nylon stops instead of silently reusing or repairing it.

The read-only cache policy deliberately does not install restrictive ACLs or TrustedInstaller-style ownership, since those would recreate the access and servicing-protection problems the prepared image is meant to avoid.

When `nylon.exe` is inside the repository's `bin` directory, runtime data is written beside the repository under `output`, `temp` and `cache`. If the executable is moved elsewhere, those folders are created beside the executable instead.

The audit database is written to `output\image.db` relative to that runtime root.
