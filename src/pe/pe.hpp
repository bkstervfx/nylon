#ifndef NYLON_PE_PE_HPP
#define NYLON_PE_PE_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::pe
{
	void scan( const std::filesystem::path& image_root, database::database_t& database );
}

#endif
