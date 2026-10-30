#include "control_sets.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "../evidence/sql.hpp"

namespace nylon::services
{
namespace
{

using evidence::check_sql;
using evidence::execute;
using evidence::query_t;

constexpr std::array<const char*, 6> tables = { "service_control_sources", "service_control_context", "service_controlsets", "service_control_select_keys", "service_control_values", "service_control_queries" };

std::string sql(std::string value, std::string_view prefix)
{
	std::size_t position = 0;
	while ((position = value.find('@', position)) != std::string::npos)
	{
		value.replace(position, 1, prefix);
		position += prefix.size();
	}
	return value;
}

sqlite3_int64 scalar(sqlite3* database, const std::string& statement)
{
	query_t query(database, statement.c_str());
	const int code = sqlite3_step(query.get());
	check_sql(database, code);
	if (code != SQLITE_ROW) throw std::runtime_error("Missing control-set scalar result");
	return sqlite3_column_int64(query.get(), 0);
}

std::string text(sqlite3_stmt* statement, int column)
{
	const auto* value = sqlite3_column_text(statement, column);
	return value ? std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(sqlite3_column_bytes(statement, column))) : std::string{};
}

void fill(sqlite3* database, std::string_view prefix)
{
	execute(database, sql("INSERT INTO @service_control_sources SELECT id FROM registry_sources WHERE required_path='WINDOWS\\SYSTEM32\\CONFIG\\SYSTEM' COLLATE NOCASE;", prefix).c_str());
	const auto source_count = scalar(database, sql("SELECT count(*) FROM @service_control_sources;", prefix));
	std::string state = source_count > 1 ? "AMBIGUOUS" : "QUERY_FAILED";
	std::string detail = "SYSTEM source not uniquely parsed; inspect bound registry source decisions";
	std::optional<sqlite3_int64> hive_id;
	std::optional<sqlite3_int64> root_id;
	if (source_count == 1)
	{
		query_t hive(database, sql("SELECT h.id FROM @service_control_sources c JOIN registry_sources s ON s.id=c.source_id JOIN registry_hives h ON h.id=s.hive_id WHERE s.state='PARSED' AND h.name='SYSTEM';", prefix).c_str());
		const auto code = sqlite3_step(hive.get());
		check_sql(database, code);
		if (code == SQLITE_ROW) hive_id = sqlite3_column_int64(hive.get(), 0);
	}
	if (hive_id)
	{
		query_t roots(database, "SELECT id FROM registry_keys WHERE hive_id=?1 AND parent_id IS NULL ORDER BY id;");
		roots.number(1, *hive_id);
		std::size_t count = 0;
		int code = SQLITE_OK;
		while ((code = sqlite3_step(roots.get())) == SQLITE_ROW) { ++count; root_id = sqlite3_column_int64(roots.get(), 0); }
		check_sql(database, code);
		if (count == 1) { state = "PRESENT"; detail = "Successfully parsed SYSTEM hive with one raw root"; }
		else { state = count > 1 ? "AMBIGUOUS" : "QUERY_FAILED"; detail = "SYSTEM raw root missing or ambiguous"; root_id.reset(); }
	}
	query_t context(database, sql("INSERT INTO @service_control_context VALUES(1,?1,?2,?3,?4);", prefix).c_str());
	if (hive_id) context.number(1, *hive_id);
	if (root_id) context.number(2, *root_id);
	context.text(3, state);
	context.text(4, detail);
	context.done();
	if (root_id)
	{
		query_t selects(database, sql("INSERT INTO @service_control_select_keys SELECT id FROM registry_keys WHERE parent_id=?1 AND name='Select' COLLATE NOCASE;", prefix).c_str());
		selects.number(1, *root_id);
		selects.done();
		query_t candidates(database, "SELECT id,name,path FROM registry_keys WHERE parent_id=?1 AND substr(name,1,10)='ControlSet' COLLATE NOCASE ORDER BY id;");
		candidates.number(1, *root_id);
		query_t insert(database, sql("INSERT INTO @service_controlsets VALUES(?1,?2,?3,?4);", prefix).c_str());
		int code = SQLITE_OK;
		while ((code = sqlite3_step(candidates.get())) == SQLITE_ROW)
		{
			const auto name = text(candidates.get(), 1);
			std::uint32_t number = 0;
			bool supported = false;
			if (name.size() == 13)
			{
				const auto parsed = std::from_chars(name.data() + 10, name.data() + 13, number);
				supported = parsed.ec == std::errc{} && parsed.ptr == name.data() + 13 && number > 0 && number <= 999;
			}
			insert.number(1, sqlite3_column_int64(candidates.get(), 0));
			insert.text(2, text(candidates.get(), 2));
			if (supported) insert.number(3, number);
			insert.text(4, supported ? "PRESENT" : "UNSUPPORTED_FORMAT");
			insert.done();
		}
		check_sql(database, code);
	}
	const auto select_count = scalar(database, sql("SELECT count(*) FROM @service_control_select_keys;", prefix));
	for (const auto* role : { "Current", "Default", "LastKnownGood" })
	{
		query_t values(database, sql("SELECT v.id,v.value_type,v.data FROM registry_values v JOIN @service_control_select_keys s ON s.key_id=v.key_id WHERE v.name=?1 COLLATE NOCASE ORDER BY v.id;", prefix).c_str());
		values.text(1, role);
		query_t value_insert(database, sql("INSERT INTO @service_control_values VALUES(?1,?2,?3,?4);", prefix).c_str());
		sqlite3_int64 value_count = 0;
		std::optional<std::uint32_t> number;
		int code = SQLITE_OK;
		while ((code = sqlite3_step(values.get())) == SQLITE_ROW)
		{
			++value_count;
			number.reset();
			if (sqlite3_column_int64(values.get(), 1) == 4 && sqlite3_column_type(values.get(), 2) == SQLITE_BLOB && sqlite3_column_bytes(values.get(), 2) == 4)
			{
				const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(values.get(), 2));
				number = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) | (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
			}
			value_insert.text(1, role);
			value_insert.number(2, sqlite3_column_int64(values.get(), 0));
			if (number) value_insert.number(3, *number);
			value_insert.text(4, number ? "PRESENT" : "PARSE_FAILED");
			value_insert.done();
		}
		check_sql(database, code);
		query_t omissions(database, sql("SELECT count(*) FROM registry_omissions o JOIN @service_control_select_keys s ON s.key_id=o.key_id WHERE o.name IS NULL OR o.name=?1 COLLATE NOCASE;", prefix).c_str());
		omissions.text(1, role);
		check_sql(database, sqlite3_step(omissions.get()));
		const auto omission_count = sqlite3_column_int64(omissions.get(), 0);
		std::string query_state = state;
		std::string query_detail = detail;
		std::optional<sqlite3_int64> target;
		if (state == "PRESENT")
		{
			if (select_count == 0) { query_state = "NOT_PRESENT"; query_detail = "Select key absent in parsed hive"; }
			else if (select_count > 1) { query_state = "AMBIGUOUS"; query_detail = "Multiple raw Select keys"; }
			else if (omission_count) { query_state = "REDACTED_SENSITIVE"; query_detail = "Relevant Select value omission; inspect raw omissions"; }
			else if (value_count == 0) { query_state = "NOT_PRESENT"; query_detail = "Select value absent in parsed key"; }
			else if (value_count > 1) { query_state = "AMBIGUOUS"; query_detail = "Multiple matching raw Select values"; }
			else if (!number) { query_state = "PARSE_FAILED"; query_detail = "Select value is not an exact REG_DWORD"; }
			else if (*number == 0 || *number > 999) { query_state = "UNSUPPORTED_FORMAT"; query_detail = "Select value outside supported positive ControlSetNNN form"; }
			else
			{
				query_t targets(database, sql("SELECT key_id FROM @service_controlsets WHERE set_number=?1 AND state='PRESENT' ORDER BY key_id;", prefix).c_str());
				targets.number(1, *number);
				std::size_t target_count = 0;
				while ((code = sqlite3_step(targets.get())) == SQLITE_ROW) { ++target_count; target = sqlite3_column_int64(targets.get(), 0); }
				check_sql(database, code);
				query_state = target_count == 1 ? "PRESENT" : target_count == 0 ? "UNRESOLVED" : "AMBIGUOUS";
				query_detail = target_count == 1 ? "Exact Select DWORD bound to one discovered control-set key" : "Declared control-set target missing or ambiguous";
				if (target_count != 1) target.reset();
			}
		}
		query_t query_insert(database, sql("INSERT INTO @service_control_queries VALUES(?1,?2,?3,?4,?5,?6);", prefix).c_str());
		query_insert.text(1, role);
		query_insert.text(2, query_state);
		if (target) query_insert.number(3, *target);
		query_insert.number(4, value_count);
		query_insert.number(5, omission_count);
		query_insert.text(6, query_detail);
		query_insert.done();
	}
}

