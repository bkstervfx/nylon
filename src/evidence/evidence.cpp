#include "evidence.hpp"
#include "sql.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <stdexcept>
#include <string>

namespace nylon::evidence
{

status_t initialize(database::database_t& database, const media::prepared_input_t& input)
{
	try
	{
		auto* handle = database.native_handle();
		execute(handle, R"sql(
CREATE TABLE audit_identity (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 audit_id TEXT NOT NULL UNIQUE,
 schema_version INTEGER NOT NULL CHECK(schema_version=3),
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
		query_t insert(handle, "INSERT INTO audit_identity(singleton,audit_id,schema_version,source_kind,source_sha256,source_identity_state,selected_index,selected_name,preparation_policy,collector_policy) VALUES(1,?1,3,?2,?3,?4,?5,?6,?7,'namespace-v1;registry-legacy');");
		insert.text(1, identity);
		insert.text(2, input.source_kind);
		if (!input.iso_sha256.empty()) insert.text(3, input.iso_sha256);
		insert.text(4, input.source_kind == "iso" ? "SOURCE_MEDIA_SHA256" : "WEAK_RUN_SCOPED_NO_CONTAINER");
		if (input.image_index != 0) insert.number(5, input.image_index);
		insert.text(6, input.image_name);
		insert.text(7, input.source_kind == "iso" ? "7zip-26.05;wimlib-1.14.5;--no-acls --no-attributes --rpfix" : "supplied-expanded-tree;no-preparation");
		insert.done();
		query_t collector(handle, "INSERT INTO collector_runs(collector,version,audit_id,scope) VALUES(?1,?2,?3,?4);");
		for (const auto* family : { "source_media", "filesystem", "registry", "servicing", "components", "features_capabilities", "appx", "services", "tasks", "drivers", "pe_signatures", "com_winrt_shell", "startup", "boot_recovery", "configuration", "certificates", "relationships" })
		{
			collector.text(1, family);
			collector.text(2, std::string_view(family) == "filesystem" ? "namespace-v1" : "not-migrated");
			collector.text(3, identity);
			collector.text(4, std::string_view(family) == "filesystem" ? "selected-image-working-namespace" : "required-family");
			collector.done();
		}
		query_t metadata(handle, "INSERT INTO source_metadata_availability VALUES(?1,'NOT_EXPOSED','ORIGINAL_SOURCE',?2);");
		for (const auto* field : { "security_descriptors", "original_attributes", "original_timestamps", "original_reparse_data", "named_streams", "extended_attributes", "sparse_compression", "original_hardlink_identity", "container_resource_identity", "image_catalog", "setup_boot_media" })
		{
			metadata.text(1, field);
			metadata.text(2, "Not collected authoritatively by namespace-v1; working-view observations are separate, not original metadata.");
			metadata.done();
		}
		auto status = database.meta("audit_id", identity);
		if (!status.ok()) return status;
		return database.meta("schema_version", "3");
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

status_t verify(database::database_t& database)
{
	try
	{
		auto* handle = database.native_handle();
		query_t check(handle, R"sql(
SELECT
 (SELECT count(*) FROM audit_identity WHERE schema_version=3)=1
 AND NOT EXISTS(SELECT 1 FROM collector_runs c,audit_identity a WHERE c.audit_id<>a.audit_id)
 AND NOT EXISTS(SELECT 1 FROM fs_entries f LEFT JOIN fs_entries p ON p.id=f.parent_id
                WHERE f.parent_id IS NOT NULL AND (p.id IS NULL OR p.kind<>'DIRECTORY'))
 AND (SELECT count(*) FROM fs_entries WHERE parent_id IS NULL)=1
 AND NOT EXISTS(SELECT 1 FROM fs_entries WHERE children_state='PENDING')
 AND NOT EXISTS(SELECT 1 FROM collector_runs WHERE collector='filesystem' AND
   (state NOT IN ('COMPLETE','COMPLETE_WITH_GAPS') OR discovered<>represented
    OR represented<>(SELECT count(*) FROM fs_entries) OR representation_complete<>1
    OR (discovery_complete=1 AND EXISTS(SELECT 1 FROM evidence_gaps WHERE collector='filesystem' AND operation='enumerate'))))
 AND (SELECT count(*) FROM collector_runs WHERE collector='filesystem')=1;
)sql");
		const auto code = sqlite3_step(check.get());
		check_sql(handle, code);
		if (code != SQLITE_ROW || sqlite3_column_int(check.get(), 0) != 1)
			return status_t::failure("Raw namespace accounting invariant failed");
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
