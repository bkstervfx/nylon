#include "staging.hpp"

#include <string>
#include <string_view>
#include <utility>

#include <sqlite3.h>

namespace nylon::database
{

namespace
{

status_t sqlite_failure(sqlite3* database, std::string context, int code)
{
	const char* message = database != nullptr ? sqlite3_errmsg(database) : nullptr;
	if (message != nullptr)
	{
		context.append(": ");
		context.append(message);
	}
	return status_t::failure(std::move(context), static_cast<std::uint32_t>(code));
}

status_t execute(sqlite3* database, const char* sql)
{
	char* error = nullptr;
	const int rc = sqlite3_exec(database, sql, nullptr, nullptr, &error);
	if (rc == SQLITE_OK)
	{
		return status_t::success();
	}

	std::string message = "Unable to configure SQLite staging database";
	if (error != nullptr)
	{
		message.append(": ");
		message.append(error);
		sqlite3_free(error);
	}
	return status_t::failure(std::move(message), static_cast<std::uint32_t>(rc));
}

}

status_t prepare_staging_database(database_t& database)
{
	sqlite3* handle = database.native_handle();
	if (handle == nullptr) return status_t::failure("SQLite database is not open");

	return execute(handle, R"sql(
PRAGMA page_size=16384;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-262144;
)sql");
}

status_t activate_staging_database(database_t& database)
{
	sqlite3* handle = database.native_handle();
	if (handle == nullptr) return status_t::failure("SQLite database is not open");

	return execute(handle, R"sql(
PRAGMA journal_mode=DELETE;
PRAGMA synchronous=OFF;
PRAGMA locking_mode=EXCLUSIVE;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-262144;
PRAGMA wal_autocheckpoint=0;
)sql");
}

status_t quick_check(database_t& database)
{
	sqlite3* handle = database.native_handle();
	if (handle == nullptr) return status_t::failure("SQLite database is not open");

	sqlite3_stmt* statement = nullptr;
	int rc = sqlite3_prepare_v2(handle, "PRAGMA quick_check;", -1, &statement, nullptr);
	if (rc != SQLITE_OK)
	{
		return sqlite_failure(handle, "Unable to prepare SQLite quick_check", rc);
	}

	rc = sqlite3_step(statement);
	if (rc != SQLITE_ROW)
	{
		const auto failure = sqlite_failure(handle, "SQLite quick_check did not return a result", rc);
		sqlite3_finalize(statement);
		return failure;
	}

	const unsigned char* text = sqlite3_column_text(statement, 0);
	const bool ok = text != nullptr && std::string_view(reinterpret_cast<const char*>(text)) == "ok";
	rc = sqlite3_finalize(statement);
	if (rc != SQLITE_OK)
	{
		return sqlite_failure(handle, "Unable to finalize SQLite quick_check", rc);
	}
	if (!ok)
	{
		return status_t::failure("SQLite quick_check reported database corruption");
	}
	return status_t::success();
}

}
