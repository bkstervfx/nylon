#include "bulk_writer.hpp"

#include <limits>
#include <string>
#include <utility>

#include <sqlite3.h>

namespace nylon::registry
{

namespace
{

constexpr std::size_t batch_rows = 256U;
constexpr std::size_t max_pending_blob_bytes = 16U * 1024U * 1024U;

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

status_t execute(sqlite3* database, std::string_view sql)
{
	char* error = nullptr;
	const int rc = sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr, &error);
	if (rc == SQLITE_OK)
	{
		return status_t::success();
	}

	std::string message = "SQLite registry bulk operation failed";
	if (error != nullptr)
	{
		message.append(": ");
		message.append(error);
		sqlite3_free(error);
	}
	return status_t::failure(std::move(message), static_cast<std::uint32_t>(rc));
}

result_t<database::statement_t> prepare(sqlite3* database, std::string_view sql)
{
	if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return result_t<database::statement_t>::failure("SQLite registry bulk statement is too large");
	}

	sqlite3_stmt* statement = nullptr;
	const int rc = sqlite3_prepare_v3(
		database,
		sql.data(),
		static_cast<int>(sql.size()),
		SQLITE_PREPARE_PERSISTENT,
		&statement,
		nullptr);
	if (rc != SQLITE_OK)
	{
		const auto failure = sqlite_failure(database, "Unable to prepare registry bulk statement", rc);
		return result_t<database::statement_t>::failure(failure.error().message, failure.error().code);
	}
	return result_t<database::statement_t>::success(database::statement_t(statement));
}

status_t reset_statement(sqlite3_stmt* statement)
{
	const int reset_rc = sqlite3_reset(statement);
	const int clear_rc = sqlite3_clear_bindings(statement);
	if (reset_rc != SQLITE_OK)
	{
		return status_t::failure("sqlite3_reset failed", static_cast<std::uint32_t>(reset_rc));
	}
	if (clear_rc != SQLITE_OK)
	{
		return status_t::failure("sqlite3_clear_bindings failed", static_cast<std::uint32_t>(clear_rc));
	}
	return status_t::success();
}

status_t bind_text(sqlite3* database, sqlite3_stmt* statement, int index, std::string_view value)
{
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return status_t::failure("Registry text value exceeds SQLite size limit");
	}
	const char* text = value.empty() ? "" : value.data();
	const int rc = sqlite3_bind_text(
		statement,
		index,
		text,
		static_cast<int>(value.size()),
		SQLITE_STATIC);
	if (rc != SQLITE_OK)
	{
		return sqlite_failure(database, "Unable to bind registry text", rc);
	}
	return status_t::success();
}

std::string insert_sql(std::string_view table, std::string_view columns, std::size_t column_count, std::size_t rows)
{
	std::string sql;
	sql.reserve(96U + rows * (column_count * 2U + 4U));
	sql.append("INSERT INTO ");
	sql.append(table);
	sql.push_back('(');
	sql.append(columns);
	sql.append(") VALUES ");
	for (std::size_t row = 0; row < rows; ++row)
	{
		if (row != 0) sql.push_back(',');
		sql.push_back('(');
		for (std::size_t column = 0; column < column_count; ++column)
		{
			if (column != 0) sql.push_back(',');
			sql.push_back('?');
		}
		sql.push_back(')');
	}
	sql.push_back(';');
	return sql;
}

}

struct bulk_writer_t::impl_t
{
	struct key_row_t
	{
		database::object_id_t id = 0;
		database::object_id_t hive_id = 0;
		std::optional<database::object_id_t> parent_id;
		std::uint32_t cell_offset = 0;
		std::string name;
		std::string path;
	};

	struct value_row_t
	{
		database::object_id_t id = 0;
		database::object_id_t key_id = 0;
		std::uint32_t cell_offset = 0;
		std::string name;
		std::uint32_t value_type = 0;
		std::vector<std::byte> data;
		std::optional<std::string> text;
	};

	sqlite3* database = nullptr;
	database::statement_t hive_insert;
	database::statement_t key_batch_insert;
	database::statement_t value_batch_insert;
	std::vector<key_row_t> keys;
	std::vector<value_row_t> values;
	std::size_t pending_blob_bytes = 0;
	database::object_id_t next_hive_id = 1;
	database::object_id_t next_key_id = 1;
	database::object_id_t next_value_id = 1;
	bool finalized = false;

