#ifndef NYLON_TASKS_TASKS_HPP
#define NYLON_TASKS_TASKS_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::tasks
{
	void scan( const std::filesystem::path& image_root, database::database_t& database );
}

#endif
