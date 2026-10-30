#ifndef NYLON_PLATFORM_HASH_HPP
#define NYLON_PLATFORM_HASH_HPP

#include <filesystem>
#include <span>
#include <string>

#include "../result.hpp"

namespace nylon::platform
{

[[nodiscard]] result_t<std::string> sha256_file(const std::filesystem::path& path);
[[nodiscard]] result_t<std::string> sha256_bytes(std::span<const std::byte> bytes);

}

#endif
