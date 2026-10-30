import os
import ctypes
from ctypes import wintypes
from pathlib import Path
import sqlite3
import shutil
import struct
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parent.parent
EXE = PROJECT / "bin" / "nylon-foundation-test.exe"


def hive_bytes():
    hive = bytearray(8192)
    hive[:4] = b"regf"
    for offset, value in [(4, 1), (8, 1), (0x14, 1), (0x18, 5), (0x20, 1), (0x24, 0x20), (0x28, 4096)]:
        struct.pack_into("<I", hive, offset, value)
    hive[4096:4100] = b"hbin"
    struct.pack_into("<I", hive, 4104, 4096)
    struct.pack_into("<i", hive, 4096 + 0x20, -0x58)
    key = 4096 + 0x24
    hive[key:key + 2] = b"nk"
    struct.pack_into("<H", hive, key + 2, 0x20)
    struct.pack_into("<I", hive, key + 0x24, 1)
    struct.pack_into("<I", hive, key + 0x28, 0x78)
    struct.pack_into("<H", hive, key + 0x48, 4)
    hive[key + 0x4C:key + 0x50] = b"Root"
    struct.pack_into("<iI", hive, 4096 + 0x78, -8, 0x80)
    struct.pack_into("<i", hive, 4096 + 0x80, -32)
    value = 4096 + 0x84
    struct.pack_into("<2sHIIIH", hive, value, b"vk", 6, 0x80000004, 42, 4, 1)
    hive[value + 0x14:value + 0x1A] = b"Number"
    struct.pack_into("<i", hive, 4096 + 0xA0, 4096 - 0xA0)
    checksum = 0
    for offset in range(0, 0x1FC, 4):
        checksum ^= struct.unpack_from("<I", hive, offset)[0]
    struct.pack_into("<I", hive, 0x1FC, 1 if checksum == 0 else 0xFFFFFFFE if checksum == 0xFFFFFFFF else checksum)
    return hive


def collect(root, destination):
    subprocess.run([str(EXE), str(root), str(destination)], check=True)
    connection = sqlite3.connect(destination)
    assert connection.execute("pragma integrity_check").fetchall() == [("ok",)]
    assert connection.execute("pragma foreign_key_check").fetchall() == []
    discovered, represented, complete = connection.execute(
        "select discovered,represented,representation_complete from collector_runs where collector='filesystem'"
    ).fetchone()
    assert complete == 1 and discovered == represented
    assert represented == connection.execute("select count(*) from fs_entries").fetchone()[0]
    assert not connection.execute("select 1 from fs_entries where children_state='PENDING'").fetchall()
    assert connection.execute("select source_identity_state from audit_identity").fetchone() == (
        "WEAK_RUN_SCOPED_NO_CONTAINER",
    )
    assert connection.execute("select count(*) from collector_runs where state='PENDING'").fetchone()[0] == 16
    assert connection.execute("select count(*) from source_metadata_availability where state='NOT_EXPOSED'").fetchone()[0] == 11
    return connection