	status_t flush_keys()
	{
		if (keys.empty()) return status_t::success();

		database::statement_t temporary;
		sqlite3_stmt* statement = nullptr;
		const bool full_batch = keys.size() == batch_rows;
		if (full_batch)
		{
			statement = key_batch_insert.get();
			auto status = reset_statement(statement);
			if (!status.ok()) return status;
		}
		else
		{
			auto prepared = prepare(
				database,
				insert_sql(
					"registry_keys",
					"id,hive_id,parent_id,cell_offset,name,path",
					6U,
					keys.size()));
			if (!prepared.ok())
			{
				return status_t::failure(prepared.error().message, prepared.error().code);
			}
			temporary = std::move(prepared).value();
			statement = temporary.get();
		}

		int parameter = 1;
		for (const auto& row : keys)
		{
			int rc = sqlite3_bind_int64(statement, parameter++, row.id);
			if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, parameter++, row.hive_id);
			if (rc == SQLITE_OK)
			{
				rc = row.parent_id.has_value()
					? sqlite3_bind_int64(statement, parameter++, *row.parent_id)
					: sqlite3_bind_null(statement, parameter++);
			}
			if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, parameter++, row.cell_offset);
			if (rc != SQLITE_OK) return sqlite_failure(database, "Unable to bind registry key row", rc);
			auto status = bind_text(database, statement, parameter++, row.name);
			if (!status.ok()) return status;
			status = bind_text(database, statement, parameter++, row.path);
			if (!status.ok()) return status;
		}

		const int rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			return sqlite_failure(database, "Registry key batch insert did not complete", rc);
		}
		if (full_batch)
		{
			auto status = reset_statement(statement);
			if (!status.ok()) return status;
		}
		keys.clear();
		return status_t::success();
	}

	status_t flush_values()
	{
		if (values.empty()) return status_t::success();

		database::statement_t temporary;
		sqlite3_stmt* statement = nullptr;
		const bool full_batch = values.size() == batch_rows;
		if (full_batch)
		{
			statement = value_batch_insert.get();
			auto status = reset_statement(statement);
			if (!status.ok()) return status;
		}
		else
		{
			auto prepared = prepare(
				database,
				insert_sql(
					"registry_values",
					"id,key_id,cell_offset,name,value_type,data,text_value",
					7U,
					values.size()));
			if (!prepared.ok())
			{
				return status_t::failure(prepared.error().message, prepared.error().code);
			}
			temporary = std::move(prepared).value();
			statement = temporary.get();
		}

		int parameter = 1;
		for (const auto& row : values)
		{
			if (row.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
			{
				return status_t::failure("Registry value exceeds SQLite blob size limit");
			}

			int rc = sqlite3_bind_int64(statement, parameter++, row.id);
			if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, parameter++, row.key_id);
			if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, parameter++, row.cell_offset);
			if (rc != SQLITE_OK) return sqlite_failure(database, "Unable to bind registry value row", rc);

			auto status = bind_text(database, statement, parameter++, row.name);
			if (!status.ok()) return status;
			rc = sqlite3_bind_int64(statement, parameter++, row.value_type);
			if (rc == SQLITE_OK)
			{
				rc = row.data.empty()
					? sqlite3_bind_zeroblob(statement, parameter++, 0)
					: sqlite3_bind_blob(
						statement,
						parameter++,
						row.data.data(),
						static_cast<int>(row.data.size()),
						SQLITE_STATIC);
			}
			if (rc != SQLITE_OK) return sqlite_failure(database, "Unable to bind registry value data", rc);
			if (row.text.has_value())
			{
				status = bind_text(database, statement, parameter++, *row.text);
				if (!status.ok()) return status;
			}
			else
			{
				rc = sqlite3_bind_null(statement, parameter++);
				if (rc != SQLITE_OK) return sqlite_failure(database, "Unable to bind registry text value", rc);
			}
		}

		const int rc = sqlite3_step(statement);
		if (rc != SQLITE_DONE)
		{
			return sqlite_failure(database, "Registry value batch insert did not complete", rc);
		}
		if (full_batch)
		{
			auto status = reset_statement(statement);
			if (!status.ok()) return status;
		}
		values.clear();
		pending_blob_bytes = 0;
		return status_t::success();
	}
};

