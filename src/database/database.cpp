#include "database.hpp"

#include <limits>
#include <string>
#include <utility>

#include "../platform/win32.hpp"

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

}

statement_t::statement_t(sqlite3_stmt* statement) noexcept
	: statement_(statement)
{
}

statement_t::~statement_t()
{
	reset();
}

statement_t::statement_t(statement_t&& other) noexcept
	: statement_(other.release())
{
}

statement_t& statement_t::operator=(statement_t&& other) noexcept
{
	if (this != &other)
	{
		reset(other.release());
	}

	return *this;
}

sqlite3_stmt* statement_t::get() const noexcept
{
	return statement_;
}

sqlite3_stmt* statement_t::release() noexcept
{
	sqlite3_stmt* statement = statement_;
	statement_ = nullptr;
	return statement;
}

void statement_t::reset(sqlite3_stmt* statement) noexcept
{
	if (statement_ != nullptr)
	{
		sqlite3_finalize(statement_);
	}

	statement_ = statement;
}

database_t::database_t(sqlite3* database) noexcept
	: database_(database)
{
}

database_t::~database_t()
{
	reset();
}

database_t::database_t(database_t&& other) noexcept
	: database_(std::exchange(other.database_, nullptr)),
	  object_upsert_(std::move(other.object_upsert_)),
	  object_select_(std::move(other.object_select_)),
	  property_insert_(std::move(other.property_insert_)),
	  edge_insert_(std::move(other.edge_insert_)),
	  file_metadata_upsert_(std::move(other.file_metadata_upsert_)),
	  registry_data_upsert_(std::move(other.registry_data_upsert_)),
	  metadata_upsert_(std::move(other.metadata_upsert_)),
	  collector_upsert_(std::move(other.collector_upsert_)),
	  in_transaction_(std::exchange(other.in_transaction_, false))
{
}

database_t& database_t::operator=(database_t&& other) noexcept
{
	if (this != &other)
	{
		reset();
		database_ = std::exchange(other.database_, nullptr);
		object_upsert_ = std::move(other.object_upsert_);
		object_select_ = std::move(other.object_select_);
		property_insert_ = std::move(other.property_insert_);
		edge_insert_ = std::move(other.edge_insert_);
		file_metadata_upsert_ = std::move(other.file_metadata_upsert_);
		registry_data_upsert_ = std::move(other.registry_data_upsert_);
		metadata_upsert_ = std::move(other.metadata_upsert_);
		collector_upsert_ = std::move(other.collector_upsert_);
		in_transaction_ = std::exchange(other.in_transaction_, false);
	}

	return *this;
}

void database_t::reset() noexcept
{
	collector_upsert_.reset();
	metadata_upsert_.reset();
	registry_data_upsert_.reset();
	file_metadata_upsert_.reset();
	edge_insert_.reset();
	property_insert_.reset();
	object_select_.reset();
	object_upsert_.reset();

	in_transaction_ = false;

	if (database_ != nullptr)
	{
		sqlite3_close_v2(database_);
		database_ = nullptr;
	}
}

result_t<database_t> database_t::open(const std::filesystem::path& path)
{
	const auto utf8_path = platform::utf8(path.native());

	if (!utf8_path.ok())
	{
		return result_t<database_t>::failure(
			utf8_path.error().message,
			utf8_path.error().code);
	}

	sqlite3* database = nullptr;
	const int rc = sqlite3_open_v2(
		utf8_path.value().c_str(),
		&database,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
		nullptr);

	if (rc != SQLITE_OK)
	{
		const char* detail = database != nullptr ? sqlite3_errmsg(database) : nullptr;
		std::string message = "sqlite3_open_v2 failed";

		if (detail != nullptr)
		{
			message.append(": ");
			message.append(detail);
		}

		if (database != nullptr)
		{
			sqlite3_close_v2(database);
		}

		return result_t<database_t>::failure(
			std::move(message),
			static_cast<std::uint32_t>(rc));
	}

	int configuration_rc = sqlite3_extended_result_codes(database, 1);
	if (configuration_rc == SQLITE_OK)
	{
		configuration_rc = sqlite3_busy_timeout(database, 5000);
	}
	if (configuration_rc != SQLITE_OK)
	{
		const auto failure = sqlite_failure(database, "Unable to configure SQLite connection", configuration_rc);
		sqlite3_close_v2(database);
		return result_t<database_t>::failure(failure.error().message, failure.error().code);
	}

	return result_t<database_t>::success(database_t(database));
}