def main():
    with tempfile.TemporaryDirectory(prefix="nylon-foundation-", dir=PROJECT / "build") as temporary:
        workspace = Path(temporary)
        root = workspace / "image"
        root.mkdir()
        empty = collect(root, workspace / "empty.db")
        assert empty.execute("select discovered,discovery_complete from collector_runs where collector='filesystem'").fetchone() == (1, 1)
        empty.close()
        (root / "empty").mkdir()
        (root / "Unicode-é-日本語").mkdir()
        original = root / "Unicode-é-日本語" / "payload.bin"
        original.write_bytes(b"opaque-not-interpreted")
        os.link(original, root / "hardlink.bin")
        long_directory = root
        for index in range(8):
            long_directory = long_directory / (f"directory-{index}-" + "a" * 35)
            Path("\\\\?\\" + str(long_directory)).mkdir()
        Path("\\\\?\\" + str(long_directory / "long.txt")).write_text("long", encoding="utf-8")
        outside = workspace / "outside"
        outside.mkdir()
        (outside / "must-not-appear.txt").write_text("outside", encoding="utf-8")
        for name, target in [("loop", root), ("escape", outside)]:
            subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(root / name), str(target)], check=True, capture_output=True)
        result = collect(root, workspace / "namespace.db")
        paths = {row[0] for row in result.execute("select path from fs_entries")}
        assert len(paths) == 16, paths
        assert not any("must-not-appear" in path for path in paths)
        assert result.execute("select count(*) from fs_entries where kind='REPARSE' and children_state='NOT_APPLICABLE'").fetchone()[0] == 2
        assert result.execute("select count(*) from fs_entries where kind='REPARSE' and reparse_state='PRESENT'").fetchone()[0] == 2
        identities = result.execute("select volume_serial,file_index,link_count from fs_entries where path in (?,?)", ("hardlink.bin", "Unicode-é-日本語\\payload.bin")).fetchall()
        assert len(identities) == 2 and identities[0] == identities[1] and identities[0][2] == 2
        assert result.execute("select discovery_complete from collector_runs where collector='filesystem'").fetchone()[0] == 1
        assert not result.execute("select * from evidence_gaps").fetchall()
        identity = result.execute("select audit_id from audit_identity").fetchone()[0]
        result.close()
        repeated = collect(root, workspace / "repeated.db")
        assert identity != repeated.execute("select audit_id from audit_identity").fetchone()[0]
        assert paths == {row[0] for row in repeated.execute("select path from fs_entries")}
        repeated.close()
        unsafe = collect(root / "escape", workspace / "unsafe.db")
        assert unsafe.execute("select discovery_complete,represented,state from collector_runs where collector='filesystem'").fetchone() == (0, 1, "COMPLETE_WITH_GAPS")
        assert unsafe.execute("select count(*) from evidence_gaps where operation='enumerate'").fetchone()[0] == 1
        unsafe.close()
        blocked = root / "blocked"
        blocked.mkdir()
        (blocked / "hidden-by-lock.txt").write_text("present", encoding="utf-8")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        lock = kernel.CreateFileW(str(blocked), 0x40000000, 7, None, 3, 0x02000000, None)
        assert lock != wintypes.HANDLE(-1).value, ctypes.get_last_error()
        try:
            failure = collect(root, workspace / "blocked.db")
            try:
                assert failure.execute("select discovery_complete from collector_runs where collector='filesystem'").fetchone()[0] == 0
                assert failure.execute("select children_state from fs_entries where path='blocked'").fetchone()[0] == "QUERY_FAILED"
                assert failure.execute("select count(*) from evidence_gaps where operation='enumerate' and error_code=32").fetchone()[0] == 1
            finally:
                failure.close()
        finally:
            kernel.CloseHandle(lock)
        deep_root = workspace / "deep"
        deep_root.mkdir()
        nested = deep_root
        for _ in range(514):
            nested = nested / "d"
            Path("\\\\?\\" + str(nested)).mkdir()
        deep = collect(deep_root, workspace / "deep.db")
        assert deep.execute("select discovery_complete from collector_runs where collector='filesystem'").fetchone()[0] == 0
        assert deep.execute("select count(*) from evidence_gaps where operation='enumerate' and error_code=111").fetchone()[0] == 1
        deep.close()
        invalid_root = workspace / "invalid-unicode"
        invalid_root.mkdir()
        (invalid_root / "surrogate-\ud800").mkdir()
        invalid = collect(invalid_root, workspace / "encoding.db")
        assert invalid.execute("select count(*) from fs_entries where path is null and length(path_utf16)>0 and encoding_state='UNRESOLVED'").fetchone()[0] == 1
        assert invalid.execute("select discovery_complete from collector_runs where collector='filesystem'").fetchone()[0] == 1
        assert invalid.execute("select count(*) from evidence_gaps where operation='encode_path'").fetchone()[0] == 1
        invalid.close()
        subprocess.run(["icacls.exe", str(blocked), "/deny", "*S-1-1-0:(RD)"], check=True, capture_output=True)
        try:
            denied = collect(root, workspace / "denied.db")
            try:
                assert denied.execute("select discovery_complete from collector_runs where collector='filesystem'").fetchone()[0] == 0
                assert denied.execute("select count(*) from evidence_gaps where operation='enumerate' and error_code=5").fetchone()[0] == 1
            finally:
                denied.close()
        finally:
            subprocess.run(["icacls.exe", str(blocked), "/remove:d", "*S-1-1-0"], check=True, capture_output=True)
        image = workspace / "windows"
        config = image / "Windows" / "System32" / "config"
        config.mkdir(parents=True)
        (config.parent / "ntoskrnl.exe").write_bytes(b"not-executed")
        for name in ["SOFTWARE", "SYSTEM", "COMPONENTS", "DEFAULT", "SAM", "SECURITY"]:
            (config / name).write_bytes(hive_bytes())
        scanner = workspace / "scanner.exe"
        shutil.copy2(PROJECT / "bin" / "nylon.exe", scanner)
        subprocess.run([str(scanner), "scan", str(image)], check=True, capture_output=True)
        with sqlite3.connect(workspace / "output" / "image.db") as scanned:
            assert scanned.execute("select count(*) from registry_keys").fetchone()[0] == 6
            assert scanned.execute("select count(*) from registry_values where text_value='42'").fetchone()[0] == 6
            assert scanned.execute("select schema_version,state from audit_identity").fetchone() == (3, "PARTIAL")
            assert scanned.execute("pragma integrity_check").fetchall() == [("ok",)]
        scanned.close()
    print("PASS: namespace accounting, identity, long/Unicode/invalid UTF-16 paths, hardlinks, reparse no-follow, unsafe-root/blocked-directory/depth gaps")


if __name__ == "__main__":
    main()
