#ifndef NYLON_REGISTRY_REGISTRY_HPP
#define NYLON_REGISTRY_REGISTRY_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../database/database.hpp"
#include "../image/image.hpp"
#include "../result.hpp"

namespace nylon::progress
{
class registry_state_t;
}

namespace nylon::registry
{

struct value_t
{
	std::wstring name;
	std::uint32_t type = 0;
	std::vector<std::byte> data;
};

class hive_t
{
public:
	hive_t() noexcept = default;
	~hive_t();

	hive_t(const hive_t&) = delete;
	hive_t& operator=(const hive_t&) = delete;

	hive_t(hive_t&&) noexcept;
	hive_t& operator=(hive_t&&) noexcept;

	static result_t<hive_t> open(const std::filesystem::path& path);

	result_t<std::vector<std::wstring>> subkeys(std::wstring_view path) const;
	result_t<std::vector<value_t>> values(std::wstring_view path) const;
	result_t<std::optional<std::wstring>> string(
		std::wstring_view path,
		std::wstring_view name) const;
	result_t<std::optional<std::uint32_t>> dword(
		std::wstring_view path,
		std::wstring_view name) const;
	result_t<std::vector<std::wstring>> multi_string(
		std::wstring_view path,
		std::wstring_view name) const;

private:
	struct impl_t;
	explicit hive_t(std::unique_ptr<impl_t> impl) noexcept;

	result_t<std::optional<value_t>> value(
		std::wstring_view path,
		std::wstring_view name) const;

	std::unique_ptr<impl_t> impl_;
};

[[nodiscard]] std::string key_object_key(
	std::string_view hive,
	std::string_view relative_path);

[[nodiscard]] std::string value_object_key(
	std::string_view hive,
	std::string_view relative_path,
	std::string_view value_name);

status_t scan_all(
	const image::image_t& image,
	database::database_t& database,
	progress::registry_state_t* progress = nullptr);

}

#endif
