#ifndef NYLON_REGISTRY_REGISTRY_HPP
#define NYLON_REGISTRY_REGISTRY_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::registry
{
	void scan(
		const std::filesystem::path& image_root,
		const std::filesystem::path& temp_root,
		database::database_t& database );
}

#endif