std::string summary_sql(std::string_view prefix)
{
	return sql(R"sql(WITH states AS (
 SELECT state FROM @service_control_context UNION ALL SELECT state FROM @service_controlsets UNION ALL SELECT state FROM @service_control_queries)
SELECT count(*),sum(state IN ('PRESENT','NOT_PRESENT')),sum(state='UNSUPPORTED_FORMAT'),
 sum(state IN ('QUERY_FAILED','PARSE_FAILED')),sum(state='REDACTED_SENSITIVE'),1+sum(state IN ('UNRESOLVED','AMBIGUOUS')) FROM states;)sql", prefix);
}

}

status_t collect_control_sets(database::database_t& database)
{
	try
	{
		auto* handle = database.native_handle();
		execute(handle, R"sql(
CREATE TABLE service_control_sources(source_id INTEGER PRIMARY KEY REFERENCES registry_sources(id));
CREATE TABLE service_control_context(singleton INTEGER PRIMARY KEY CHECK(singleton=1),hive_id INTEGER REFERENCES registry_hives(id),root_key_id INTEGER REFERENCES registry_keys(id),state TEXT NOT NULL,detail TEXT NOT NULL);
CREATE TABLE service_controlsets(key_id INTEGER PRIMARY KEY REFERENCES registry_keys(id),canonical_path TEXT NOT NULL,set_number INTEGER,state TEXT NOT NULL);
CREATE TABLE service_control_select_keys(key_id INTEGER PRIMARY KEY REFERENCES registry_keys(id));
CREATE TABLE service_control_values(role TEXT NOT NULL,value_id INTEGER NOT NULL REFERENCES registry_values(id),number INTEGER,state TEXT NOT NULL,PRIMARY KEY(role,value_id)) WITHOUT ROWID;
CREATE TABLE service_control_queries(role TEXT PRIMARY KEY,state TEXT NOT NULL,target_key_id INTEGER REFERENCES service_controlsets(key_id),source_count INTEGER NOT NULL,omission_count INTEGER NOT NULL,detail TEXT NOT NULL) WITHOUT ROWID;
)sql");
		fill(handle, "main.");
		query_t summary(handle, summary_sql("main.").c_str());
		check_sql(handle, sqlite3_step(summary.get()));
		query_t run(handle, "UPDATE collector_runs SET version='control-set-v1',scope='offline-control-set-selection;service-definitions-pending',state='PARTIAL',started_utc=CURRENT_TIMESTAMP,ended_utc=CURRENT_TIMESTAMP,discovered=?1,represented=?1,parsed=?2,unsupported=?3,failed=?4,redacted=?5,unresolved=?6,discovery_complete=0,representation_complete=1 WHERE collector='services';");
		for (int column = 0; column < 6; ++column) run.number(column + 1, sqlite3_column_int64(summary.get(), column));
		run.done();
		execute(handle, "INSERT INTO evidence_gaps(collector,operation,state,error_code,detail) VALUES('services','service_definitions','UNRESOLVED',0,'Control-set evidence only; service definitions, svchost, configuration interpretation and relationships remain uncollected.');");
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

status_t verify_control_sets(database::database_t& database)
{
	try
	{
		auto* handle = database.native_handle();
		for (const auto* table : tables)
		{
			execute(handle, ("CREATE TEMP TABLE IF NOT EXISTS expected_" + std::string(table) + " AS SELECT * FROM main." + table + " WHERE 0;").c_str());
			execute(handle, ("DELETE FROM temp.expected_" + std::string(table) + ";").c_str());
		}
		fill(handle, "temp.expected_");
		for (const auto* table : tables)
		{
			const auto statement = "SELECT NOT EXISTS(SELECT * FROM main." + std::string(table) + " EXCEPT SELECT * FROM temp.expected_" + table + ") AND NOT EXISTS(SELECT * FROM temp.expected_" + table + " EXCEPT SELECT * FROM main." + table + ");";
			if (scalar(handle, statement) != 1) throw std::runtime_error("Offline control-set provenance/accounting invariant failed: " + std::string(table));
		}
		query_t summary(handle, summary_sql("temp.expected_").c_str());
		check_sql(handle, sqlite3_step(summary.get()));
		query_t run(handle, "SELECT discovered,parsed,unsupported,failed,redacted,unresolved FROM collector_runs WHERE collector='services' AND version='control-set-v1' AND state='PARTIAL' AND represented=discovered AND discovery_complete=0 AND representation_complete=1;");
		if (sqlite3_step(run.get()) != SQLITE_ROW) throw std::runtime_error("Offline control-set collector state invariant failed");
		for (int column = 0; column < 6; ++column)
			if (sqlite3_column_int64(summary.get(), column) != sqlite3_column_int64(run.get(), column)) throw std::runtime_error("Offline control-set counters inconsistent");
		if (scalar(handle, "SELECT count(*) FROM evidence_gaps WHERE collector='services';") != 1 ||
			scalar(handle, "SELECT count(*) FROM evidence_gaps WHERE collector='services' AND operation='service_definitions' AND state='UNRESOLVED';") != 1)
			throw std::runtime_error("Missing service migration gap");
		check_sql(handle, sqlite3_reset(summary.get()));
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

}
