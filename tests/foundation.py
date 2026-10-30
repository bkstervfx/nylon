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


def partial_failure_hive():
    cells = bytearray(32)

    def allocate(payload):
        offset = len(cells)
        size = (len(payload) + 11) // 8 * 8
        cells.extend(struct.pack("<i", -size) + payload + bytes(size - 4 - len(payload)))
        return offset

    def key_payload(name, parent):
        payload = bytearray(0x4C) + name.encode("ascii")
        payload[:2] = b"nk"
        struct.pack_into("<H", payload, 2, 0x20)
        struct.pack_into("<I", payload, 0x10, parent)
        struct.pack_into("<H", payload, 0x48, len(name))
        return payload

    root = allocate(key_payload("Root", 0xFFFFFFFF))
    index = allocate(b"li" + struct.pack("<H", 300) + bytes(1200))
    struct.pack_into("<I", cells, root + 4 + 0x14, 300)
    struct.pack_into("<I", cells, root + 4 + 0x1C, index)
    for number in range(300):
        payload = key_payload(f"Child{number:03}", root)
        struct.pack_into("<I", payload, 0x24, 1)
        child = allocate(payload)
        struct.pack_into("<I", cells, index + 8 + number * 4, child)
        if number == 299:
            struct.pack_into("<I", cells, child + 4 + 0x28, 0xFFFFFFF8)
        else:
            value_list = allocate(bytes(4))
            value = bytearray(0x14) + b"Number"
            struct.pack_into("<2sHIIIH", value, 0, b"vk", 6, 0x80000004, 42, 4, 1)
            value_offset = allocate(value)
            struct.pack_into("<I", cells, value_list + 4, value_offset)
            struct.pack_into("<I", cells, child + 4 + 0x28, value_list)
    bin_size = (len(cells) + 8 + 4095) // 4096 * 4096
    free_size = bin_size - len(cells)
    cells.extend(struct.pack("<i", free_size) + bytes(free_size - 4))
    cells[:4] = b"hbin"
    struct.pack_into("<I", cells, 8, bin_size)
    header = hive_bytes()[:4096]
    struct.pack_into("<I", header, 0x28, bin_size)
    checksum = 0
    for offset in range(0, 0x1FC, 4):
        checksum ^= struct.unpack_from("<I", header, offset)[0]
    struct.pack_into("<I", header, 0x1FC, checksum)
    return header + cells


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
    assert connection.execute("select count(*) from collector_runs where state='PENDING'").fetchone()[0] == 15
    assert connection.execute("select state,discovery_complete from collector_runs where collector='source_media'").fetchone() == ("PARTIAL", 0)
    assert connection.execute("select count(*) from source_metadata_availability where state='NOT_EXPOSED'").fetchone()[0] == 11
    return connection


def assert_refused(source, destination, mutation):
    shutil.copy2(source, destination)
    changed = sqlite3.connect(destination)
    changed.executescript(mutation)
    changed.close()
    refused = subprocess.run([str(EXE), "--verify", str(destination)], capture_output=True)
    assert refused.returncode != 0, f"Publication accepted corrupted evidence: {mutation}"


