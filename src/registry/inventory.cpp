#include "collector.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <set>
#include <string>

#include "../evidence/sql.hpp"

namespace nylon::registry
{
namespace
{

struct stock_hive_t
{
	const char* path;
	const char* name;
	bool required;
	bool sensitive;
};

constexpr stock_hive_t stock[] = {
	{ "WINDOWS\\SYSTEM32\\CONFIG\\SOFTWARE", "SOFTWARE", true, false },
	{ "WINDOWS\\SYSTEM32\\CONFIG\\SYSTEM", "SYSTEM", true, false },
	{ "WINDOWS\\SYSTEM32\\CONFIG\\COMPONENTS", "COMPONENTS", true, false },
	{ "WINDOWS\\SYSTEM32\\CONFIG\\DEFAULT", "DEFAULT", true, false },
	{ "WINDOWS\\SYSTEM32\\CONFIG\\SAM", "SAM", true, true },
	{ "WINDOWS\\SYSTEM32\\CONFIG\\SECURITY", "SECURITY", true, true },
	{ "BOOT\\BCD", "BCD", false, false },
	{ "EFI\\MICROSOFT\\BOOT\\BCD", "BCD_EFI", false, false },
	{ "WINDOWS\\APPCOMPAT\\PROGRAMS\\AMCACHE.HVE", "AMCACHE", false, false }
};

std::string text(sqlite3_stmt* statement, int column)
{
	const auto* value = sqlite3_column_text(statement, column);
	return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, column)) : std::string{};
}

}

