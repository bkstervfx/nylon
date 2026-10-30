#include "evidence.hpp"
#include "sql.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "../platform/hash.hpp"

namespace nylon::evidence
{
namespace
{

std::string column_text(sqlite3_stmt* statement, int column)
{
	const auto* text = sqlite3_column_text(statement, column);
	return text ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(statement, column))) : std::string{};
}

void require(bool condition)
{
	if (!condition) throw std::runtime_error("Source-media catalog accounting invariant failed");
}

}

status_t initialize_source_media(database::database_t& database, const media::prepared_input_t& input)
{
	try
	{
		auto* handle = database.native_handle();
		execute(handle, R"sql(
CREATE TABLE source_catalog (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 audit_id TEXT NOT NULL REFERENCES audit_identity(audit_id),
 parser_version TEXT NOT NULL CHECK(parser_version='wim-xml-v1'),
 state TEXT NOT NULL CHECK(state IN ('PRESENT','NOT_EXPOSED','QUERY_FAILED','PARSE_FAILED','UNRESOLVED')),
 detail TEXT NOT NULL,
 container_path TEXT NOT NULL,
 xml_sha256 TEXT,
 xml BLOB,
 image_count INTEGER NOT NULL CHECK(image_count>=0)
);
CREATE TABLE source_images (
 image_index INTEGER PRIMARY KEY CHECK(image_index>0),
 catalog_id INTEGER NOT NULL DEFAULT 1 REFERENCES source_catalog(singleton),
 selected INTEGER NOT NULL CHECK(selected IN (0,1)),
 field_count INTEGER NOT NULL CHECK(field_count>=0)
);
CREATE TABLE source_image_fields (
 image_index INTEGER NOT NULL REFERENCES source_images(image_index),
 ordinal INTEGER NOT NULL CHECK(ordinal>=0),
 path TEXT NOT NULL,
 value TEXT NOT NULL,
 PRIMARY KEY(image_index,ordinal)
) WITHOUT ROWID;
)sql");
		query_t catalog(handle, "INSERT INTO source_catalog SELECT 1,audit_id,'wim-xml-v1',?1,?2,?3,?4,?5,?6 FROM audit_identity;");
		catalog.text(1, input.catalog_state);
		catalog.text(2, input.catalog_detail);
		catalog.text(3, input.install_container_path);
		const auto count = input.catalog ? input.catalog->images.size() : 0;
		if (input.catalog)
		{
			catalog.text(4, input.catalog_sha256);
			catalog.blob(5, input.catalog->xml.data(), input.catalog->xml.size());
		}
		catalog.number(6, static_cast<sqlite3_int64>(count));
		catalog.done();
		query_t image(handle, "INSERT INTO source_images(image_index,selected,field_count) VALUES(?1,?2,?3);");
		query_t field(handle, "INSERT INTO source_image_fields VALUES(?1,?2,?3,?4);");
		if (input.catalog) for (const auto& item : input.catalog->images)
		{
			image.number(1, item.index);
			image.number(2, item.index == input.image_index);
			image.number(3, static_cast<sqlite3_int64>(item.fields.size()));
			image.done();
			for (std::size_t ordinal = 0; ordinal < item.fields.size(); ++ordinal)
			{
				field.number(1, item.index);
				field.number(2, static_cast<sqlite3_int64>(ordinal));
				field.text(3, item.fields[ordinal].path);
				field.text(4, item.fields[ordinal].value);
				field.done();
			}
		}
		query_t availability(handle, "UPDATE source_metadata_availability SET state=?1,detail=?2 WHERE field='image_catalog';");
		availability.text(1, input.catalog_state);
		availability.text(2, input.catalog_detail);
		availability.done();
		query_t gap(handle, "INSERT INTO evidence_gaps(collector,operation,state,error_code,detail) VALUES('source_media',?1,?2,0,?3);");
		gap.text(1, "remaining_source_media");
		gap.text(2, "UNRESOLVED");
		gap.text(3, "Setup/boot media and original per-file metadata are not collected; install catalog alone is not full source-media coverage.");
		gap.done();
		if (input.catalog_state != "PRESENT")
		{
			gap.text(1, "image_catalog");
			gap.text(2, input.catalog_state);
			gap.text(3, input.catalog_detail);
			gap.done();
		}
		query_t run(handle, R"sql(
UPDATE collector_runs SET version='catalog-v1',scope='original-install-catalog;remaining-media-uncollected',
 state='PARTIAL',started_utc=CURRENT_TIMESTAMP,ended_utc=CURRENT_TIMESTAMP,
 discovered=?1,represented=?1,parsed=?2,failed=?3,unresolved=?4,
 discovery_complete=0,representation_complete=1 WHERE collector='source_media';
)sql");
		run.number(1, static_cast<sqlite3_int64>(1 + count));
		run.number(2, input.catalog && input.catalog_state != "PARSE_FAILED" ? static_cast<sqlite3_int64>(count) : 0);
		run.number(3, input.catalog_state == "PARSE_FAILED" || input.catalog_state == "QUERY_FAILED");
		run.number(4, input.catalog_state == "PRESENT" ? 1 : 2);
		run.done();
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

status_t verify_source_media(database::database_t& database)
{
	try
	{
		auto* handle = database.native_handle();
		query_t invariants(handle, R"sql(
SELECT (SELECT count(*) FROM source_catalog)=1
 AND NOT EXISTS(SELECT 1 FROM source_catalog c,audit_identity a WHERE c.audit_id<>a.audit_id)
 AND (SELECT count(*) FROM source_metadata_availability)=11
 AND (SELECT count(*) FROM source_metadata_availability m JOIN source_catalog c ON m.field='image_catalog' AND m.state=c.state AND m.provenance='ORIGINAL_SOURCE')=1
 AND (SELECT count(*) FROM evidence_gaps WHERE collector='source_media' AND operation='remaining_source_media' AND state='UNRESOLVED')=1
 AND (SELECT count(*) FROM evidence_gaps WHERE collector='source_media')=(SELECT CASE WHEN state='PRESENT' THEN 1 ELSE 2 END FROM source_catalog)
 AND (SELECT count(*) FROM source_images)=(SELECT image_count FROM source_catalog)
 AND NOT EXISTS(SELECT 1 FROM source_images i WHERE i.catalog_id<>1 OR i.field_count<>(SELECT count(*) FROM source_image_fields f WHERE f.image_index=i.image_index))
 AND NOT EXISTS(SELECT 1 FROM source_image_fields f LEFT JOIN source_images i ON i.image_index=f.image_index WHERE i.image_index IS NULL)
 AND (SELECT count(*) FROM collector_runs r,source_catalog c WHERE r.collector='source_media'
   AND r.version='catalog-v1' AND r.state='PARTIAL' AND r.discovery_complete=0 AND r.representation_complete=1
   AND r.discovered=c.image_count+1 AND r.represented=r.discovered
   AND r.parsed=CASE WHEN c.state='PARSE_FAILED' THEN 0 ELSE c.image_count END
   AND r.failed=CASE WHEN c.state IN ('PARSE_FAILED','QUERY_FAILED') THEN 1 ELSE 0 END
   AND r.unresolved=CASE WHEN c.state='PRESENT' THEN 1 ELSE 2 END AND r.unsupported=0 AND r.redacted=0)=1
 AND NOT EXISTS(SELECT 1 FROM source_catalog c WHERE c.state<>'PRESENT' AND NOT EXISTS(
   SELECT 1 FROM evidence_gaps g WHERE g.collector='source_media' AND g.operation='image_catalog' AND g.state=c.state));
)sql");
		check_sql(handle, sqlite3_step(invariants.get()));
		require(sqlite3_column_int(invariants.get(), 0) == 1);
		query_t metadata(handle, "SELECT state,provenance FROM source_metadata_availability WHERE field=?1;");
		for (const auto name : original_metadata_fields)
		{
			metadata.text(1, name);
			require(sqlite3_step(metadata.get()) == SQLITE_ROW);
			require(column_text(metadata.get(), 1) == "ORIGINAL_SOURCE");
			if (name != "image_catalog") require(column_text(metadata.get(), 0) == "NOT_EXPOSED");
			check_sql(handle, sqlite3_reset(metadata.get()));
		}
		query_t catalog(handle, "SELECT c.state,c.xml_sha256,c.xml,a.selected_index,a.source_kind FROM source_catalog c,audit_identity a;");
		check_sql(handle, sqlite3_step(catalog.get()));
		const auto state = column_text(catalog.get(), 0);
		const auto hash = column_text(catalog.get(), 1);
		if (sqlite3_column_type(catalog.get(), 2) == SQLITE_NULL)
		{
			require(state == "NOT_EXPOSED" || state == "QUERY_FAILED");
			require(hash.empty());
			query_t empty(handle, "SELECT (SELECT count(*) FROM source_images)=0;");
			check_sql(handle, sqlite3_step(empty.get()));
			require(sqlite3_column_int(empty.get(), 0) == 1);
			return status_t::success();
		}
		require(column_text(catalog.get(), 4) == "iso");
		const auto size = sqlite3_column_bytes(catalog.get(), 2);
		require(size > 0 && static_cast<std::size_t>(size) <= media::max_catalog_bytes);
		const auto* raw = static_cast<const std::byte*>(sqlite3_column_blob(catalog.get(), 2));
		std::vector<std::byte> bytes(raw, raw + size);
		auto digest = platform::sha256_bytes(bytes);
		require(digest.ok() && digest.value() == hash);
		auto parsed = media::parse_catalog_bytes(std::move(bytes));
		if (!parsed.ok())
		{
			require(state == "PARSE_FAILED");
			query_t empty(handle, "SELECT (SELECT count(*) FROM source_images)=0;");
			check_sql(handle, sqlite3_step(empty.get()));
			require(sqlite3_column_int(empty.get(), 0) == 1);
			return status_t::success();
		}
		const auto selected = sqlite3_column_int64(catalog.get(), 3);
		const bool found = std::any_of(parsed.value().images.begin(), parsed.value().images.end(), [&](const auto& image) { return image.index == selected; });
		require(state == (found ? "PRESENT" : "UNRESOLVED"));
		query_t image(handle, "SELECT selected,field_count FROM source_images WHERE image_index=?1;");
		query_t field(handle, "SELECT ordinal,path,value FROM source_image_fields WHERE image_index=?1 ORDER BY ordinal;");
		std::size_t image_count = 0;
		for (const auto& item : parsed.value().images)
		{
			++image_count;
			image.number(1, item.index);
			require(sqlite3_step(image.get()) == SQLITE_ROW);
			require(sqlite3_column_int(image.get(), 0) == (item.index == selected));
			require(sqlite3_column_int64(image.get(), 1) == static_cast<sqlite3_int64>(item.fields.size()));
			check_sql(handle, sqlite3_reset(image.get()));
			field.number(1, item.index);
			std::size_t ordinal = 0;
			for (const auto& value : item.fields)
			{
				require(sqlite3_step(field.get()) == SQLITE_ROW);
				require(sqlite3_column_int64(field.get(), 0) == static_cast<sqlite3_int64>(ordinal++));
				require(column_text(field.get(), 1) == value.path && column_text(field.get(), 2) == value.value);
			}
			require(sqlite3_step(field.get()) == SQLITE_DONE);
			check_sql(handle, sqlite3_reset(field.get()));
		}
		query_t count(handle, "SELECT image_count FROM source_catalog;");
		check_sql(handle, sqlite3_step(count.get()));
		require(sqlite3_column_int64(count.get(), 0) == static_cast<sqlite3_int64>(image_count));
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

}
