#ifndef NYLON_DATABASE_DATABASE_HPP
#define NYLON_DATABASE_DATABASE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <sqlite3.h>

#include "../result.hpp"

namespace nylon::database
{

using object_id_t = std::int64_t;

class statement_t
{
public:
	statement_t() noexcept = default;
	explicit statement_t(sqlite3_stmt* statement) noexcept;
	~statement_t();

	statement_t(const statement_t&) = delete;
	statement_t& operator=(const statement_t&) = delete;

	statement_t(statement_t&& other) noexcept;
	statement_t& operator=(statement_t&& other) noexcept;

	[[nodiscard]] sqlite3_stmt* get() const noexcept;
	[[nodiscard]] sqlite3_stmt* release() noexcept;
	void reset(sqlite3_stmt* statement = nullptr) noexcept;

private:
	sqlite3_stmt* statement_ = nullptr;
};

class database_t
{
public:
	database_t() noexcept = default;
	~database_t();

	database_t(const database_t&) = delete;
	database_t& operator=(const database_t&) = delete;

	database_t(database_t&& other) noexcept;
	database_t& operator=(database_t&& other) noexcept;

	static result_t<database_t> open(const std::filesystem::path& path);

	status_t initialize();
	status_t begin();
	status_t commit();
	status_t rollback() noexcept;

	result_t<object_id_t> object(
		std::string_view kind,
		std::string_view key,
		std::string_view name,
		std::string_view path = {},
		std::optional<object_id_t> parent = std::nullopt);

	result_t<object_id_t> insert_object(
		std::string_view kind,
		std::string_view key,
		std::string_view name,
		std::string_view path = {},
		std::optional<object_id_t> parent = std::nullopt);

	status_t property(
		object_id_t object_id,
		std::string_view key,
		std::string_view value);

	status_t edge(
		object_id_t from,
		std::string_view relation,
		object_id_t to,
		std::string_view evidence);

	status_t file_metadata(
		object_id_t object_id,
		std::uint64_t size,
		std::uint32_t volume_serial,
		std::uint64_t file_index,
		std::uint32_t link_count,
		std::uint32_t attributes);

	status_t registry_data(
		object_id_t object_id,
		std::uint32_t type,
		std::span<const std::byte> data);

	status_t meta(std::string_view key, std::string_view value);
	status_t collector_status(
		std::string_view collector,
		std::string_view state,
		std::string_view detail = {});
	status_t finalize_graph();

private:
	explicit database_t(sqlite3* database) noexcept;

	void reset() noexcept;
	status_t prepare_cached_statements();
	status_t execute(std::string_view sql);
	result_t<statement_t> prepare(std::string_view sql);
	status_t bind_text(sqlite3_stmt* statement, int index, std::string_view value);
	status_t step_done_and_reset(sqlite3_stmt* statement);
	status_t reset_statement(sqlite3_stmt* statement) noexcept;

	sqlite3* database_ = nullptr;
	statement_t object_upsert_;
	statement_t object_insert_;
	statement_t property_insert_;
	statement_t edge_insert_;
	statement_t file_metadata_upsert_;
	statement_t registry_data_upsert_;
	statement_t metadata_upsert_;
	statement_t collector_upsert_;
	bool in_transaction_ = false;
};

}

#endif