status_t scan_all(const image::image_t& image, database::database_t& database,
	progress::registry_state_t* progress_state)
{
	try
	{
		auto* handle = database.native_handle();
		auto created = bulk_writer_t::create(database);
		if (!created.ok()) return status_t::failure(created.error().message, created.error().code);
		auto writer = std::move(created).value();
		evidence::execute(handle, "UPDATE collector_runs SET version='registry-inventory-v1',scope='inventory-file-header-probes-and-stock-source-queries',state='RUNNING',started_utc=CURRENT_TIMESTAMP WHERE collector='registry';");
		evidence::query_t source(handle, "INSERT INTO registry_sources(entry_id,required_path,required,state,hive_id,error_code,detail) VALUES(?1,?2,?3,?4,?5,?6,?7);");
		evidence::query_t gap(handle, "INSERT INTO evidence_gaps(collector,source_entry_id,operation,state,error_code,detail) VALUES('registry',?1,'registry_source',?2,?3,?4);");
		auto record = [&](std::optional<sqlite3_int64> entry, const stock_hive_t* known,
			std::string_view state, DWORD code, std::string_view detail, bool linked_hive = false)
		{
			if (entry) source.number(1, *entry);
			if (known) source.text(2, known->path);
			source.number(3, known && known->required ? 1 : 0);
			source.text(4, state);
			if (linked_hive)
			{
				evidence::query_t latest(handle, "SELECT max(id) FROM registry_hives;");
				evidence::check_sql(handle, sqlite3_step(latest.get()));
				source.number(5, sqlite3_column_int64(latest.get(), 0));
			}
			source.number(6, code);
			source.text(7, detail);
			source.done();
			if (state != "PARSED" && state != "NOT_REGF" && state != "NOT_PRESENT")
			{
				if (entry) gap.number(1, *entry);
				gap.text(2, state);
				gap.number(3, code);
				gap.text(4, detail);
				gap.done();
			}
		};
		std::set<std::string> seen;
		evidence::query_t files(handle, "SELECT id,path_utf16,comparison_key,volume_serial,file_index FROM fs_entries WHERE kind='FILE' ORDER BY parent_id,id;");
		int code = SQLITE_OK;
		while ((code = sqlite3_step(files.get())) == SQLITE_ROW)
		{
			const auto entry_id = sqlite3_column_int64(files.get(), 0);
			const auto comparison = text(files.get(), 2);
			const stock_hive_t* known = nullptr;
			for (const auto& candidate : stock) if (comparison == candidate.path) { known = &candidate; seen.insert(comparison); break; }
			if (known)
			{
				evidence::query_t matches(handle, "SELECT count(*) FROM fs_entries WHERE comparison_key=?1;");
				matches.text(1, comparison);
				evidence::check_sql(handle, sqlite3_step(matches.get()));
				if (sqlite3_column_int64(matches.get(), 0) != 1)
				{
					record(entry_id, known, "UNRESOLVED", 0, "Ambiguous case-normalized stock hive identity; no payload imported");
					continue;
				}
			}
			const auto separator = comparison.find_last_of('\\');
			const auto basename = comparison.substr(separator == std::string::npos ? 0 : separator + 1);
			if ((known && known->sensitive) || basename == "SAM" || basename == "SECURITY")
			{
				record(entry_id, known, "REDACTED_SENSITIVE", 0, "Structural presence only; sensitive hive payload was not opened");
				continue;
			}
			if (basename == "NTUSER.DAT" || basename == "USRCLASS.DAT")
			{
				record(entry_id, known, "REDACTED_POLICY", 0, "Unreviewed per-user hive payload omitted by default");
				continue;
			}
			const auto bytes = sqlite3_column_bytes(files.get(), 1);
			std::wstring relative(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
			std::memcpy(relative.data(), sqlite3_column_blob(files.get(), 1), static_cast<std::size_t>(bytes));
			auto opened = image.open_file(relative);
			if (!opened.ok())
			{
				record(entry_id, known, opened.error().code == ERROR_ACCESS_DENIED ? "ACCESS_DENIED" : "QUERY_FAILED", opened.error().code, opened.error().message);
				continue;
			}
			auto guarded = std::move(opened).value();
			BY_HANDLE_FILE_INFORMATION information{};
			if (!GetFileInformationByHandle(guarded.file.get(), &information))
			{
				record(entry_id, known, "QUERY_FAILED", GetLastError(), "Cannot verify probed file identity");
				continue;
			}
			const std::uint64_t index = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) | information.nFileIndexLow;
			if (sqlite3_column_type(files.get(), 3) != SQLITE_NULL &&
				(static_cast<sqlite3_int64>(information.dwVolumeSerialNumber) != sqlite3_column_int64(files.get(), 3) ||
					 sqlite3_column_bytes(files.get(), 4) != static_cast<int>(sizeof(index)) ||
				 std::memcmp(sqlite3_column_blob(files.get(), 4), &index, sizeof(index)) != 0))
			{
				record(entry_id, known, "UNRESOLVED", ERROR_FILE_INVALID, "File identity changed since namespace inventory");
				continue;
			}
			std::array<char, 4> header{};
			DWORD read = 0;
			if (!ReadFile(guarded.file.get(), header.data(), static_cast<DWORD>(header.size()), &read, nullptr))
			{
				record(entry_id, known, "QUERY_FAILED", GetLastError(), "Cannot read registry format signature");
				continue;
			}
			if (read != header.size() || std::memcmp(header.data(), "regf", header.size()) != 0)
			{
				record(entry_id, known, known ? "PARSE_FAILED" : "NOT_REGF", 0, known ? "Stock hive has no valid REGF signature" : "Not a primary REGF source");
				continue;
			}
			if (!known)
			{
				record(entry_id, nullptr, "REDACTED_POLICY", 0, "REGF source discovered; unreviewed arbitrary hive payload omitted");
				continue;
			}
			evidence::execute(handle, "SAVEPOINT registry_hive;");
			auto relative_text = platform::utf8(relative);
			if (!relative_text.ok()) throw std::runtime_error("Stock hive path encoding failed");
			auto status = scan_hive(image.root() / relative, relative_text.value(), known->name,
				known->required, database, writer, progress_state);
			if (status.ok())
			{
				status = writer.flush();
				if (!status.ok()) return status;
				evidence::execute(handle, "RELEASE registry_hive;");
				record(entry_id, known, "PARSED", 0, "REGF parsed with canonical filesystem provenance", true);
			}
			else
			{
				writer.discard_pending();
				evidence::execute(handle, "ROLLBACK TO registry_hive; RELEASE registry_hive;");
				record(entry_id, known, "PARSE_FAILED", status.error().code, status.error().message);
			}
		}
		evidence::check_sql(handle, code);
		for (const auto& candidate : stock)
		{
			if (seen.contains(candidate.path)) continue;
			evidence::query_t present(handle, "SELECT id FROM fs_entries WHERE comparison_key=?1;");
			present.text(1, candidate.path);
			const auto result = sqlite3_step(present.get());
			evidence::check_sql(handle, result);
			if (result == SQLITE_ROW)
			{
				record(sqlite3_column_int64(present.get(), 0), &candidate, "UNSUPPORTED_FORMAT", 0, "Stock hive path is not a regular file; not followed");
				continue;
			}
			evidence::query_t discovery(handle, "SELECT discovery_complete FROM collector_runs WHERE collector='filesystem';");
			evidence::check_sql(handle, sqlite3_step(discovery.get()));
			auto wide = platform::wide(candidate.path);
			if (!wide.ok()) throw std::runtime_error("Stock path conversion failed");
			auto resolved = image.resolve(wide.value());
			const bool complete = sqlite3_column_int(discovery.get(), 0) == 1 && resolved.ok();
			record(std::nullopt, &candidate, !complete ? "UNRESOLVED" : candidate.required ? "MISSING_REQUIRED" : "NOT_PRESENT", 0,
				complete ? "Stock source not found in complete working namespace" : "Namespace discovery incomplete; absence not established");
		}
		auto status = writer.finalize();
		if (!status.ok()) return status;
		evidence::execute(handle, R"sql(
UPDATE collector_runs SET
 discovered=(SELECT count(*) FROM registry_sources),represented=(SELECT count(*) FROM registry_sources),
 parsed=(SELECT count(*) FROM registry_sources WHERE state='PARSED'),
 failed=(SELECT count(*) FROM registry_sources WHERE state IN ('PARSE_FAILED','QUERY_FAILED','ACCESS_DENIED','MISSING_REQUIRED')),
 redacted=(SELECT count(*) FROM registry_sources WHERE state IN ('REDACTED_SENSITIVE','REDACTED_POLICY')),
 unsupported=(SELECT count(*) FROM registry_sources WHERE state='UNSUPPORTED_FORMAT'),
 unresolved=(SELECT count(*) FROM registry_sources WHERE state='UNRESOLVED'),
 representation_complete=1,
 discovery_complete=(SELECT discovery_complete FROM collector_runs WHERE collector='filesystem') AND
   NOT EXISTS(SELECT 1 FROM registry_sources WHERE state IN ('QUERY_FAILED','ACCESS_DENIED','UNRESOLVED')),
 state=CASE WHEN EXISTS(SELECT 1 FROM evidence_gaps WHERE collector='registry') OR EXISTS(SELECT 1 FROM registry_omissions) THEN 'COMPLETE_WITH_GAPS' ELSE 'COMPLETE' END,
 ended_utc=CURRENT_TIMESTAMP,checkpoint='inventory-probes-finished'
WHERE collector='registry';
)sql");
		return status_t::success();
	}
	catch (const std::exception& error) { return status_t::failure(error.what()); }
}

}
