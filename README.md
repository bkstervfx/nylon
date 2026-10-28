# Nylon

Nylon is a read-only C++20 auditor for expanded offline Windows images.

Current implementation covers the project foundation and direct binary registry-hive auditing. Project-owned runtime data stays under `C:\nylon\`.

```powershell
nylon.exe scan <expanded-windows-image>
```

The audit database is written to `C:\nylon\output\image.db`.
