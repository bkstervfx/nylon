#ifndef NYLON_IMAGE_IMAGE_HPP
#define NYLON_IMAGE_IMAGE_HPP

#include <filesystem>

namespace nylon::database
{
	class database_t;
}

namespace nylon::image
{
	struct paths_t final
	{
		std::filesystem::path project_root;
		std::filesystem::path image_root;
		std::filesystem::path temp_root;
		std::filesystem::path cache_root;
		std::filesystem::path output_root;
	};

	[[nodiscard]] paths_t resolve_paths(
		const std::filesystem::path& executable,
		const std::filesystem::path& requested_image );

	void audit( const paths_t& paths, database::database_t& database );
}

#endif