def main():
    with tempfile.TemporaryDirectory(prefix="nylon-foundation-", dir=PROJECT / "build") as temporary:
        workspace = Path(temporary)
        root = workspace / "image"
        root.mkdir()
        empty = collect(root, workspace / "empty.db")
        assert empty.execute("select discovered,discovery_complete from collector_runs where collector='filesystem'").fetchone() == (1, 1)
        empty.close()
        assert_refused(workspace / "empty.db", workspace / "missing-family.db", "DELETE FROM collector_runs WHERE collector='appx';")
        assert_refused(workspace / "empty.db", workspace / "invalid-counter.db", "UPDATE collector_runs SET parsed=parsed+1 WHERE collector='filesystem';")
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
        assert not result.execute("select * from evidence_gaps where collector='filesystem'").fetchall()
        assert result.execute("select operation,state from evidence_gaps where collector='source_media' order by operation").fetchall() == [("image_catalog", "NOT_EXPOSED"), ("remaining_source_media", "UNRESOLVED")]
        identity = result.execute("select audit_id from audit_identity").fetchone()[0]
        result.close()
        assert_refused(workspace / "namespace.db", workspace / "wrong-comparison.db", "UPDATE fs_entries SET comparison_key='WRONG' WHERE path='hardlink.bin';")
        assert_refused(workspace / "namespace.db", workspace / "wrong-utf16.db", "UPDATE fs_entries SET path_utf16=zeroblob(3) WHERE path='hardlink.bin';")
        assert_refused(workspace / "namespace.db", workspace / "wrong-readable-path.db", "UPDATE fs_entries SET path='different.bin' WHERE path='hardlink.bin';")
        assert_refused(workspace / "namespace.db", workspace / "wrong-parent.db", "UPDATE fs_entries SET parent_id=(SELECT id FROM fs_entries WHERE path='empty') WHERE path='hardlink.bin';")
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
        assert_refused(workspace / "encoding.db", workspace / "hidden-encoding-gap.db", "DELETE FROM evidence_gaps WHERE collector='filesystem'; UPDATE collector_runs SET failed=0,unresolved=0,state='COMPLETE' WHERE collector='filesystem';")
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
            assert scanned.execute("select count(*) from registry_keys").fetchone()[0] == 4
            assert scanned.execute("select count(*) from registry_values where text_value='42'").fetchone()[0] == 4
            assert scanned.execute("select schema_version,state from audit_identity").fetchone() == (6, "PARTIAL")
            assert scanned.execute("select state from collector_runs where collector='services'").fetchone() == ("PARTIAL",)
            assert scanned.execute("select count(*) from service_control_queries where state='NOT_PRESENT'").fetchone()[0] == 3
            assert scanned.execute("select count(*) from registry_sources where state='REDACTED_SENSITIVE'").fetchone()[0] == 2
            assert scanned.execute("select count(*) from registry_hives h join registry_sources s on s.hive_id=h.id join fs_entries f on f.id=s.entry_id where h.source_path=f.path").fetchone()[0] == 4
            assert scanned.execute("pragma integrity_check").fetchall() == [("ok",)]
        scanned.close()
        assert_refused(workspace / "output" / "image.db", workspace / "missing-probe.db", "DELETE FROM registry_sources WHERE state='NOT_REGF'; UPDATE collector_runs SET discovered=(SELECT count(*) FROM registry_sources),represented=(SELECT count(*) FROM registry_sources) WHERE collector='registry';")
        assert_refused(workspace / "output" / "image.db", workspace / "wrong-registry-counter.db", "UPDATE collector_runs SET redacted=0 WHERE collector='registry';")
        reparse_image = workspace / "windows-reparse"
        reparse_config = reparse_image / "Windows" / "System32" / "config"
        reparse_config.parent.mkdir(parents=True)
        (reparse_config.parent / "ntoskrnl.exe").write_bytes(b"not-executed")
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(reparse_config), str(config)], check=True, capture_output=True)
        subprocess.run([str(scanner), "scan", str(reparse_image)], check=True, capture_output=True)
        reparse_scan = sqlite3.connect(workspace / "output" / "image.db")
        try:
            assert reparse_scan.execute("SELECT count(*) FROM registry_sources WHERE required=1 AND state='UNRESOLVED'").fetchone()[0] == 6
            assert reparse_scan.execute("SELECT count(*) FROM registry_hives").fetchone()[0] == 0
            assert reparse_scan.execute("SELECT discovery_complete FROM collector_runs WHERE collector='filesystem'").fetchone()[0] == 1
            assert reparse_scan.execute("SELECT discovery_complete FROM collector_runs WHERE collector='registry'").fetchone()[0] == 0
            assert reparse_scan.execute("SELECT count(*) FROM service_control_queries WHERE state='QUERY_FAILED'").fetchone()[0] == 3
        finally:
            reparse_scan.close()
        (image / "unusual.extension").write_bytes(hive_bytes())
        (config / "SOFTWARE").unlink()
        (config / "SYSTEM").write_bytes(partial_failure_hive())
        dirty = hive_bytes()
        struct.pack_into("<I", dirty, 8, 2)
        checksum = 0
        for offset in range(0, 0x1FC, 4):
            checksum ^= struct.unpack_from("<I", dirty, offset)[0]
        struct.pack_into("<I", dirty, 0x1FC, checksum)
        (config / "COMPONENTS").write_bytes(dirty)
        (config / "SAM").write_bytes(b"intentionally-invalid-sensitive-payload")
        user_hive = image / "Users" / "Default" / "NTUSER.DAT"
        user_hive.parent.mkdir(parents=True)
        user_hive.write_bytes(b"unreviewed-user-data-not-opened")
        subprocess.run([str(scanner), "scan", str(image)], check=True, capture_output=True)
        failed_scan = sqlite3.connect(workspace / "output" / "image.db")
        try:
            assert failed_scan.execute("select count(*) from registry_keys").fetchone()[0] == 1
            assert failed_scan.execute("select count(*) from registry_values").fetchone()[0] == 1
            assert failed_scan.execute("select count(*) from registry_hives").fetchone()[0] == 1
            assert failed_scan.execute("select state from registry_sources where required_path like '%SOFTWARE'").fetchone()[0] == "MISSING_REQUIRED"
            assert failed_scan.execute("select state from registry_sources where required_path like '%SYSTEM'").fetchone()[0] == "PARSE_FAILED"
            assert failed_scan.execute("select count(*) from service_control_queries where state='QUERY_FAILED'").fetchone()[0] == 3
            assert "transaction-log" in failed_scan.execute("select detail from registry_sources where required_path like '%COMPONENTS'").fetchone()[0]
            assert failed_scan.execute("select state from registry_sources s join fs_entries f on f.id=s.entry_id where f.path='unusual.extension'").fetchone()[0] == "REDACTED_POLICY"
            assert failed_scan.execute("select state from registry_sources where required_path like '%SAM'").fetchone()[0] == "REDACTED_SENSITIVE"
            assert failed_scan.execute("select discovered=represented,representation_complete,parsed from collector_runs where collector='registry'").fetchone() == (1, 1, 1)
            assert failed_scan.execute("select state from collector_status where collector='registry'").fetchone()[0] == "complete_with_gaps"
            assert failed_scan.execute("pragma integrity_check").fetchall() == [("ok",)]
            assert failed_scan.execute("pragma foreign_key_check").fetchall() == []
        finally:
            failed_scan.close()
        named = hive_bytes()
        named[4096 + 0x84 + 0x14:4096 + 0x84 + 0x1A] = b"TokenX"
        (config / "DEFAULT").write_bytes(named)
        denied_folder = image / "blocked"
        denied_folder.mkdir()
        (denied_folder / "unseen.hive").write_bytes(hive_bytes())
        subprocess.run(["icacls.exe", str(denied_folder), "/deny", "*S-1-1-0:(RD)"], check=True, capture_output=True)
        try:
            subprocess.run([str(scanner), "scan", str(image)], check=True, capture_output=True)
            with sqlite3.connect(workspace / "output" / "image.db") as partial:
                assert partial.execute("select state from registry_sources where required_path like '%SOFTWARE'").fetchone()[0] == "UNRESOLVED"
                assert partial.execute("select discovery_complete from collector_runs where collector='registry'").fetchone()[0] == 0
                assert partial.execute("select count(*) from registry_values").fetchone()[0] == 0
                assert partial.execute("select name,state from registry_omissions").fetchall() == [("TokenX", "REDACTED_POLICY")]
                assert partial.execute("select count(*) from fs_entries").fetchone()[0] > 0
                assert partial.execute("pragma foreign_key_check").fetchall() == []
            partial.close()
        finally:
            subprocess.run(["icacls.exe", str(denied_folder), "/remove:d", "*S-1-1-0"], check=True, capture_output=True)
    print("PASS: namespace identity/accounting/adversarial and corrupt-evidence refusal fixtures; inventory registry provenance, redaction, dirty/missing/partial-hive rollback, reparse ancestry and independent gaps")


if __name__ == "__main__":
    main()
