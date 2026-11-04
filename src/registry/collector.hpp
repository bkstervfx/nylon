#ifndef NYLON_REGISTRY_COLLECTOR_HPP
#define NYLON_REGISTRY_COLLECTOR_HPP

#include "registry.hpp"
#include "bulk_writer.hpp"

namespace nylon::registry
{

status_t scan_hive(const std::filesystem::path& path, std::string_view source_relative,
	std::string_view hive_name, bool required, database::database_t& database,
	bulk_writer_t& writer, progress::registry_state_t* progress_state);

}

#endif
