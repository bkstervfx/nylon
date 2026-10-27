#ifndef NYLON_SERVICES_SERVICES_HPP
#define NYLON_SERVICES_SERVICES_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::services
{
	void scan( const std::filesystem::path& image_root, database::database_t& database );
}

#endif
