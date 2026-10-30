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
        cleanup_roots = [cold_cache]
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

            def legacy_cache(label, image_index=2, existing=None):
                destination = existing or root / label
                cleanup_roots.append(destination)
                old = destination / "images" / source_hash / f"index-{image_index}"
                old_config = old / "image" / "Windows" / "System32" / "config"
                old_config.mkdir(parents=True)
                for name in ["SYSTEM", "SOFTWARE"]:
                    (old_config / name).write_bytes(b"unchanged legacy payload")
                (old / "manifest.txt").write_text(base.replace("image_index=2", f"image_index={image_index}") + "iso_extractor=7zip-26.05\nwimlib=1.14.5\nwim_apply_flags=--no-acls --no-attributes --rpfix\n", encoding="utf-8")
                for path in [old, *old.rglob("*")]:
                    os.chmod(path, stat.S_IREAD)
                return destination, old

            def snapshot(directory):
                return {path.relative_to(directory): (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_file_attributes) for path in directory.rglob("*") if path.is_file()}

            def assert_state(path, state, xml=None):
                evidence = sqlite3.connect(path)
                try:
                    assert evidence.execute("select state,xml from source_catalog").fetchone() == (state, xml), evidence.execute("select state,detail from source_catalog").fetchone()
                    assert evidence.execute("pragma integrity_check").fetchall() == [("ok",)]
                    assert evidence.execute("pragma foreign_key_check").fetchall() == []
                finally:
                    evidence.close()

            recovery, old = legacy_cache("legacy-recovery")
            original = snapshot(old)
            run("--prepare", iso, recovery, root / "recovery.db", tools)
            assert_state(root / "recovery.db", "PRESENT", valid)
            assert snapshot(old) == original
            assert (extractor / "activity.log").read_text().splitlines() == ["full-media", "containers-only"]
            assert (wimlib / "activity.log").read_text().splitlines() == ["info", "apply", "info"]
            run("--prepare", iso, recovery, root / "recovery-reuse.db", tools)
            assert_state(root / "recovery-reuse.db", "PRESENT", valid)
            legacy_cache("index1", image_index=1, existing=recovery)
            run("--prepare-index1", iso, recovery, root / "recovery-index1.db", tools)
            with sqlite3.connect(root / "recovery-index1.db") as evidence:
                assert evidence.execute("select image_index from source_images where selected=1").fetchone() == (1,)
                assert evidence.execute("select selected_name from audit_identity").fetchone() == ("other",)
            evidence.close()
            assert (wimlib / "activity.log").read_text().splitlines() == ["info", "apply", "info"]
            sidecar = recovery / "catalogs-v1" / source_hash
            sidecar_manifest = sidecar / "manifest.txt"
            original_manifest = sidecar_manifest.read_bytes()
            os.chmod(sidecar_manifest, stat.S_IREAD | stat.S_IWRITE)
            sidecar_manifest.write_bytes(original_manifest.replace(source_hash.encode(), b"0" * 64))
            os.chmod(sidecar_manifest, stat.S_IREAD)
            run("--prepare", iso, recovery, root / "sidecar-identity-failure.db", tools)
            assert_state(root / "sidecar-identity-failure.db", "QUERY_FAILED")
            os.chmod(sidecar_manifest, stat.S_IREAD | stat.S_IWRITE)
            sidecar_manifest.write_bytes(original_manifest)
            os.chmod(sidecar_manifest, stat.S_IREAD)
            sidecar_xml = sidecar / "catalog.xml"
            os.chmod(sidecar_xml, stat.S_IREAD | stat.S_IWRITE)
            sidecar_xml.write_bytes(b"corrupt catalog")
            os.chmod(sidecar_xml, stat.S_IREAD)
            run("--prepare", iso, recovery, root / "sidecar-hash-failure.db", tools)
            assert_state(root / "sidecar-hash-failure.db", "QUERY_FAILED")
            assert snapshot(old) == original
            assert (wimlib / "activity.log").read_text().splitlines() == ["info", "apply", "info"]
            reparse_cache, reparse_old = legacy_cache("reparse-sidecar-parent")
            junction = reparse_cache / "catalogs-v1"
            subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(junction), str(recovery / "catalogs-v1")], check=True, capture_output=True)
            try:
                run("--prepare", iso, reparse_cache, root / "sidecar-reparse.db", tools)
                assert_state(root / "sidecar-reparse.db", "QUERY_FAILED")
            finally:
                os.rmdir(junction)
            for failure, tool_directory in [("fail-extract", extractor), ("fail-info", wimlib)]:
                failed_cache, failed_old = legacy_cache(failure)
                unchanged = snapshot(failed_old)
                (tool_directory / failure).touch()
                try:
                    output = root / f"{failure}.db"
                    run("--prepare", iso, failed_cache, output, tools)
                    assert_state(output, "QUERY_FAILED")
                    assert snapshot(failed_old) == unchanged
                    assert not (failed_cache / "catalogs-v1" / source_hash).exists()
                finally:
                    (tool_directory / failure).unlink()
            invalid_cache, invalid_old = legacy_cache("recovery-malformed")
            malformed = b"<WIM><IMAGE INDEX='2'>"
            (wimlib / "catalog-fixture.xml").write_bytes(malformed)
            run("--prepare", iso, invalid_cache, root / "recovery-malformed.db", tools)
            assert_state(root / "recovery-malformed.db", "PARSE_FAILED", malformed)
            (wimlib / "catalog-fixture.xml").write_bytes(valid)
            blocked_cache, blocked_old = legacy_cache("publication-failure")
            (wimlib / "block-sidecar-publish").touch()
            try:
                run("--prepare", iso, blocked_cache, root / "publication-failure.db", tools)
                assert_state(root / "publication-failure.db", "PRESENT", valid)
                evidence = sqlite3.connect(root / "publication-failure.db")
                assert "Sidecar publication/reuse failed" in evidence.execute("select detail from source_catalog").fetchone()[0]
                evidence.close()
            finally:
                (wimlib / "block-sidecar-publish").unlink()
            single = b"<WIM><IMAGE INDEX='2'><NAME>single</NAME></IMAGE></WIM>"
            (wimlib / "catalog-fixture.xml").write_bytes(single)
            for label in ["auto-index", "auto-publication-failure"]:
                auto_cache, auto_old = legacy_cache(label)
                if label.endswith("failure"):
                    (wimlib / "block-sidecar-publish").touch()
                try:
                    run("--prepare-auto", iso, auto_cache, root / f"{label}.db", tools)
                    assert_state(root / f"{label}.db", "PRESENT", single)
                finally:
                    if (wimlib / "block-sidecar-publish").exists():
                        (wimlib / "block-sidecar-publish").unlink()
            (wimlib / "catalog-fixture.xml").write_bytes(valid)
            concurrent_cache, concurrent_old = legacy_cache("concurrent-recovery")
            processes = [subprocess.Popen([str(EXE), "--prepare", str(iso), str(concurrent_cache), str(root / f"concurrent-{position}.db"), str(tools)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for position in range(2)]
            for position, process in enumerate(processes):
                stdout, stderr = process.communicate(timeout=30)
                assert process.returncode == 0, stdout + stderr
                assert_state(root / f"concurrent-{position}.db", "PRESENT", valid)
            assert (wimlib / "activity.log").read_text().splitlines().count("apply") == 1, "Metadata-only recovery applied payloads"
        finally:
            for directory in cleanup_roots:
                for path in directory.rglob("*"):
                    os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    print("PASS: catalog bytes/projection/accounting, cold/warm retention, legacy metadata-only recovery/reuse/concurrency, parser and corruption/failure gates; existing image caches unchanged")


if __name__ == "__main__":
    main()
