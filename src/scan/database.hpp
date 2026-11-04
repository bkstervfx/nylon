#ifndef NYLON_SCAN_DATABASE_HPP
#define NYLON_SCAN_DATABASE_HPP
//
//
//
#include "common.hpp"
//
#include "database/database.hpp"
#include "image/image.hpp"
#include "media/media.hpp"
//
//
namespace nylon::scan::internal
{
	//
	result_t<database::database_t> open_database
	(
		const std::filesystem::path& path,
		const image::image_t& image,
		const media::prepared_input_t& source
	);
	//
	status_t finish_registry( database::database_t& database );
	//
	status_t publish_database
	(
		database::database_t& database,
		const std::filesystem::path& staging,
		const std::filesystem::path& destination
	);
	//
}
//
//
//
#endif // NYLON_SCAN_DATABASE_HPP
