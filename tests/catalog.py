import hashlib
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parent.parent
EXE = PROJECT / "bin" / "nylon-catalog-test.exe"


def run(*arguments, success=True):
    result = subprocess.run([str(EXE), *map(str, arguments)], capture_output=True, text=True)
    assert (result.returncode == 0) == success, result.stderr
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="nylon-catalog-", dir=PROJECT / "build") as temporary:
        root = Path(temporary)
        iso = root / "synthetic.iso"
        iso.write_bytes(b"synthetic source identity, not real ISO media")
        source_hash = hashlib.sha256(iso.read_bytes()).hexdigest()
        cache = root / "cache"
        entry = cache / "images" / source_hash / "index-2"
        config = entry / "image" / "Windows" / "System32" / "config"
        config.mkdir(parents=True)
        for name in ["SYSTEM", "SOFTWARE"]:
            (config / name).write_bytes(b"synthetic payload, not parsed in catalog test")
        manifest = entry / "manifest.txt"
        base = f"format=1\ncomplete=1\nimmutable=1\niso_sha256={source_hash}\nimage_index=2\nimage_name=cached name\n"

        def collect(label, xml=None, declared_hash=None):
            manifest.write_text(base + (f"catalog_sha256={declared_hash or hashlib.sha256(xml).hexdigest()}\ninstall_container_path=sources/install.wim\n" if xml is not None else ""), encoding="utf-8")
            if xml is not None:
                (entry / "catalog.xml").write_bytes(xml)
            before = {path.relative_to(entry): (path.read_bytes(), path.stat().st_mtime_ns) for path in entry.rglob("*") if path.is_file()}
            database = root / f"{label}.db"
            run("--prepare", iso, cache, database)
            after = {path.relative_to(entry): (path.read_bytes(), path.stat().st_mtime_ns) for path in entry.rglob("*") if path.is_file()}
            assert before == after, "Preparation mutated existing cache"
            connection = sqlite3.connect(database)
            assert connection.execute("pragma integrity_check").fetchall() == [("ok",)]
            assert connection.execute("pragma foreign_key_check").fetchall() == []
            return database, connection

        legacy, connection = collect("legacy")
        assert connection.execute("select state,xml from source_catalog").fetchone() == ("NOT_EXPOSED", None)
        connection.close()
        valid = ("<?xml version='1.0' encoding='UTF-16'?><WIM><TOTALBYTES>900</TOTALBYTES>"
                 "<IMAGE INDEX='2'><NAME>日本 &amp; &#x1F600;</NAME><WINDOWS><ARCH>9</ARCH><EDITIONID>IoT</EDITIONID>"
                 "<VERSION><BUILD>26100</BUILD></VERSION><LANGUAGES><LANGUAGE>en-US</LANGUAGE><LANGUAGE>ja-JP</LANGUAGE></LANGUAGES>"
                 "</WINDOWS><UNKNOWN flag='x'><![CDATA[<not markup>]]></UNKNOWN><EMPTY/></IMAGE>"
                 "<IMAGE INDEX='1'><NAME>other</NAME></IMAGE></WIM>").encode("utf-16")
        database, connection = collect("valid", valid)
        assert connection.execute("select state,xml,image_count from source_catalog").fetchone() == ("PRESENT", valid, 2)
        assert connection.execute("select image_index,selected from source_images order by image_index").fetchall() == [(1, 0), (2, 1)]
        fields = connection.execute("select path,value from source_image_fields where image_index=2 order by ordinal").fetchall()
        assert ("NAME", "日本 & 😀") in fields
        assert ("WINDOWS/VERSION/BUILD", "26100") in fields
        assert [value for path, value in fields if path == "WINDOWS/LANGUAGES/LANGUAGE"] == ["en-US", "ja-JP"]
        assert ("UNKNOWN/@flag", "x") in fields and ("UNKNOWN", "<not markup>") in fields and ("EMPTY", "") in fields
        assert connection.execute("select state,discovery_complete from collector_runs where collector='source_media'").fetchone() == ("PARTIAL", 0)
        assert connection.execute("select count(*) from source_metadata_availability where state='NOT_EXPOSED'").fetchone()[0] == 10
        connection.close()
        mutations = [
            "DELETE FROM source_images WHERE image_index=1; UPDATE source_catalog SET image_count=1; UPDATE collector_runs SET discovered=2,represented=2,parsed=1 WHERE collector='source_media';",
            "DELETE FROM source_image_fields WHERE path='WINDOWS/ARCH'; UPDATE source_images SET field_count=field_count-1 WHERE image_index=2;",
            "UPDATE source_image_fields SET value='invented' WHERE path='NAME';",
            "UPDATE source_image_fields SET ordinal=100 WHERE image_index=1;",
            "UPDATE source_images SET selected=1 WHERE image_index=1;",
            "UPDATE source_catalog SET xml_sha256='wrong';",
            "UPDATE source_catalog SET xml=zeroblob(length(xml));",
            "UPDATE source_metadata_availability SET state='NOT_EXPOSED' WHERE field='image_catalog';",
            "UPDATE source_metadata_availability SET state='PRESENT' WHERE field='security_descriptors';",
            "UPDATE source_metadata_availability SET field='invented' WHERE field='security_descriptors';",
            "UPDATE collector_runs SET state='COMPLETE',discovery_complete=1 WHERE collector='source_media';",
            "DELETE FROM evidence_gaps WHERE collector='source_media';",
        ]
        for position, mutation in enumerate(mutations):
            corrupted = root / f"corrupted-{position}.db"
            shutil.copyfile(database, corrupted)
            with sqlite3.connect(corrupted) as changed:
                changed.executescript(mutation)
            changed.close()
            run("--verify", corrupted, success=False)
        missing, connection = collect("missing-selected", b"<WIM><IMAGE INDEX='1'><NAME>other</NAME></IMAGE></WIM>")
        assert connection.execute("select state from source_catalog").fetchone()[0] == "UNRESOLVED"
        connection.close()
        invalids = [
            b"<WIM><IMAGE INDEX='2'>",
            b"<!DOCTYPE WIM [<!ENTITY x SYSTEM 'file:///C:/Windows/win.ini'>]><WIM><IMAGE INDEX='2'><NAME>&x;</NAME></IMAGE></WIM>",
            b"<!DOCTYPE WIM [<!ENTITY x SYSTEM 'https://example.invalid/never-resolve'>]><WIM><IMAGE INDEX='2'/></WIM>",
            b"<WIM><IMAGE INDEX='2'/><IMAGE INDEX='2'/></WIM>",
            b"<WIM><IMAGE INDEX='0'/></WIM>",
            b"<WIM><IMAGE INDEX='4294967296'/></WIM>",
            b"<WIM><IMAGE INDEX='2'><NAME/><NAME>duplicate</NAME></IMAGE></WIM>",
            b"<WIM><IMAGE INDEX='2'>" + b"<DEEP>" * 70 + b"x" + b"</DEEP>" * 70 + b"</IMAGE></WIM>",
            b"<WIM><IMAGE INDEX='2'><NAME>\xff</NAME></IMAGE></WIM>",
            b"<WIM xmlns='unsupported'><IMAGE INDEX='2'/></WIM>",
        ]
        for position, xml in enumerate(invalids):
            invalid, connection = collect(f"invalid-{position}", xml)
            assert connection.execute("select state,xml,image_count from source_catalog").fetchone() == ("PARSE_FAILED", xml, 0)
            connection.close()
            path = root / "invalid.xml"
            path.write_bytes(xml)
            run("--parse", path, success=False)
        wrong_hash, connection = collect("hash-mismatch", valid, "0" * 64)
        assert connection.execute("select state,xml from source_catalog").fetchone() == ("QUERY_FAILED", None)
        connection.close()
        oversize, connection = collect("oversize", b" " * (64 * 1024 * 1024 + 1))
        assert connection.execute("select state,xml from source_catalog").fetchone() == ("QUERY_FAILED", None)
        connection.close()
        assert not (cache / "staging").exists(), "Warm cache unnecessarily staged/reapplied media"
        tools = root / "synthetic-tools"
        extractor = tools / "tools" / "media" / "7zip"
        wimlib = tools / "tools" / "media" / "wimlib"
        extractor.mkdir(parents=True)
        wimlib.mkdir(parents=True)
        for target in [extractor / "7z.exe", wimlib / "wimlib-imagex.exe"]:
            shutil.copyfile(PROJECT / "bin" / "nylon-media-tool-fixture.exe", target)
        (wimlib / "catalog-fixture.xml").write_bytes(valid)
        cold_cache = root / "cold-cache"
        try:
            cold = root / "cold.db"
            run("--prepare", iso, cold_cache, cold, tools)
            cold_entry = cold_cache / "images" / source_hash / "index-2"
            assert (cold_entry / "catalog.xml").read_bytes() == valid
            assert f"catalog_sha256={hashlib.sha256(valid).hexdigest()}" in (cold_entry / "manifest.txt").read_text()
            with sqlite3.connect(cold) as evidence:
                assert evidence.execute("select state,xml from source_catalog").fetchone() == ("PRESENT", valid)
            evidence.close()
            run("--prepare", iso, cold_cache, root / "warm-after-cold.db", tools)
            assert (wimlib / "activity.log").read_text().splitlines() == ["info", "apply"], "Warm reuse invoked synthetic media tools again"
        finally:
            for path in cold_cache.rglob("*"):
                os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    print("PASS: catalog bytes/projection/accounting, legacy warm availability, invalid/DTD/depth/size refusal, corruption gates; existing cache unchanged")


if __name__ == "__main__":
    main()
