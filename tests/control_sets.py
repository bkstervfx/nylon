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
    finally:
        connection.close()


def main():
    with tempfile.TemporaryDirectory(prefix="nylon-controls-", dir=PROJECT / "build") as temporary:
        root = Path(temporary)
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
    print("PASS: offline control-set provenance, typed selections, failures/ambiguity/redactions and corrupt-projection refusal; raw DB unchanged by verification")


if __name__ == "__main__":
    main()
