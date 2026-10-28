#ifndef NYLON_PLATFORM_PROCESS_HPP
#define NYLON_PLATFORM_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "../result.hpp"

namespace nylon::platform
{

[[nodiscard]] result_t<std::uint32_t> run_process(
	const std::filesystem::path& application,
	std::span<const std::wstring> arguments,
	const std::filesystem::path& working_directory = {});

}

#endif