status_t database_t::initialize()
{
	const char* schema = R"sql(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;
PRAGMA foreign_keys=OFF;
PRAGMA cache_size=-65536;

CREATE TABLE IF NOT EXISTS objects (
	id INTEGER PRIMARY KEY,
	kind TEXT NOT NULL,
	object_key TEXT NOT NULL,
	name TEXT NOT NULL,
	path TEXT NOT NULL DEFAULT '',
	parent_id INTEGER,
	UNIQUE(kind, object_key)
);

CREATE TABLE IF NOT EXISTS properties (
	object_id INTEGER NOT NULL,
	property_key TEXT NOT NULL,
	value TEXT NOT NULL,
	PRIMARY KEY(object_id, property_key, value)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS edges (
	from_id INTEGER NOT NULL,
	relation TEXT NOT NULL,
	to_id INTEGER NOT NULL,
	evidence TEXT NOT NULL,
	PRIMARY KEY(from_id, relation, to_id, evidence)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS edge_target_index
	ON edges(to_id, relation, from_id);

CREATE TABLE IF NOT EXISTS file_metadata (
	object_id INTEGER PRIMARY KEY,
	size INTEGER NOT NULL,
	volume_serial INTEGER NOT NULL,
	file_index INTEGER NOT NULL,
	link_count INTEGER NOT NULL,
	attributes INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS file_identity_index
	ON file_metadata(volume_serial, file_index);

CREATE TABLE IF NOT EXISTS registry_data (
	object_id INTEGER PRIMARY KEY,
	value_type INTEGER NOT NULL,
	data BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS metadata (
	meta_key TEXT PRIMARY KEY,
	value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS collector_status (
	collector TEXT PRIMARY KEY,
	state TEXT NOT NULL,
	detail TEXT NOT NULL DEFAULT ''
) WITHOUT ROWID;
)sql";

	auto status = execute(schema);

	if (!status.ok())
	{
		return status;
	}

	return prepare_cached_statements();
}

status_t database_t::prepare_cached_statements()
{
	auto assign = [this](statement_t& target, std::string_view sql) -> status_t
	{
		auto result = prepare(sql);

		if (!result.ok())
		{
			return status_t::failure(result.error().message, result.error().code);
		}

		target = std::move(result).value();
		return status_t::success();
	};

	status_t status = assign(
		object_upsert_,
		"INSERT INTO objects(kind, object_key, name, path, parent_id) "
		"VALUES(?1, ?2, ?3, ?4, ?5) "
		"ON CONFLICT(kind, object_key) DO UPDATE SET "
		"name=CASE WHEN excluded.name<>'' THEN excluded.name ELSE objects.name END, "
		"path=CASE WHEN excluded.path<>'' THEN excluded.path ELSE objects.path END, "
		"parent_id=COALESCE(objects.parent_id, excluded.parent_id);");

	if (!status.ok()) return status;

	status = assign(
		object_select_,
		"SELECT id FROM objects WHERE kind=?1 AND object_key=?2;");
	if (!status.ok()) return status;

	status = assign(
		property_insert_,
		"INSERT OR IGNORE INTO properties(object_id, property_key, value) VALUES(?1, ?2, ?3);");
	if (!status.ok()) return status;

	status = assign(
		edge_insert_,
		"INSERT OR IGNORE INTO edges(from_id, relation, to_id, evidence) VALUES(?1, ?2, ?3, ?4);");
	if (!status.ok()) return status;

	status = assign(
		file_metadata_upsert_,
		"INSERT OR REPLACE INTO file_metadata(object_id, size, volume_serial, file_index, link_count, attributes) "
		"VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
	if (!status.ok()) return status;

	status = assign(
		registry_data_upsert_,
		"INSERT OR REPLACE INTO registry_data(object_id, value_type, data) VALUES(?1, ?2, ?3);");
	if (!status.ok()) return status;

	status = assign(
		metadata_upsert_,
		"INSERT OR REPLACE INTO metadata(meta_key, value) VALUES(?1, ?2);");
	if (!status.ok()) return status;

	return assign(
		collector_upsert_,
		"INSERT INTO collector_status(collector, state, detail) VALUES(?1, ?2, ?3) "
		"ON CONFLICT(collector) DO UPDATE SET state=excluded.state, detail=excluded.detail;");
}

status_t database_t::begin()
{
	if (in_transaction_)
	{
		return status_t::failure("SQLite transaction is already active");
	}

	auto status = execute("BEGIN IMMEDIATE;");
	if (status.ok())
	{
		in_transaction_ = true;
	}
	return status;
}

status_t database_t::commit()
{
	if (!in_transaction_)
	{
		return status_t::failure("SQLite commit requested without an active transaction");
	}

	auto status = execute("COMMIT;");
	if (status.ok())
	{
		in_transaction_ = false;
	}
	return status;
}

status_t database_t::rollback() noexcept
{
	if (database_ == nullptr || !in_transaction_)
	{
		return status_t::success();
	}

	char* error = nullptr;
	const int rc = sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, &error);
	if (error != nullptr)
	{
		sqlite3_free(error);
	}

	if (rc == SQLITE_OK)
	{
		in_transaction_ = false;
		return status_t::success();
	}

	return status_t::failure("SQLite rollback failed", static_cast<std::uint32_t>(rc));
}

status_t database_t::reset_statement(sqlite3_stmt* statement) noexcept
{
	if (statement == nullptr)
	{
		return status_t::failure("SQLite cached statement is not initialized");
	}

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

result_t<object_id_t> database_t::object(
	std::string_view kind,
	std::string_view key,
	std::string_view name,
	std::string_view path,
	std::optional<object_id_t> parent)
{
	auto status = reset_statement(object_upsert_.get());

	if (!status.ok())
	{
		return result_t<object_id_t>::failure(status.error().message, status.error().code);
	}

	const std::pair<int, std::string_view> text_values[] = {
		{ 1, kind }, { 2, key }, { 3, name }, { 4, path }
	};

	for (const auto& [index, value] : text_values)
	{
		status = bind_text(object_upsert_.get(), index, value);
		if (!status.ok()) return result_t<object_id_t>::failure(status.error().message, status.error().code);
	}

	int rc = parent.has_value()
		? sqlite3_bind_int64(object_upsert_.get(), 5, *parent)
		: sqlite3_bind_null(object_upsert_.get(), 5);

	if (rc != SQLITE_OK)
	{
		reset_statement(object_upsert_.get());
		const auto failure = sqlite_failure(database_, "sqlite3_bind parent failed", rc);
		return result_t<object_id_t>::failure(failure.error().message, failure.error().code);
	}

	status = step_done_and_reset(object_upsert_.get());
	if (!status.ok()) return result_t<object_id_t>::failure(status.error().message, status.error().code);

	status = reset_statement(object_select_.get());
	if (!status.ok()) return result_t<object_id_t>::failure(status.error().message, status.error().code);

	status = bind_text(object_select_.get(), 1, kind);
	if (!status.ok()) return result_t<object_id_t>::failure(status.error().message, status.error().code);
	status = bind_text(object_select_.get(), 2, key);
	if (!status.ok()) return result_t<object_id_t>::failure(status.error().message, status.error().code);

	rc = sqlite3_step(object_select_.get());

	if (rc != SQLITE_ROW)
	{
		const auto failure = sqlite_failure(database_, "Object lookup failed", rc);
		reset_statement(object_select_.get());
		return result_t<object_id_t>::failure(failure.error().message, failure.error().code);
	}

	const auto id = sqlite3_column_int64(object_select_.get(), 0);
	status = reset_statement(object_select_.get());

	if (!status.ok())
	{
		return result_t<object_id_t>::failure(status.error().message, status.error().code);
	}

	return result_t<object_id_t>::success(id);
}

status_t database_t::property(
	object_id_t object_id,
	std::string_view key,
	std::string_view value)
{
	auto status = reset_statement(property_insert_.get());
	if (!status.ok()) return status;

	int rc = sqlite3_bind_int64(property_insert_.get(), 1, object_id);
	if (rc != SQLITE_OK) return sqlite_failure(database_, "sqlite3_bind object id failed", rc);

	status = bind_text(property_insert_.get(), 2, key);
	if (!status.ok()) return status;
	status = bind_text(property_insert_.get(), 3, value);
	if (!status.ok()) return status;

	return step_done_and_reset(property_insert_.get());
}

status_t database_t::edge(
	object_id_t from,
	std::string_view relation,
	object_id_t to,
	std::string_view evidence)
{
	auto status = reset_statement(edge_insert_.get());
	if (!status.ok()) return status;

	int rc = sqlite3_bind_int64(edge_insert_.get(), 1, from);
	if (rc != SQLITE_OK) return sqlite_failure(database_, "sqlite3_bind edge source failed", rc);

	status = bind_text(edge_insert_.get(), 2, relation);
	if (!status.ok()) return status;

	rc = sqlite3_bind_int64(edge_insert_.get(), 3, to);
	if (rc != SQLITE_OK) return sqlite_failure(database_, "sqlite3_bind edge target failed", rc);

	status = bind_text(edge_insert_.get(), 4, evidence);
	if (!status.ok()) return status;

	return step_done_and_reset(edge_insert_.get());
}

status_t database_t::file_metadata(
	object_id_t object_id,
	std::uint64_t size,
	std::uint32_t volume_serial,
	std::uint64_t file_index,
	std::uint32_t link_count,
	std::uint32_t attributes)
{
	if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
		file_index > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
	{
		return status_t::failure("File metadata exceeds SQLite signed integer range");
	}

	auto status = reset_statement(file_metadata_upsert_.get());
	if (!status.ok()) return status;

	const std::int64_t values[] = {
		object_id,
		static_cast<std::int64_t>(size),
		static_cast<std::int64_t>(volume_serial),
		static_cast<std::int64_t>(file_index),
		static_cast<std::int64_t>(link_count),
		static_cast<std::int64_t>(attributes)
	};

	for (int index = 0; index < 6; ++index)
	{
		const int rc = sqlite3_bind_int64(file_metadata_upsert_.get(), index + 1, values[index]);
		if (rc != SQLITE_OK) return sqlite_failure(database_, "sqlite3_bind file metadata failed", rc);
	}

	return step_done_and_reset(file_metadata_upsert_.get());
}

status_t database_t::registry_data(
	object_id_t object_id,
	std::uint32_t type,
	std::span<const std::byte> data)
{
	if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return status_t::failure("Registry value exceeds SQLite blob size limit");
	}

	auto status = reset_statement(registry_data_upsert_.get());
	if (!status.ok()) return status;

	int rc = sqlite3_bind_int64(registry_data_upsert_.get(), 1, object_id);
	if (rc == SQLITE_OK) rc = sqlite3_bind_int64(registry_data_upsert_.get(), 2, type);
	if (rc == SQLITE_OK)
	{
		if (data.empty())
		{
			rc = sqlite3_bind_zeroblob(registry_data_upsert_.get(), 3, 0);
		}
		else
		{
			rc = sqlite3_bind_blob(
				registry_data_upsert_.get(),
				3,
				data.data(),
				static_cast<int>(data.size()),
				SQLITE_TRANSIENT);
		}
	}

	if (rc != SQLITE_OK)
	{
		return sqlite_failure(database_, "sqlite3_bind registry data failed", rc);
	}

	return step_done_and_reset(registry_data_upsert_.get());
}

status_t database_t::meta(std::string_view key, std::string_view value)
{
	auto status = reset_statement(metadata_upsert_.get());
	if (!status.ok()) return status;
	status = bind_text(metadata_upsert_.get(), 1, key);
	if (!status.ok()) return status;
	status = bind_text(metadata_upsert_.get(), 2, value);
	if (!status.ok()) return status;
	return step_done_and_reset(metadata_upsert_.get());
}

status_t database_t::collector_status(
	std::string_view collector,
	std::string_view state,
	std::string_view detail)
{
	auto status = reset_statement(collector_upsert_.get());
	if (!status.ok()) return status;
	status = bind_text(collector_upsert_.get(), 1, collector);
	if (!status.ok()) return status;
	status = bind_text(collector_upsert_.get(), 2, state);
	if (!status.ok()) return status;
	status = bind_text(collector_upsert_.get(), 3, detail);
	if (!status.ok()) return status;
	return step_done_and_reset(collector_upsert_.get());
}

status_t database_t::finalize_graph()
{
	const char* sql = R"sql(
CREATE VIEW IF NOT EXISTS edge_view AS
SELECT
	e.from_id,
	source.kind AS from_kind,
	source.name AS from_name,
	e.relation,
	e.to_id,
	target.kind AS to_kind,
	target.name AS to_name,
	e.evidence
FROM edges AS e
JOIN objects AS source ON source.id=e.from_id
JOIN objects AS target ON target.id=e.to_id;

CREATE VIEW IF NOT EXISTS object_summary AS
SELECT kind, COUNT(*) AS count
FROM objects
GROUP BY kind;

PRAGMA optimize;
)sql";

	auto status = execute(sql);
	if (!status.ok())
	{
		return status;
	}
	return execute("PRAGMA wal_checkpoint(TRUNCATE);");
}

status_t database_t::execute(std::string_view sql)
{
	char* error = nullptr;
	const std::string sql_text(sql);
	const int rc = sqlite3_exec(database_, sql_text.c_str(), nullptr, nullptr, &error);

	if (rc == SQLITE_OK)
	{
		return status_t::success();
	}

	std::string message = "SQLite execution failed";

	if (error != nullptr)
	{
		message.append(": ");
		message.append(error);
		sqlite3_free(error);
	}

	return status_t::failure(std::move(message), static_cast<std::uint32_t>(rc));
}

result_t<statement_t> database_t::prepare(std::string_view sql)
{
	sqlite3_stmt* statement = nullptr;

	if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return result_t<statement_t>::failure("SQLite statement is too large");
	}

	const int rc = sqlite3_prepare_v2(
		database_,
		sql.data(),
		static_cast<int>(sql.size()),
		&statement,
		nullptr);

	if (rc != SQLITE_OK)
	{
		const auto failure = sqlite_failure(database_, "sqlite3_prepare_v2 failed", rc);
		return result_t<statement_t>::failure(failure.error().message, failure.error().code);
	}

	return result_t<statement_t>::success(statement_t(statement));
}

status_t database_t::bind_text(sqlite3_stmt* statement, int index, std::string_view value)
{
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return status_t::failure("SQLite text value is too large");
	}

	const char* text = value.empty() ? "" : value.data();
	const int rc = sqlite3_bind_text(
		statement,
		index,
		text,
		static_cast<int>(value.size()),
		SQLITE_TRANSIENT);

	if (rc != SQLITE_OK)
	{
		return sqlite_failure(database_, "sqlite3_bind_text failed", rc);
	}

	return status_t::success();
}

status_t database_t::step_done_and_reset(sqlite3_stmt* statement)
{
	const int rc = sqlite3_step(statement);

	if (rc != SQLITE_DONE)
	{
		const auto failure = sqlite_failure(database_, "SQLite statement did not complete", rc);
		reset_statement(statement);
		return failure;
	}

	return reset_statement(statement);
}

}
