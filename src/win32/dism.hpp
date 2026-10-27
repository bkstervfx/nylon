#ifndef NYLON_WIN32_DISM_HPP
#define NYLON_WIN32_DISM_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nylon::win32
{
	struct dism_package_t final
	{
		std::wstring name;
		std::uint32_t state = 0;
		std::uint32_t release_type = 0;
	};

	struct dism_feature_t final
	{
		std::wstring name;
		std::uint32_t state = 0;
	};

	struct dism_capability_t final
	{
		std::wstring name;
		std::uint32_t state = 0;
	};

	struct dism_driver_t final
	{
		std::wstring published_name;
		std::wstring original_file_name;
		std::wstring class_name;
		std::wstring class_guid;
		std::wstring provider_name;
		bool inbox = false;
		bool boot_critical = false;
	};

	struct dism_inventory_t final
	{
		bool available = false;
		std::wstring unavailable_reason;
		std::vector<dism_package_t> packages;
		std::vector<dism_feature_t> features;
		std::vector<dism_capability_t> capabilities;
		std::vector<dism_driver_t> drivers;
	};

	[[nodiscard]] dism_inventory_t dism_inventory(
		const std::filesystem::path& image_root,
		const std::filesystem::path& scratch_root,
		const std::filesystem::path& log_file );
}

#endif
