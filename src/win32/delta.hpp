#ifndef NYLON_WIN32_DELTA_HPP
#define NYLON_WIN32_DELTA_HPP

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace nylon::win32
{
	[[nodiscard]] std::optional<std::vector<std::byte>> decode_dcm_manifest(
		const std::filesystem::path& offline_wcp,
		std::span<const std::byte> manifest );
}

#endif
