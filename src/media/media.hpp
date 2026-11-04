#ifndef NYLON_MEDIA_MEDIA_HPP
#define NYLON_MEDIA_MEDIA_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "../result.hpp"
#include "catalog.hpp"

namespace nylon::media
{

struct prepare_options_t
{
	std::filesystem::path nylon_root;
	std::filesystem::path cache_root;
	std::filesystem::path temp_root;
	std::optional<std::uint32_t> image_index;
};

struct prepared_input_t
{
	std::filesystem::path image_root;
	std::string source_kind;
	std::string iso_sha256;
	std::string image_name;
	std::uint32_t image_index = 0;
	bool cache_hit = false;
	std::optional<catalog_t> catalog;
	std::string catalog_state = "NOT_EXPOSED";
	std::string catalog_detail = "Expanded input has no authoritative install-container catalog.";
	std::string catalog_sha256;
	std::string install_container_path;
};

[[nodiscard]] result_t<prepared_input_t> prepare(
	std::filesystem::path input,
	const prepare_options_t& options);

[[nodiscard]] status_t verify_prepared_cache(const prepared_input_t& prepared);

}

#endif
