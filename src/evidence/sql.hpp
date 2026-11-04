#ifndef NYLON_EVIDENCE_SQL_HPP
#define NYLON_EVIDENCE_SQL_HPP

#include <stdexcept>
#include <string>
#include <string_view>

#include "../database/database.hpp"

namespace nylon::evidence
{

inline void check_sql(sqlite3* database, int code)
{
	if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
	{
		throw std::runtime_error(std::string("Evidence SQLite failure: ") + sqlite3_errmsg(database));
	}
}

inline void execute(sqlite3* database, const char* sql)
{
	check_sql(database, sqlite3_exec(database, sql, nullptr, nullptr, nullptr));
}

class query_t
{
public:
	query_t(sqlite3* database, const char* sql) : database_(database)
	{
		sqlite3_stmt* raw = nullptr;
		check_sql(database, sqlite3_prepare_v2(database, sql, -1, &raw, nullptr));
		statement_.reset(raw);
	}

	void text(int column, std::string_view value)
	{
		check_sql(database_, sqlite3_bind_text64(get(), column, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
	}
	void number(int column, sqlite3_int64 value)
	{
		check_sql(database_, sqlite3_bind_int64(get(), column, value));
	}
	void blob(int column, const void* data, std::size_t size)
	{
		check_sql(database_, sqlite3_bind_blob64(get(), column, data, size, SQLITE_TRANSIENT));
	}
	void done()
	{
		const int code = sqlite3_step(get());
		check_sql(database_, code);
		if (code != SQLITE_DONE) throw std::runtime_error("Evidence statement did not finish");
		check_sql(database_, sqlite3_reset(get()));
		check_sql(database_, sqlite3_clear_bindings(get()));
	}
	[[nodiscard]] sqlite3_stmt* get() const { return statement_.get(); }

private:
	sqlite3* database_;
	database::statement_t statement_;
};

}

#endif
