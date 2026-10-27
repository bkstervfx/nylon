#ifndef NYLON_COM_COM_HPP
#define NYLON_COM_COM_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::com
{
	void scan( const std::filesystem::path& image_root, database::database_t& database );
}

#endif
