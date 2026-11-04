import hashlib
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parent.parent
EXE = PROJECT / "bin" / "nylon-controlsets-test.exe"


def run(path, collect=False, success=True):
    result = subprocess.run([str(EXE), "--collect" if collect else "--verify", str(path)], capture_output=True, text=True)
    assert (result.returncode == 0) == success, result.stderr


def fixture(path, mutation=""):
    connection = sqlite3.connect(path)
    connection.executescript(r"""
    CREATE TABLE registry_hives(id INTEGER PRIMARY KEY,name TEXT);
    CREATE TABLE registry_sources(id INTEGER PRIMARY KEY,required_path TEXT,state TEXT,hive_id INTEGER);
    CREATE TABLE registry_keys(id INTEGER PRIMARY KEY,hive_id INTEGER,parent_id INTEGER,name TEXT,path TEXT);
    CREATE TABLE registry_values(id INTEGER PRIMARY KEY,key_id INTEGER,name TEXT,value_type INTEGER,data BLOB);
    CREATE TABLE registry_omissions(id INTEGER PRIMARY KEY,key_id INTEGER,name TEXT);
    CREATE TABLE collector_runs(collector TEXT PRIMARY KEY,version TEXT,scope TEXT,state TEXT,started_utc TEXT,ended_utc TEXT,discovered INTEGER,represented INTEGER,parsed INTEGER,unsupported INTEGER,failed INTEGER,redacted INTEGER,unresolved INTEGER,discovery_complete INTEGER,representation_complete INTEGER);
    CREATE TABLE evidence_gaps(id INTEGER PRIMARY KEY,collector TEXT,operation TEXT,state TEXT,error_code INTEGER,detail TEXT);
    INSERT INTO registry_hives VALUES(1,'SYSTEM');
    INSERT INTO registry_sources VALUES(1,'WINDOWS\SYSTEM32\CONFIG\SYSTEM','PARSED',1);
    INSERT INTO registry_keys VALUES(1,1,NULL,'SYSTEM','SYSTEM');
    INSERT INTO registry_keys VALUES(2,1,1,'ControlSet001','SYSTEM\ControlSet001');
    INSERT INTO registry_keys VALUES(3,1,1,'ControlSet002','SYSTEM\ControlSet002');
    INSERT INTO registry_keys VALUES(4,1,1,'Select','SYSTEM\Select');
    INSERT INTO registry_keys VALUES(5,1,1,'CurrentControlSet','SYSTEM\CurrentControlSet');
    INSERT INTO registry_keys VALUES(6,1,1,'ControlSetBogus','SYSTEM\ControlSetBogus');
    INSERT INTO collector_runs(collector,state) VALUES('services','PENDING');
    """)
    for identity, name, number in [(1, "Current", 1), (2, "Default", 2), (3, "LastKnownGood", 1)]:
        connection.execute("INSERT INTO registry_values VALUES(?,4,?,4,?)", (identity, name, struct.pack("<I", number)))
    connection.executescript(mutation)
    connection.commit()
    connection.close()


def check(path, expected):
    connection = sqlite3.connect(path)
    try:
        outcomes = dict(connection.execute("SELECT role,state FROM service_control_queries"))
        for role, state in expected.items():
            assert outcomes[role] == state, (role, outcomes)
        assert len(outcomes) == 3
        assert connection.execute("SELECT state,discovery_complete,representation_complete FROM collector_runs WHERE collector='services'").fetchone() == ("PARTIAL", 0, 1)
        assert connection.execute("pragma integrity_check").fetchall() == [("ok",)]
        assert connection.execute("pragma foreign_key_check").fetchall() == []
        if connection.execute("SELECT state FROM service_control_context").fetchone() != ("PRESENT",):
            assert connection.execute("SELECT * FROM service_inventory_accounting").fetchall() == [(1, 0, 0, 0, 0, 0, 0)]
            assert connection.execute("SELECT count(*) FROM service_value_decodings").fetchone() == (0,)
    finally:
        connection.close()


