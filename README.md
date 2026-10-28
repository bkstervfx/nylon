# Nylon

Nylon is a read-only C++20 auditor for expanded offline Windows images.

Current implementation covers the project foundation and direct binary registry-hive auditing.

## Build

No Visual Studio, MSVC, CMake, Ninja or PowerShell script is required.

Run from Command Prompt:

```cmd
build.cmd
```

On the first build, `build.cmd` automatically downloads the pinned LLVM-MinGW toolchain and SQLite amalgamation into the repository's local `tools` directory, verifies the LLVM archive SHA-256 and verifies the SQLite source identity, then builds `bin\nylon.exe`. Later builds reuse the local tools.

The repository can be placed anywhere. Nylon does not depend on `C:\nylon` or on a fixed image directory.

## Run

Pass any expanded offline Windows image path explicitly:

```cmd
bin\nylon.exe scan D:\path\to\expanded-image
```

When `nylon.exe` is inside the repository's `bin` directory, runtime data is written beside the repository under `output`, `temp` and `cache`. If the executable is moved elsewhere, those folders are created beside the executable instead.

The audit database is written to `output\image.db` relative to that runtime root.
