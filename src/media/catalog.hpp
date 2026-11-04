#ifndef NYLON_MEDIA_CATALOG_HPP
#define NYLON_MEDIA_CATALOG_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../result.hpp"

namespace nylon::media
{

inline constexpr std::size_t max_catalog_bytes = 64U * 1024U * 1024U;

struct catalog_field_t
{
	std::string path;
	std::string value;
};

struct catalog_image_t
{
	std::uint32_t index = 0;
	std::wstring name;
	std::vector<catalog_field_t> fields;
};

struct catalog_t
{
	std::vector<std::byte> xml;
	std::vector<catalog_image_t> images;
};

[[nodiscard]] result_t<catalog_t> parse_catalog_bytes(std::vector<std::byte> bytes);
[[nodiscard]] result_t<catalog_t> read_catalog(const std::filesystem::path& path);
[[nodiscard]] result_t<std::vector<std::byte>> read_catalog_xml(const std::filesystem::path& path);

}

#endif