def service_inventory(root):
    path = root / "service-inventory.db"
    fixture(path, r"""
    INSERT INTO registry_keys VALUES(10,1,2,'Services','SYSTEM\ControlSet001\Services');
    INSERT INTO registry_keys VALUES(11,1,3,'services','SYSTEM\ControlSet002\services');
    INSERT INTO registry_keys VALUES(12,1,6,'Services','SYSTEM\ControlSetBogus\Services');
    INSERT INTO registry_keys VALUES(20,1,10,'Valid','SYSTEM\ControlSet001\Services\Valid');
    INSERT INTO registry_keys VALUES(21,1,10,'Unknown','SYSTEM\ControlSet001\Services\Unknown');
    INSERT INTO registry_keys VALUES(22,1,11,'Valid','SYSTEM\ControlSet002\services\Valid');
    INSERT INTO registry_keys VALUES(23,1,12,'Unknown','SYSTEM\ControlSetBogus\Services\Unknown');
    INSERT INTO registry_keys VALUES(24,1,10,'valid','SYSTEM\ControlSet001\Services\valid');
    INSERT INTO registry_keys VALUES(30,1,20,'Parameters','SYSTEM\ControlSet001\Services\Valid\Parameters');
    INSERT INTO registry_keys VALUES(31,1,30,'Services','SYSTEM\ControlSet001\Services\Valid\Parameters\Services');
    INSERT INTO registry_keys VALUES(40,1,1,'DriverDatabase','SYSTEM\DriverDatabase');
    INSERT INTO registry_keys VALUES(41,1,40,'Services','SYSTEM\DriverDatabase\Services');
    INSERT INTO registry_keys VALUES(42,1,41,'Decoy','SYSTEM\DriverDatabase\Services\Decoy');
    INSERT INTO registry_keys VALUES(50,1,5,'Services','SYSTEM\CurrentControlSet\Services');
    INSERT INTO registry_values VALUES(100,10,'RootValue',3,x'00');
    INSERT INTO registry_values VALUES(101,20,'Type',4,x'20000000');
    INSERT INTO registry_values VALUES(102,30,'ServiceDll',2,x'00');
    INSERT INTO registry_values VALUES(103,31,'Unknown',99,x'');
    INSERT INTO registry_values VALUES(104,42,'Decoy',3,x'');
    INSERT INTO registry_omissions VALUES(1,30,'Secret');
    INSERT INTO registry_omissions VALUES(2,10,NULL);
    """)
    run(path, collect=True)
    connection = sqlite3.connect(path)
    assert connection.execute("SELECT * FROM service_inventory_accounting").fetchall() == [(1, 3, 3, 5, 10, 4, 2)]
    assert connection.execute("SELECT key_id,definition_id FROM service_inventory_keys ORDER BY key_id").fetchall() == [(10, None), (11, None), (12, None), (20, 20), (21, 21), (22, 22), (23, 23), (24, 24), (30, 20), (31, 20)]
    assert connection.execute("SELECT key_id FROM service_definitions ORDER BY key_id").fetchall() == [(20,), (21,), (22,), (23,), (24,)]
    assert connection.execute("SELECT value_id FROM service_inventory_values ORDER BY value_id").fetchall() == [(100,), (101,), (102,), (103,)]
    assert connection.execute("SELECT omission_id FROM service_inventory_omissions ORDER BY omission_id").fetchall() == [(1,), (2,)]
    connection.close()
    before = hashlib.sha256(path.read_bytes()).digest()
    run(path)
    assert hashlib.sha256(path.read_bytes()).digest() == before
    corruptions = [
        "DELETE FROM service_definitions WHERE key_id=21;",
        "UPDATE service_definitions SET canonical_path='invented' WHERE key_id=20;",
        "UPDATE service_inventory_keys SET definition_id=21 WHERE key_id=30;",
        "DELETE FROM service_inventory_values WHERE value_id=103;",
        "DELETE FROM service_inventory_omissions WHERE omission_id=2;",
        "DELETE FROM service_roots WHERE key_id=12;",
        "UPDATE service_namespaces SET state='NOT_PRESENT' WHERE control_set_key_id=2;",
        "UPDATE service_inventory_accounting SET definitions=4;",
        "INSERT INTO registry_keys VALUES(60,1,10,'New','SYSTEM\\ControlSet001\\Services\\New');",
    ]
    for position, mutation in enumerate(corruptions):
        damaged = root / f"inventory-corrupt-{position}.db"
        shutil.copyfile(path, damaged)
        connection = sqlite3.connect(damaged)
        connection.executescript(mutation)
        connection.close()
        run(damaged, success=False)
    duplicate = root / "duplicate-services-root.db"
    fixture(duplicate, r"""
    INSERT INTO registry_keys VALUES(10,1,2,'Services','SYSTEM\ControlSet001\Services');
    INSERT INTO registry_keys VALUES(11,1,2,'services','SYSTEM\ControlSet001\services');
    INSERT INTO registry_keys VALUES(12,1,10,'One','SYSTEM\ControlSet001\Services\One');
    INSERT INTO registry_keys VALUES(13,1,11,'Two','SYSTEM\ControlSet001\services\Two');
    """)
    run(duplicate, collect=True)
    connection = sqlite3.connect(duplicate)
    assert connection.execute("SELECT root_count,state FROM service_namespaces WHERE control_set_key_id=2").fetchone() == (2, "AMBIGUOUS")
    assert connection.execute("SELECT count(*) FROM service_definitions").fetchone() == (2,)
    assert connection.execute("SELECT state FROM service_namespaces WHERE control_set_key_id=3").fetchone() == ("NOT_PRESENT",)
    connection.close()


