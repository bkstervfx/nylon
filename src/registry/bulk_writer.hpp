#ifndef NYLON_REGISTRY_BULK_WRITER_HPP
#define NYLON_REGISTRY_BULK_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../database/database.hpp"
#include "../result.hpp"

namespace nylon::registry
{

class bulk_writer_t
{
public:
	bulk_writer_t() noexcept = default;
	~bulk_writer_t();

	bulk_writer_t(const bulk_writer_t&) = delete;
	bulk_writer_t& operator=(const bulk_writer_t&) = delete;

	bulk_writer_t(bulk_writer_t&&) noexcept;
	bulk_writer_t& operator=(bulk_writer_t&&) noexcept;

	static result_t<bulk_writer_t> create(database::database_t& database);

	result_t<database::object_id_t> add_hive(
		std::string_view name,
		std::string_view source_path,
		bool required);

	[[nodiscard]] database::object_id_t allocate_key_id() noexcept;

	status_t add_key(
		database::object_id_t id,
		database::object_id_t hive_id,
		std::optional<database::object_id_t> parent_id,
		std::uint32_t cell_offset,
		std::string name,
		std::string path);

	status_t add_value(
		database::object_id_t key_id,
		std::string name,
		std::uint32_t value_type,
		std::vector<std::byte> data,
		std::optional<std::string> text);

	status_t add_value(
		database::object_id_t key_id,
		std::uint32_t,
		std::string name,
		std::uint32_t value_type,
		std::vector<std::byte> data,
		std::optional<std::string> text)
	{
		return add_value(
			key_id,
			std::move(name),
			value_type,
			std::move(data),
			std::move(text));
	}

	status_t flush();
	status_t finalize();

private:
	struct impl_t;
	explicit bulk_writer_t(std::unique_ptr<impl_t> impl) noexcept;
	std::unique_ptr<impl_t> impl_;
};

}

#endif
