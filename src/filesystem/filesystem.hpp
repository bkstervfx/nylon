#ifndef NYLON_FILESYSTEM_FILESYSTEM_HPP
#define NYLON_FILESYSTEM_FILESYSTEM_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::filesystem
{
	void scan( const std::filesystem::path& image_root, database::database_t& database );
}

#endif