def typed_values(root):
    path = root / "typed-service-values.db"
    fixture(path, r"""
    INSERT INTO registry_keys VALUES(10,1,2,'Services','SYSTEM\ControlSet001\Services');
    INSERT INTO registry_keys VALUES(11,1,10,'Candidate','SYSTEM\ControlSet001\Services\Candidate');
    """)
    limit = 2 * 1024 * 1024
    cases = [
        (4, struct.pack("<I", 0), "PRESENT", 0, []),
        (4, struct.pack("<I", 0xFFFFFFFF), "PRESENT", 0xFFFFFFFF, []),
        (4, b"", "PARSE_FAILED", None, []),
        (4, b"\x01\x00\x00", "PARSE_FAILED", None, []),
        (4, b"\x01\x00\x00\x00\x00", "PARSE_FAILED", None, []),
        (1, "Hello\0".encode("utf-16le"), "PRESENT", None, ["Hello"]),
        (1, b"\0\0", "PRESENT", None, [""]),
        (2, "%SystemRoot%\\Name.dll\0".encode("utf-16le"), "PRESENT", None, ["%SystemRoot%\\Name.dll"]),
        (1, "  preserve spaces  \0".encode("utf-16le"), "PRESENT", None, ["  preserve spaces  "]),
        (1, "漢字🙂\0".encode("utf-16le"), "PRESENT", None, ["漢字🙂"]),
        (1, "\ufeffName\0".encode("utf-16le"), "PRESENT", None, ["\ufeffName"]),
        (1, b"", "PARSE_FAILED", None, []),
        (2, "unterminated".encode("utf-16le"), "PARSE_FAILED", None, []),
        (1, b"x\x00\x00", "PARSE_FAILED", None, []),
        (1, "a\0b\0".encode("utf-16le"), "PARSE_FAILED", None, []),
        (1, b"\x00\xd8\x00\x00", "PARSE_FAILED", None, []),
        (1, b"\x00\xdc\x00\x00", "PARSE_FAILED", None, []),
        (7, b"\0\0\0\0", "PRESENT", None, []),
        (7, "one\0two\0one\0\0".encode("utf-16le"), "PRESENT", None, ["one", "two", "one"]),
        (7, "漢字🙂\0\0".encode("utf-16le"), "PRESENT", None, ["漢字🙂"]),
        (7, b"", "PARSE_FAILED", None, []),
        (7, b"\0\0", "PARSE_FAILED", None, []),
        (7, "one\0".encode("utf-16le"), "PARSE_FAILED", None, []),
        (7, "one\0\0hidden\0\0".encode("utf-16le"), "PARSE_FAILED", None, []),
        (7, "one\0\0\0".encode("utf-16le"), "PARSE_FAILED", None, []),
        (7, b"\x00\xd8\x00\x00\x00\x00", "PARSE_FAILED", None, []),
        (7, "good\0".encode("utf-16le") + b"\x00\xdc\x00\x00\x00\x00", "PARSE_FAILED", None, []),
        (3, b"\x01\x02", "UNSUPPORTED_FORMAT", None, []),
        (5, b"\0\0\0\1", "UNSUPPORTED_FORMAT", None, []),
        (11, struct.pack("<Q", 123), "UNSUPPORTED_FORMAT", None, []),
        (99, b"", "UNSUPPORTED_FORMAT", None, []),
        (1, ("x" * (limit // 2 - 1) + "\0").encode("utf-16le"), "PRESENT", None, ["x" * (limit // 2 - 1)]),
        (1, b"x" * (limit + 2), "UNSUPPORTED_FORMAT", None, []),
        (4, b"x" * (limit + 2), "UNSUPPORTED_FORMAT", None, []),
        (1, "not a BLOB", "PARSE_FAILED", None, []),
    ]
    connection = sqlite3.connect(path)
    for identity, (kind, data, _, _, _) in enumerate(cases, 100):
        connection.execute("INSERT INTO registry_values VALUES(?,11,?, ?, ?)", (identity, f"Value{identity}", kind, data))
    connection.commit()
    connection.close()
    run(path, collect=True)
    connection = sqlite3.connect(path)
    assert connection.execute("SELECT count(*) FROM service_value_decodings").fetchone() == (len(cases),)
    for identity, (_, _, state, number, strings) in enumerate(cases, 100):
        assert connection.execute("SELECT parser_version,state,number FROM service_value_decodings WHERE value_id=?", (identity,)).fetchone() == ("service-value-v1", state, number), identity
        assert connection.execute("SELECT ordinal,text FROM service_value_strings WHERE value_id=? ORDER BY ordinal", (identity,)).fetchall() == list(enumerate(strings)), identity
    assert connection.execute("SELECT count(*) FROM service_value_strings s JOIN service_value_decodings d USING(value_id) WHERE d.state<>'PRESENT'").fetchone() == (0,)
    assert connection.execute("pragma integrity_check").fetchall() == [("ok",)]
    assert connection.execute("pragma foreign_key_check").fetchall() == []
    connection.close()
    before = hashlib.sha256(path.read_bytes()).digest()
    run(path)
    assert hashlib.sha256(path.read_bytes()).digest() == before
    for position, mutation in enumerate([
        "DELETE FROM service_value_decodings WHERE value_id=100;",
        "UPDATE service_value_decodings SET number=1 WHERE value_id=100;",
        "UPDATE service_value_decodings SET kind='SZ' WHERE value_id=100;",
        "UPDATE service_value_decodings SET state='PRESENT' WHERE value_id=102;",
        "UPDATE service_value_decodings SET parser_version='other' WHERE value_id=100;",
        "UPDATE service_value_strings SET text='invented' WHERE value_id=118 AND ordinal=1;",
        "DELETE FROM service_value_strings WHERE value_id=118 AND ordinal=2;",
        "INSERT INTO service_value_strings VALUES(102,0,'invented');",
        "UPDATE registry_values SET data=x'02000000' WHERE id=100;",
    ]):
        damaged = root / f"typed-corrupt-{position}.db"
        shutil.copyfile(path, damaged)
        connection = sqlite3.connect(damaged)
        connection.executescript(mutation)
        connection.close()
        run(damaged, success=False)


def main():
    with tempfile.TemporaryDirectory(prefix="nylon-controls-", dir=PROJECT / "build") as temporary:
        root = Path(temporary)
        service_inventory(root)
        typed_values(root)
        baseline = root / "baseline.db"
        fixture(baseline)
        run(baseline, collect=True)
        check(baseline, {role: "PRESENT" for role in ["Current", "Default", "LastKnownGood"]})
        connection = sqlite3.connect(baseline)
        assert connection.execute("SELECT role,target_key_id FROM service_control_queries ORDER BY role").fetchall() == [("Current", 2), ("Default", 3), ("LastKnownGood", 2)]
        assert connection.execute("SELECT key_id,state FROM service_controlsets ORDER BY key_id").fetchall() == [(2, "PRESENT"), (3, "PRESENT"), (6, "UNSUPPORTED_FORMAT")]
        assert connection.execute("SELECT count(*) FROM service_control_values").fetchone()[0] == 3
        connection.close()
        before = hashlib.sha256(baseline.read_bytes()).digest()
        run(baseline)
        assert hashlib.sha256(baseline.read_bytes()).digest() == before, "Verifier mutated raw rows"
        cases = [
            (r"UPDATE registry_sources SET required_path='Windows\System32\config\SYSTEM';", {"Current": "PRESENT"}),
            ("UPDATE registry_values SET value_type=1 WHERE id=1;", {"Current": "PARSE_FAILED"}),
            ("UPDATE registry_values SET data=x'010000' WHERE id=1;", {"Current": "PARSE_FAILED"}),
            ("UPDATE registry_values SET data=x'00000000' WHERE id=1;", {"Current": "UNSUPPORTED_FORMAT"}),
            ("UPDATE registry_values SET data=x'e8030000' WHERE id=1;", {"Current": "UNSUPPORTED_FORMAT"}),
            ("UPDATE registry_values SET data=x'03000000' WHERE id=1;", {"Current": "UNRESOLVED"}),
            ("DELETE FROM registry_values WHERE id=1;", {"Current": "NOT_PRESENT"}),
            ("UPDATE registry_sources SET state='PARSE_FAILED',hive_id=NULL;", {"Current": "QUERY_FAILED", "Default": "QUERY_FAILED", "LastKnownGood": "QUERY_FAILED"}),
            ("UPDATE registry_sources SET state='NOT_PRESENT',hive_id=NULL;", {"Current": "QUERY_FAILED"}),
            ("DELETE FROM registry_sources;", {"Current": "QUERY_FAILED"}),
            (r"INSERT INTO registry_sources VALUES(2,'Windows\System32\config\SYSTEM','NOT_PRESENT',NULL);", {"Current": "AMBIGUOUS"}),
            (r"INSERT INTO registry_keys VALUES(7,1,1,'select','SYSTEM\select');", {"Current": "AMBIGUOUS", "Default": "AMBIGUOUS"}),
            ("INSERT INTO registry_values VALUES(4,4,'current',4,x'01000000');", {"Current": "AMBIGUOUS"}),
            (r"INSERT INTO registry_keys VALUES(7,1,1,'controlset001','SYSTEM\controlset001');", {"Current": "AMBIGUOUS", "LastKnownGood": "AMBIGUOUS"}),
            ("DELETE FROM registry_values; DELETE FROM registry_keys WHERE id=4;", {"Current": "NOT_PRESENT", "Default": "NOT_PRESENT", "LastKnownGood": "NOT_PRESENT"}),
            ("INSERT INTO registry_omissions VALUES(1,4,'Current');", {"Current": "REDACTED_SENSITIVE", "Default": "PRESENT"}),
            ("INSERT INTO registry_omissions VALUES(1,4,NULL);", {"Current": "REDACTED_SENSITIVE", "Default": "REDACTED_SENSITIVE", "LastKnownGood": "REDACTED_SENSITIVE"}),
            ("INSERT INTO registry_omissions VALUES(1,4,'Password');", {"Current": "PRESENT"}),
            ("DELETE FROM registry_keys WHERE id=2;", {"Current": "UNRESOLVED"}),
            ("INSERT INTO registry_keys VALUES(7,1,NULL,'ROOT','ROOT');", {"Current": "AMBIGUOUS"}),
        ]
        for position, (mutation, expected) in enumerate(cases):
            path = root / f"case-{position}.db"
            fixture(path, mutation)
            run(path, collect=True)
            check(path, expected)
        corruptions = [
            "DELETE FROM service_controlsets WHERE key_id=2;",
            "UPDATE service_controlsets SET canonical_path='invented' WHERE key_id=2;",
            "DELETE FROM service_control_values WHERE value_id=1; UPDATE service_control_queries SET source_count=0 WHERE role='Current';",
            "DELETE FROM service_control_queries WHERE role='LastKnownGood';",
            "UPDATE service_control_queries SET target_key_id=3 WHERE role='Current';",
            "UPDATE service_control_values SET number=2 WHERE value_id=1;",
            "DELETE FROM service_control_sources;",
            "DELETE FROM service_control_select_keys;",
            "UPDATE collector_runs SET state='COMPLETE',discovery_complete=1 WHERE collector='services';",
            "UPDATE collector_runs SET discovered=0,represented=0 WHERE collector='services';",
            "UPDATE collector_runs SET scope='complete-services' WHERE collector='services';",
            "DELETE FROM evidence_gaps;",
        ]
        for position, mutation in enumerate(corruptions):
            path = root / f"corrupt-{position}.db"
            shutil.copyfile(baseline, path)
            changed = sqlite3.connect(path)
            changed.executescript(mutation)
            changed.commit()
            changed.close()
            run(path, success=False)
    print("PASS: offline control sets, discovered service-subtree representation, strict typed value decoding/limits/Unicode/ordering, malformed candidates/duplicates/omissions, corrupt-projection refusal; raw DB unchanged by verification")


if __name__ == "__main__":
    main()
