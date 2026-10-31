#include "evidence.hpp"
#include "sql.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../platform/win32.hpp"
#include "../services/control_sets.hpp"

namespace nylon::evidence
{

namespace
{

void verify_namespace_keys(sqlite3* database)
{
	query_t entries(database, "SELECT path_utf16,path,comparison_key,encoding_state FROM fs_entries ORDER BY id;");
	int code = SQLITE_OK;
	while ((code = sqlite3_step(entries.get())) == SQLITE_ROW)
	{
		const auto size = sqlite3_column_bytes(entries.get(), 0);
		if (sqlite3_column_type(entries.get(), 0) != SQLITE_BLOB || size % sizeof(wchar_t) != 0)
			throw std::runtime_error("Invalid raw UTF-16 namespace identity");
		std::wstring path(static_cast<std::size_t>(size) / sizeof(wchar_t), L'\0');
		if (size) std::memcpy(path.data(), sqlite3_column_blob(entries.get(), 0), static_cast<std::size_t>(size));
		auto text = platform::utf8(path);
		auto key = platform::path_comparison_key(path);
		auto matches = [&](int column, const result_t<std::string>& expected)
		{
			if (!expected.ok()) return sqlite3_column_type(entries.get(), column) == SQLITE_NULL;
			if (sqlite3_column_type(entries.get(), column) != SQLITE_TEXT) return false;
			const auto* raw = sqlite3_column_text(entries.get(), column);
			return std::string_view(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(sqlite3_column_bytes(entries.get(), column))) == expected.value();
		};
		const auto* state = sqlite3_column_text(entries.get(), 3);
		if (!matches(1, text) || !matches(2, key) || !state ||
			std::string_view(reinterpret_cast<const char*>(state)) != (text.ok() && key.ok() ? "PRESENT" : "UNRESOLVED"))
			throw std::runtime_error("Namespace text/comparison key does not match original UTF-16 identity");
	}
	check_sql(database, code);
}

}

status_t initialize(database::database_t& database, const media::prepared_input_t& input)
{
	try
	{
		auto* handle = database.native_handle();
		execute(handle, R"sql(
CREATE TABLE audit_identity (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 audit_id TEXT NOT NULL UNIQUE,
 schema_version INTEGER NOT NULL CHECK(schema_version=7),
 source_kind TEXT NOT NULL,
 source_sha256 TEXT,
 source_identity_state TEXT NOT NULL,
 selected_index INTEGER,
 selected_name TEXT NOT NULL,
 preparation_policy TEXT NOT NULL,
 collector_policy TEXT NOT NULL,
 started_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 state TEXT NOT NULL DEFAULT 'RUNNING'
);
CREATE TABLE collector_runs (
 collector TEXT PRIMARY KEY,
 version TEXT NOT NULL,
 audit_id TEXT NOT NULL,
 scope TEXT NOT NULL,
 state TEXT NOT NULL DEFAULT 'PENDING',
 started_utc TEXT,
 ended_utc TEXT,
 checkpoint TEXT,
 discovered INTEGER NOT NULL DEFAULT 0 CHECK(discovered>=0),
 represented INTEGER NOT NULL DEFAULT 0 CHECK(represented>=0 AND represented<=discovered),
 parsed INTEGER NOT NULL DEFAULT 0 CHECK(parsed>=0),
 unsupported INTEGER NOT NULL DEFAULT 0 CHECK(unsupported>=0),
 failed INTEGER NOT NULL DEFAULT 0 CHECK(failed>=0),
 redacted INTEGER NOT NULL DEFAULT 0 CHECK(redacted>=0),
 unresolved INTEGER NOT NULL DEFAULT 0 CHECK(unresolved>=0),
 discovery_complete INTEGER NOT NULL DEFAULT 0 CHECK(discovery_complete IN (0,1)),
 representation_complete INTEGER NOT NULL DEFAULT 0 CHECK(representation_complete IN (0,1))
) WITHOUT ROWID;
CREATE TABLE fs_entries (
 id INTEGER PRIMARY KEY,
 parent_id INTEGER REFERENCES fs_entries(id),
 path TEXT,
 path_utf16 BLOB NOT NULL UNIQUE,
 comparison_key TEXT,
 kind TEXT NOT NULL CHECK(kind IN ('DIRECTORY','FILE','REPARSE')),
 provenance TEXT NOT NULL DEFAULT 'WORKING_VIEW',
 encoding_state TEXT NOT NULL,
 metadata_state TEXT NOT NULL,
 children_state TEXT NOT NULL,
 size INTEGER,
 creation_time BLOB,
 access_time BLOB,
 write_time BLOB,
 attributes INTEGER,
 volume_serial INTEGER,
 file_index BLOB,
 link_count INTEGER,
 reparse_tag INTEGER,
 reparse_data BLOB,
 reparse_state TEXT NOT NULL,
 error_code INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX fs_parent ON fs_entries(parent_id);
CREATE INDEX fs_comparison ON fs_entries(comparison_key);
CREATE INDEX fs_kind ON fs_entries(kind,id);
CREATE TABLE evidence_gaps (
 id INTEGER PRIMARY KEY,
 collector TEXT NOT NULL REFERENCES collector_runs(collector),
 source_entry_id INTEGER REFERENCES fs_entries(id),
 operation TEXT NOT NULL,
 state TEXT NOT NULL,
 error_code INTEGER NOT NULL,
 detail TEXT NOT NULL
);
CREATE INDEX gaps_source ON evidence_gaps(source_entry_id);
CREATE TABLE registry_hives (
 id INTEGER PRIMARY KEY,
 name TEXT NOT NULL,
 source_path TEXT NOT NULL,
 required INTEGER NOT NULL
);
CREATE TABLE registry_sources (
 id INTEGER PRIMARY KEY,
 entry_id INTEGER UNIQUE REFERENCES fs_entries(id),
 required_path TEXT,
 required INTEGER NOT NULL,
 state TEXT NOT NULL,
 hive_id INTEGER UNIQUE REFERENCES registry_hives(id),
 error_code INTEGER NOT NULL,
 detail TEXT NOT NULL
);
CREATE TABLE source_metadata_availability (
 field TEXT PRIMARY KEY,
 state TEXT NOT NULL,
 provenance TEXT NOT NULL,
 detail TEXT NOT NULL
) WITHOUT ROWID;
)sql");
		std::array<unsigned char, 32> random{};
		const auto code = BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (!BCRYPT_SUCCESS(code)) return status_t::failure("Unable to generate audit identity", static_cast<std::uint32_t>(code));
		constexpr char hex[] = "0123456789abcdef";
		std::string identity;
		for (const auto byte : random)
		{
			identity.push_back(hex[byte >> 4U]);
			identity.push_back(hex[byte & 15U]);
		}
		query_t insert(handle, "INSERT INTO audit_identity(singleton,audit_id,schema_version,source_kind,source_sha256,source_identity_state,selected_index,selected_name,preparation_policy,collector_policy) VALUES(1,?1,7,?2,?3,?4,?5,?6,?7,'namespace-v1;registry-inventory-v1;sensitive-omission-v1;catalog-v1;control-set-v1;service-inventory-v1;service-value-v1');");
		insert.text(1, identity);
		insert.text(2, input.source_kind);
		if (!input.iso_sha256.empty()) insert.text(3, input.iso_sha256);
		insert.text(4, input.source_kind == "iso" ? "SOURCE_MEDIA_SHA256" : "WEAK_RUN_SCOPED_NO_CONTAINER");
		if (input.image_index != 0) insert.number(5, input.image_index);
		insert.text(6, input.image_name);
		insert.text(7, input.source_kind == "iso" ? "7zip-26.05;wimlib-1.14.5;--no-acls --no-attributes --rpfix" : "supplied-expanded-tree;no-preparation");
		insert.done();
		query_t collector(handle, "INSERT INTO collector_runs(collector,version,audit_id,scope) VALUES(?1,?2,?3,?4);");
		for (const auto family : required_collectors)
		{
			collector.text(1, family);
			collector.text(2, std::string_view(family) == "filesystem" ? "namespace-v1" : "not-migrated");
			collector.text(3, identity);
			collector.text(4, std::string_view(family) == "filesystem" ? "selected-image-working-namespace" : "required-family");
			collector.done();
		}
		query_t metadata(handle, "INSERT INTO source_metadata_availability VALUES(?1,'NOT_EXPOSED','ORIGINAL_SOURCE',?2);");
		for (const auto field : original_metadata_fields)
		{
			metadata.text(1, field);
			metadata.text(2, "Not collected authoritatively by namespace-v1; working-view observations are separate, not original metadata.");
			metadata.done();
		}
		auto status = database.meta("audit_id", identity);
		if (!status.ok()) return status;
		status = initialize_source_media(database, input);
		if (!status.ok()) return status;
		return database.meta("schema_version", "7");
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

status_t verify(database::database_t& database)
{
	try
	{
		auto* handle = database.native_handle();
		verify_namespace_keys(handle);
		query_t check(handle, R"sql(
SELECT
 (SELECT count(*) FROM audit_identity WHERE schema_version=7)=1
 AND NOT EXISTS(SELECT 1 FROM collector_runs c,audit_identity a WHERE c.audit_id<>a.audit_id)
 AND NOT EXISTS(SELECT 1 FROM fs_entries f LEFT JOIN fs_entries p ON p.id=f.parent_id
                WHERE f.parent_id IS NOT NULL AND (p.id IS NULL OR p.kind<>'DIRECTORY'))
 AND (SELECT count(*) FROM fs_entries WHERE parent_id IS NULL)=1
 AND NOT EXISTS(SELECT 1 FROM fs_entries WHERE children_state='PENDING')
 AND NOT EXISTS(SELECT 1 FROM fs_entries f WHERE
   (f.kind='DIRECTORY' AND f.children_state<>'COMPLETE' AND NOT EXISTS(
     SELECT 1 FROM evidence_gaps g WHERE g.collector='filesystem' AND g.source_entry_id=f.id AND g.operation='enumerate'))
   OR (f.kind<>'DIRECTORY' AND f.children_state<>'NOT_APPLICABLE')
   OR (f.encoding_state<>'PRESENT' AND NOT EXISTS(
     SELECT 1 FROM evidence_gaps g WHERE g.collector='filesystem' AND g.source_entry_id=f.id AND g.operation='encode_path'))
   OR (f.metadata_state<>'PRESENT' AND NOT EXISTS(
     SELECT 1 FROM evidence_gaps g WHERE g.collector='filesystem' AND g.source_entry_id=f.id AND g.operation='metadata')))
 AND NOT EXISTS(SELECT 1 FROM fs_entries f JOIN fs_entries p ON p.id=f.parent_id
   WHERE f.path IS NOT NULL AND p.path IS NOT NULL AND
   ((p.path='' AND (f.path='' OR instr(f.path,'\')<>0)) OR
    (p.path<>'' AND (substr(f.path,1,length(p.path)+1)<>p.path||'\'
      OR length(f.path)<=length(p.path)+1 OR instr(substr(f.path,length(p.path)+2),'\')<>0))))
 AND NOT EXISTS(SELECT 1 FROM collector_runs WHERE collector='filesystem' AND
   (state NOT IN ('COMPLETE','COMPLETE_WITH_GAPS') OR discovered<>represented
    OR represented<>(SELECT count(*) FROM fs_entries) OR representation_complete<>1
    OR parsed<>(SELECT count(*) FROM fs_entries WHERE metadata_state='PRESENT')
    OR failed<>(SELECT count(DISTINCT source_entry_id) FROM evidence_gaps WHERE collector='filesystem')
    OR unresolved<>(SELECT count(*) FROM evidence_gaps WHERE collector='filesystem')
    OR discovery_complete<>NOT EXISTS(SELECT 1 FROM evidence_gaps WHERE collector='filesystem' AND operation='enumerate')
    OR (state='COMPLETE')<>NOT EXISTS(SELECT 1 FROM evidence_gaps WHERE collector='filesystem')))
 AND (SELECT count(*) FROM collector_runs WHERE collector='filesystem')=1;
)sql");
		const auto code = sqlite3_step(check.get());
		check_sql(handle, code);
		if (code != SQLITE_ROW || sqlite3_column_int(check.get(), 0) != 1)
			return status_t::failure("Raw namespace accounting invariant failed");
		query_t family(handle, "SELECT count(*) FROM collector_runs WHERE collector=?1 AND audit_id=(SELECT audit_id FROM audit_identity) AND state IN ('PENDING','PARTIAL','COMPLETE','COMPLETE_WITH_GAPS','FAILED','STALLED');");
		for (const auto name : required_collectors)
		{
			family.text(1, name);
			const auto family_code = sqlite3_step(family.get());
			check_sql(handle, family_code);
			if (family_code != SQLITE_ROW || sqlite3_column_int(family.get(), 0) != 1)
				return status_t::failure("Required collector accounting row missing, running or incompatible: " + std::string(name));
			check_sql(handle, sqlite3_reset(family.get()));
			check_sql(handle, sqlite3_clear_bindings(family.get()));
		}
		query_t migrated(handle, "SELECT state FROM collector_runs WHERE collector='registry';");
		check_sql(handle, sqlite3_step(migrated.get()));
		const auto* state = sqlite3_column_text(migrated.get(), 0);
		if (state && std::string_view(reinterpret_cast<const char*>(state)) != "PENDING")
		{
			query_t registry(handle, R"sql(
SELECT
 NOT EXISTS(SELECT 1 FROM fs_entries f LEFT JOIN registry_sources s ON s.entry_id=f.id WHERE f.kind='FILE' AND s.id IS NULL)
 AND (SELECT count(DISTINCT required_path) FROM registry_sources)=9
 AND NOT EXISTS(SELECT 1 FROM registry_sources s LEFT JOIN registry_hives h ON h.id=s.hive_id
   WHERE (s.state='PARSED' AND h.id IS NULL) OR (s.state<>'PARSED' AND s.hive_id IS NOT NULL))
 AND NOT EXISTS(SELECT 1 FROM registry_sources s JOIN registry_hives h ON h.id=s.hive_id
   LEFT JOIN fs_entries f ON f.id=s.entry_id WHERE f.id IS NULL OR h.source_path<>f.path)
 AND NOT EXISTS(SELECT 1 FROM registry_hives h LEFT JOIN registry_sources s ON s.hive_id=h.id WHERE s.id IS NULL)
 AND NOT EXISTS(SELECT 1 FROM registry_keys k LEFT JOIN registry_hives h ON h.id=k.hive_id WHERE h.id IS NULL)
 AND NOT EXISTS(SELECT 1 FROM registry_keys k LEFT JOIN registry_keys p ON p.id=k.parent_id WHERE k.parent_id IS NOT NULL AND (p.id IS NULL OR p.hive_id<>k.hive_id))
 AND NOT EXISTS(SELECT 1 FROM registry_values v LEFT JOIN registry_keys k ON k.id=v.key_id WHERE k.id IS NULL)
 AND NOT EXISTS(SELECT 1 FROM registry_omissions o LEFT JOIN registry_keys k ON k.id=o.key_id WHERE k.id IS NULL)
 AND NOT EXISTS(SELECT 1 FROM registry_hives WHERE name IN ('SAM','SECURITY'))
 AND NOT EXISTS(SELECT 1 FROM collector_runs WHERE collector='registry' AND
   (state NOT IN ('COMPLETE','COMPLETE_WITH_GAPS') OR representation_complete<>1
    OR represented<>discovered OR represented<>(SELECT count(*) FROM registry_sources)
    OR parsed<>(SELECT count(*) FROM registry_sources WHERE state='PARSED')
    OR failed<>(SELECT count(*) FROM registry_sources WHERE state IN ('PARSE_FAILED','QUERY_FAILED','ACCESS_DENIED','MISSING_REQUIRED'))
    OR redacted<>(SELECT count(*) FROM registry_sources WHERE state IN ('REDACTED_SENSITIVE','REDACTED_POLICY'))
    OR unsupported<>(SELECT count(*) FROM registry_sources WHERE state='UNSUPPORTED_FORMAT')
    OR unresolved<>(SELECT count(*) FROM registry_sources WHERE state='UNRESOLVED')
    OR discovery_complete<>((SELECT discovery_complete FROM collector_runs WHERE collector='filesystem') AND
      NOT EXISTS(SELECT 1 FROM registry_sources WHERE state IN ('QUERY_FAILED','ACCESS_DENIED','UNRESOLVED')))));
)sql");
			const auto registry_code = sqlite3_step(registry.get());
			check_sql(handle, registry_code);
			if (registry_code != SQLITE_ROW || sqlite3_column_int(registry.get(), 0) != 1)
				return status_t::failure("Inventory registry accounting/provenance invariant failed");
			auto controls = services::verify_control_sets(database);
			if (!controls.ok()) return controls;
		}
		auto media_status = verify_source_media(database);
		if (!media_status.ok()) return media_status;
		query_t foreign_keys(handle, "PRAGMA foreign_key_check;");
		const auto foreign_key_code = sqlite3_step(foreign_keys.get());
		check_sql(handle, foreign_key_code);
		if (foreign_key_code != SQLITE_DONE) return status_t::failure("Evidence foreign-key invariant failed");
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

status_t finalize(database::database_t& database)
{
	auto status = verify(database);
	if (!status.ok()) return status;
	try
	{
		execute(database.native_handle(), "UPDATE audit_identity SET state='PARTIAL';");
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

}
