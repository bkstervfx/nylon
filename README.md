# Nylon

Nylon is a read-only C++20 auditor for offline Windows images.

It scans an extracted Windows image and writes a compact SQLite dependency graph containing filesystem identity, hardlinks, registry hives, AppX/SystemApps, WinSxS/CBS servicing metadata, packages, features, capabilities, services, drivers, scheduled tasks, COM/shell registrations and PE imports.

## Layout

- `src` source
- `build` CMake output
- `bin` executable
- `temp` disposable runtime data
- `cache` reusable scan data
- `output` audit databases

All project-owned runtime data stays under the Nylon root.

## Build

Use x64 Visual Studio 2022 with a current Windows SDK. Run Nylon from an elevated terminal because the offline registry collector mounts system hives read-only and unloads them immediately after scanning.

```powershell
cmake -S C:\nylon -B C:\nylon\build -A x64
cmake --build C:\nylon\build --config Release
```

If the ADK `DismApi.h` header is available, Nylon also uses the native DISM API for packages, features, capabilities and driver inventory. If it is not available, Nylon records that collector as unavailable rather than using a guessed ABI.

## Run

Place an extracted offline Windows image under `C:\nylon\image` and run:

```powershell
C:\nylon\bin\nylon.exe
```

Or provide another directory under the Nylon root:

```powershell
C:\nylon\bin\nylon.exe C:\nylon\image
```

The audit database is written to `C:\nylon\output\image.db`.