bulk_writer_t::bulk_writer_t(std::unique_ptr<impl_t> impl) noexcept
	: impl_(std::move(impl))
{
}

bulk_writer_t::~bulk_writer_t() = default;
bulk_writer_t::bulk_writer_t(bulk_writer_t&&) noexcept = default;
bulk_writer_t& bulk_writer_t::operator=(bulk_writer_t&&) noexcept = default;

result_t<bulk_writer_t> bulk_writer_t::create(database::database_t& database)
{
	sqlite3* handle = database.native_handle();
	if (handle == nullptr)
	{
		return result_t<bulk_writer_t>::failure("SQLite database is not open");
	}

	const char* schema = R"sql(
CREATE TABLE IF NOT EXISTS registry_hives (
	id INTEGER PRIMARY KEY,
	name TEXT NOT NULL,
	source_path TEXT NOT NULL,
	required INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS registry_keys (
	id INTEGER PRIMARY KEY,
	hive_id INTEGER NOT NULL,
	parent_id INTEGER,
	cell_offset INTEGER NOT NULL,
	name TEXT NOT NULL,
	path TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS registry_values (
	id INTEGER PRIMARY KEY,
	key_id INTEGER NOT NULL,
	cell_offset INTEGER NOT NULL,
	name TEXT NOT NULL,
	value_type INTEGER NOT NULL,
	data BLOB NOT NULL,
	text_value TEXT
);
)sql";

	auto status = execute(handle, schema);
	if (!status.ok())
	{
		return result_t<bulk_writer_t>::failure(status.error().message, status.error().code);
	}

	auto impl = std::make_unique<impl_t>();
	impl->database = handle;
	impl->keys.reserve(batch_rows);
	impl->values.reserve(batch_rows);

	auto hive_statement = prepare(
		handle,
		"INSERT INTO registry_hives(id,name,source_path,required) VALUES(?1,?2,?3,?4);");
	if (!hive_statement.ok())
	{
		return result_t<bulk_writer_t>::failure(hive_statement.error().message, hive_statement.error().code);
	}
	impl->hive_insert = std::move(hive_statement).value();

	auto key_statement = prepare(
		handle,
		insert_sql(
			"registry_keys",
			"id,hive_id,parent_id,cell_offset,name,path",
			6U,
			batch_rows));
	if (!key_statement.ok())
	{
		return result_t<bulk_writer_t>::failure(key_statement.error().message, key_statement.error().code);
	}
	impl->key_batch_insert = std::move(key_statement).value();

	auto value_statement = prepare(
		handle,
		insert_sql(
			"registry_values",
			"id,key_id,cell_offset,name,value_type,data,text_value",
			7U,
			batch_rows));
	if (!value_statement.ok())
	{
		return result_t<bulk_writer_t>::failure(value_statement.error().message, value_statement.error().code);
	}
	impl->value_batch_insert = std::move(value_statement).value();

	return result_t<bulk_writer_t>::success(bulk_writer_t(std::move(impl)));
}

result_t<database::object_id_t> bulk_writer_t::add_hive(
	std::string_view name,
	std::string_view source_path,
	bool required)
{
	if (!impl_ || impl_->finalized)
	{
		return result_t<database::object_id_t>::failure("Registry bulk writer is not available");
	}

	sqlite3_stmt* statement = impl_->hive_insert.get();
	auto status = reset_statement(statement);
	if (!status.ok()) return result_t<database::object_id_t>::failure(status.error().message, status.error().code);

	const database::object_id_t id = impl_->next_hive_id++;
	int rc = sqlite3_bind_int64(statement, 1, id);
	if (rc != SQLITE_OK) return result_t<database::object_id_t>::failure("Unable to bind registry hive id", static_cast<std::uint32_t>(rc));
	status = bind_text(impl_->database, statement, 2, name);
	if (!status.ok()) return result_t<database::object_id_t>::failure(status.error().message, status.error().code);
	status = bind_text(impl_->database, statement, 3, source_path);
	if (!status.ok()) return result_t<database::object_id_t>::failure(status.error().message, status.error().code);
	rc = sqlite3_bind_int(statement, 4, required ? 1 : 0);
	if (rc != SQLITE_OK) return result_t<database::object_id_t>::failure("Unable to bind registry hive requirement", static_cast<std::uint32_t>(rc));
	rc = sqlite3_step(statement);
	if (rc != SQLITE_DONE)
	{
		const auto failure = sqlite_failure(impl_->database, "Registry hive insert did not complete", rc);
		return result_t<database::object_id_t>::failure(failure.error().message, failure.error().code);
	}
	status = reset_statement(statement);
	if (!status.ok()) return result_t<database::object_id_t>::failure(status.error().message, status.error().code);
	return result_t<database::object_id_t>::success(id);
}

database::object_id_t bulk_writer_t::allocate_key_id() noexcept
{
	return impl_ != nullptr ? impl_->next_key_id++ : 0;
}

status_t bulk_writer_t::add_key(
	database::object_id_t id,
	database::object_id_t hive_id,
	std::optional<database::object_id_t> parent_id,
	std::uint32_t cell_offset,
	std::string name,
	std::string path)
{
	if (!impl_ || impl_->finalized) return status_t::failure("Registry bulk writer is not available");
	impl_->keys.push_back(impl_t::key_row_t{
		id,
		hive_id,
		parent_id,
		cell_offset,
		std::move(name),
		std::move(path) });
	if (impl_->keys.size() >= batch_rows) return impl_->flush_keys();
	return status_t::success();
}

status_t bulk_writer_t::add_value(
	database::object_id_t key_id,
	std::uint32_t cell_offset,
	std::string name,
	std::uint32_t value_type,
	std::vector<std::byte> data,
	std::optional<std::string> text)
{
	if (!impl_ || impl_->finalized) return status_t::failure("Registry bulk writer is not available");
	if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return status_t::failure("Registry value exceeds SQLite blob size limit");
	}

	if (impl_->pending_blob_bytes > std::numeric_limits<std::size_t>::max() - data.size())
	{
		return status_t::failure("Registry bulk buffer size overflow");
	}
	impl_->pending_blob_bytes += data.size();
	impl_->values.push_back(impl_t::value_row_t{
		impl_->next_value_id++,
		key_id,
		cell_offset,
		std::move(name),
		value_type,
		std::move(data),
		std::move(text) });

	if (impl_->values.size() >= batch_rows || impl_->pending_blob_bytes >= max_pending_blob_bytes)
	{
		return impl_->flush_values();
	}
	return status_t::success();
}

