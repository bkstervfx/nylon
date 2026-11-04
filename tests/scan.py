import argparse
from contextlib import closing
import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path
import runpy
import shutil
import sqlite3
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parent.parent
SCANNER = PROJECT / "bin" / "nylon.exe"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(scanner, environment, arguments, expected):
    result = subprocess.run(
        [str(scanner), *map(str, arguments)], env=environment,
        capture_output=True, text=True, timeout=120,
    )
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    return result


def snapshot(path):
    with closing(sqlite3.connect(path)) as database:
        identity = database.execute("SELECT audit_id FROM audit_identity").fetchone()[0]
        schema = database.execute("SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY type,name").fetchall()
        tables = {}
        for (name,) in database.execute("SELECT name FROM sqlite_schema WHERE type='table' ORDER BY name"):
            quoted = '"' + name.replace('"', '""') + '"'
            columns = [column[1] for column in database.execute(f"PRAGMA table_info({quoted})")]
            rows = []
            for row in database.execute(f"SELECT * FROM {quoted}"):
                values = []
                for column, value in zip(columns, row):
                    # Repeated scans observe changed NTFS access times, including baseline-to-baseline runs.
                    if name == "fs_entries" and column == "access_time" and value is not None:
                        value = "<access-time>"
                    elif column in ("started_utc", "ended_utc") and value is not None:
                        value = "<run-time>"
                    elif value == identity:
                        value = "<audit-id>"
                    values.append(value)
                rows.append(tuple(values))
            tables[name] = sorted(rows, key=repr)
        return schema, tables


def main(baseline=None):
    hive_bytes = runpy.run_path(str(PROJECT / "tests" / "foundation.py"))["hive_bytes"]
    with tempfile.TemporaryDirectory(prefix="nylon-scan-", dir=PROJECT / "build") as temporary:
        root = Path(temporary)
        binary = root / "bin" / "nylon.exe"
        binary.parent.mkdir()
        shutil.copy2(SCANNER, binary)
        (root / "build.cmd").write_text("synthetic project marker", encoding="utf-8")
        environment = dict(os.environ, LOCALAPPDATA=str(root / "state"))
        output = root / "output" / "image.db"
        staging = output.with_name("image.db.staging")

        cases = [
            ([], "Invalid command line"),
            (["scan"], "Invalid command line"),
            (["unknown", "unused"], "Unknown command"),
            (["scan", "unused", "--wrong", "1"], "Expected --index before image number"),
            (["scan", "unused", "--index", ""], "Image index cannot be empty"),
            (["scan", "unused", "--index", "0"], "Image index must be greater than zero"),
            (["scan", "unused", "--index", "-1"], "Image index must be a positive integer"),
            (["scan", "unused", "--index", "1x"], "Image index must be a positive integer"),
            (["scan", "unused", "--index", "4294967296"], "Image index is too large"),
            (["scan", "unused", "--index", "999999999999999999999"], "Image index is too large"),
        ]
        for arguments, message in cases:
            result = run(binary, environment, arguments, 2)
            assert result.stderr == f"error: {message}\n", result.stderr
            assert "nylon.exe scan <windows.iso> [--index <number>]" in result.stdout
            assert not output.parent.exists(), "Invalid CLI created audit output"
            assert not (root / "state").exists(), "Invalid CLI created runtime state"

        image = root / "image"
        config = image / "Windows" / "System32" / "config"
        config.mkdir(parents=True)
        (config.parent / "ntoskrnl.exe").write_bytes(b"synthetic kernel marker; never executed")
        for name in ("SYSTEM", "SOFTWARE", "DEFAULT", "COMPONENTS"):
            (config / name).write_bytes(hive_bytes())
        for name in ("SAM", "SECURITY"):
            (config / name).write_bytes(b"synthetic sensitive placeholder; never opened")
        original = {path: digest(path) for path in image.rglob("*") if path.is_file()}

        previous = None
        if baseline is not None:
            shutil.copy2(baseline, binary)
            previous = run(binary, environment, ["scan", image], 0)
            expected = snapshot(output)
            shutil.copy2(SCANNER, binary)

        current = run(binary, environment, ["scan", image], 0)
        assert output.is_file() and not staging.exists()
        if previous is not None:
            assert (previous.stdout, previous.stderr) == (current.stdout, current.stderr)
            observed = snapshot(output)
            assert expected[0] == observed[0], "Database schema changed"
            for table, rows in expected[1].items():
                if rows != observed[1][table]:
                    missing = [row for row in rows if row not in observed[1][table]]
                    added = [row for row in observed[1][table] if row not in rows]
                    raise AssertionError(f"Evidence changed in {table}: before={missing[:2]!r}; after={added[:2]!r}")
            print(f"PASS: baseline/current console, schema and all {len(expected[1])} tables match; audit IDs, run timestamps and observed filesystem access times normalized")
        with closing(sqlite3.connect(output)) as database:
            assert database.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
            assert database.execute("PRAGMA foreign_key_check").fetchall() == []
            assert database.execute("SELECT value FROM metadata WHERE meta_key='schema_version'").fetchone() == ("7",)
            assert database.execute("SELECT count(*) FROM registry_hives").fetchone() == (4,)
            identity = database.execute("SELECT audit_id FROM audit_identity").fetchone()[0]
        published = digest(output)

        run(binary, environment, ["scan", root / "missing-image"], 1)
        assert digest(output) == published and not staging.exists()

        missing_environment = dict(environment)
        missing_environment.pop("LOCALAPPDATA")
        result = run(binary, missing_environment, ["scan", image], 1)
        assert "LOCALAPPDATA is not available" in result.stderr
        assert digest(output) == published and not staging.exists()

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        handle = kernel.CreateFileW(str(output), 0x80000000, 1, None, 3, 0, None)
        assert handle != ctypes.c_void_p(-1).value, ctypes.get_last_error()
        try:
            result = run(binary, environment, ["scan", image], 1)
            assert "Unable to publish audit database" in result.stderr
            assert digest(output) == published
            assert staging.is_file(), "Failed publication discarded the checked staging database"
        finally:
            assert kernel.CloseHandle(handle)

        run(binary, environment, ["scan", image], 0)
        assert output.is_file() and not staging.exists()
        with closing(sqlite3.connect(output)) as database:
            assert database.execute("SELECT audit_id FROM audit_identity").fetchone()[0] != identity
            assert database.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
        assert all(digest(path) == before for path, before in original.items()), "Scan changed source payloads"

    print("PASS: scan CLI boundaries, fresh v7 audit, unchanged source payloads, failed-publication preservation and retry")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, help="Optional pre-refactor scanner for evidence comparison")
    main(parser.parse_args().baseline)
