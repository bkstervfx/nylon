import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import threading


PROJECT = Path(__file__).resolve().parent.parent
RUNTIME = PROJECT / "bin" / "nylon-semantic.exe"
CLASSIFIER = PROJECT / "bin" / "nylon-semantic-classify.exe"
LINKER = PROJECT / "bin" / "nylon-semantic-link.exe"


def run(executable, *arguments, success=True):
    result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, text=True, timeout=120)
    if (result.returncode == 0) != success:
        raise AssertionError(f"Unexpected exit {result.returncode}: {result.stdout}\n{result.stderr}")
    return result


def scalar(path, query):
    database = sqlite3.connect(path)
    try:
        return database.execute(query).fetchone()[0]
    finally:
        database.close()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(path):
    database = sqlite3.connect(path)
    database.executescript("""
    CREATE TABLE metadata(meta_key TEXT PRIMARY KEY,value TEXT NOT NULL);
    CREATE TABLE registry_hives(id INTEGER PRIMARY KEY,name TEXT,source_path TEXT,required INTEGER);
    CREATE TABLE registry_keys(id INTEGER PRIMARY KEY,hive_id INTEGER,parent_id INTEGER,cell_offset INTEGER,name TEXT,path TEXT);
    CREATE TABLE registry_values(id INTEGER PRIMARY KEY,key_id INTEGER,name TEXT,value_type INTEGER,data BLOB,text_value TEXT);
    CREATE INDEX registry_key_parent_index ON registry_keys(parent_id);
    CREATE INDEX registry_value_key_index ON registry_values(key_id);
    INSERT INTO metadata VALUES('schema_version','2');
    INSERT INTO registry_hives VALUES(1,'SYSTEM','Windows\\System32\\config\\SYSTEM',1);
    """)
    keys = [
        (1, None, "SYSTEM", "SYSTEM"),
        (2, 1, "ControlSet001", "SYSTEM\\ControlSet001"),
        (3, 2, "Services", "SYSTEM\\ControlSet001\\Services"),
        (4, 3, "Svc000", "SYSTEM\\ControlSet001\\Services\\Svc000"),
        (5, 1, "NylonUnknown", "SYSTEM\\NylonUnknown"),
        (6, 1, "Classes", "SYSTEM\\Classes"),
        (7, 6, "Interface", "SYSTEM\\Classes\\Interface"),
        (8, 7, "{11111111-2222-3333-4444-555555555555}", "SYSTEM\\Classes\\Interface\\{11111111-2222-3333-4444-555555555555}"),
    ]
    for identifier, parent, name, key_path in keys:
        database.execute("INSERT INTO registry_keys VALUES(?,1,?,?,?,?)", (identifier, parent, identifier * 32, name, key_path))
    values = [(4, "Start", "2"), (4, "Type", "16"), (4, "ImagePath", "C:\\Windows\\System32\\Svc000.exe"), (5, "Meaning", "synthetic semantic fixture")]
    values += [(8, f"Value{index:03}", f"interface-value-{index}") for index in range(120)]
    for identifier, (key, name, value) in enumerate(values, 1):
        database.execute("INSERT INTO registry_values VALUES(?,?,?,?,?,?)", (identifier, key, name, 1, b"x", value))
    database.commit()
    database.close()