status_t bulk_writer_t::flush()
{
	if (!impl_ || impl_->finalized) return status_t::failure("Registry bulk writer is not available");
	auto status = impl_->flush_keys();
	if (!status.ok()) return status;
	return impl_->flush_values();
}

status_t bulk_writer_t::finalize()
{
	if (!impl_) return status_t::failure("Registry bulk writer is not available");
	if (impl_->finalized) return status_t::success();

	auto status = impl_->flush_keys();
	if (!status.ok()) return status;
	status = impl_->flush_values();
	if (!status.ok()) return status;

	const char* sql = R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS registry_key_cell_index
	ON registry_keys(hive_id, cell_offset);
CREATE INDEX IF NOT EXISTS registry_key_parent_index
	ON registry_keys(parent_id);
CREATE INDEX IF NOT EXISTS registry_value_key_index
	ON registry_values(key_id);

CREATE VIEW IF NOT EXISTS registry_value_view AS
SELECT
	v.id,
	v.key_id,
	v.cell_offset,
	CASE WHEN v.name='' THEN '(Default)' ELSE v.name END AS name,
	k.path || '::' || v.name AS path,
	v.value_type,
	v.data,
	v.text_value
FROM registry_values AS v
JOIN registry_keys AS k ON k.id=v.key_id;

CREATE VIEW IF NOT EXISTS object_summary AS
SELECT kind, COUNT(*) AS count
FROM (
	SELECT kind FROM objects
	UNION ALL SELECT 'registry_hive' FROM registry_hives
	UNION ALL SELECT 'registry_key' FROM registry_keys
	UNION ALL SELECT 'registry_value' FROM registry_values
)
GROUP BY kind;
)sql";
	status = execute(impl_->database, sql);
	if (!status.ok()) return status;
	impl_->finalized = true;
	return status_t::success();
}

}
