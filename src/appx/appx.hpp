#ifndef NYLON_APPX_APPX_HPP
#define NYLON_APPX_APPX_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::appx
{
	void scan( const std::filesystem::path& image_root, database::database_t& database );
}

#endif