class Handler(BaseHTTPRequestHandler):
    requests = 0

    def log_message(self, *arguments):
        pass

    def do_GET(self):
        body = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert request["response_format"]["type"] == "json_schema"
        assert "schema" in request["response_format"]["json_schema"]
        Handler.requests += 1
        delta = {"group_kind": "registry_configuration", "group_label": "Normalized registry configuration", "summary": "Synthetic fixture result.", "entities": [], "relationships": [], "unresolved": []}
        body = json.dumps({"choices": [{"message": {"content": json.dumps(delta)}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    with tempfile.TemporaryDirectory(prefix="nylon-binding-", dir=PROJECT / "build") as temporary:
        root = Path(temporary)
        source = root / "source.db"
        mapped = root / "source.map.db"
        fixture(source)
        original_source_hash = digest(source)
        run(RUNTIME, source, "--plan-only")
        expected = "raw-db-sha256-v1:" + original_source_hash
        assert scalar(mapped, "SELECT value FROM metadata WHERE key='source_fingerprint'") == expected
        run(CLASSIFIER, source)
        assert scalar(mapped, "SELECT count(*) FROM lift_jobs WHERE deterministic_kind='component'") > 0
        run(RUNTIME, source, "--deterministic-only")
        server = ThreadingHTTPServer(("127.0.0.1", 51827), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            run(RUNTIME, source)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)
        run(LINKER, source)
        assert Handler.requests > 0
        assert scalar(mapped, "SELECT value FROM metadata WHERE key='hierarchy_complete'") == "1"
        assert scalar(mapped, "SELECT value FROM metadata WHERE key='source_coverage_complete'") == "1"
        total = scalar(source, "SELECT (SELECT count(*) FROM registry_keys)+(SELECT count(*) FROM registry_values)")
        assert scalar(mapped, "SELECT count(DISTINCT source_ref) FROM semantic_sources WHERE source_ref LIKE 'rk:%' OR source_ref LIKE 'rv:%'") == total
        assert scalar(mapped, "SELECT count(*) FROM lift_jobs WHERE status NOT IN (2,3)") == 0
        assert digest(source) == original_source_hash
        relocated = root / "relocated.db"
        shutil.copy2(source, relocated)
        run(CLASSIFIER, relocated, mapped)
        run(LINKER, relocated, mapped)
        assert scalar(mapped, "SELECT value FROM metadata WHERE key='source_fingerprint'") == expected
        before = source.stat()
        database = sqlite3.connect(source)
        database.execute("UPDATE registry_values SET text_value='synthetic semantic altered' WHERE name='Meaning'")
        database.commit()
        database.close()
        os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
        assert source.stat().st_size == before.st_size and source.stat().st_mtime_ns == before.st_mtime_ns
        assert digest(source) != original_source_hash
        map_hash = digest(mapped)
        for executable in [CLASSIFIER, LINKER]:
            result = run(executable, source, success=False)
            assert "source binding mismatch" in result.stderr
            assert digest(mapped) == map_hash
        run(RUNTIME, source, "--plan-only")
        assert scalar(mapped, "SELECT value FROM metadata WHERE key='source_fingerprint'") == "raw-db-sha256-v1:" + digest(source)
        sidecar = Path(str(source) + "-wal")
        sidecar.write_bytes(b"unfinalized-state")
        try:
            for executable in [RUNTIME, CLASSIFIER, LINKER]:
                arguments = [source, "--plan-only"] if executable == RUNTIME else [source]
                result = run(executable, *arguments, success=False)
                assert "nonempty WAL/journal" in result.stderr
        finally:
            sidecar.unlink()
        database = sqlite3.connect(source)
        database.execute("UPDATE metadata SET value='4' WHERE meta_key='schema_version'")
        database.execute("INSERT INTO metadata VALUES('audit_id',?)", ("a" * 64,))
        database.commit()
        database.close()
        for schema in ["4", "5"]:
            with sqlite3.connect(source) as upgraded:
                upgraded.execute("UPDATE metadata SET value=? WHERE meta_key='schema_version'", (schema,))
            upgraded.close()
            run(RUNTIME, source, "--plan-only")
            assert scalar(mapped, "SELECT value FROM metadata WHERE key='source_audit_id'") == "a" * 64
            assert digest(source) in scalar(mapped, "SELECT value FROM metadata WHERE key='source_fingerprint'")
        database = sqlite3.connect(source)
        database.execute("INSERT INTO registry_hives VALUES(2,'SAM','Windows\\System32\\config\\SAM',1)")
        database.commit()
        database.close()
        map_hash = digest(mapped)
        for executable in [RUNTIME, CLASSIFIER, LINKER]:
            arguments = [source, "--plan-only"] if executable == RUNTIME else [source]
            result = run(executable, *arguments, success=False)
            assert "Sensitive or unreviewed registry hive input" in result.stderr
            assert digest(mapped) == map_hash
    print("PASS: synthetic semantic plan/preclassify/resume/fake-HTTP/hierarchy source accounting; content binding, relocation, same-size/time mutation, auxiliary refusal, WAL refusal and v4/v5 metadata")


if __name__ == "__main__":
    main()
