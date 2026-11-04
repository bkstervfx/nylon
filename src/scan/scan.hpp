#ifndef NYLON_SCAN_SCAN_HPP
#define NYLON_SCAN_SCAN_HPP
//
//
//
#include "common.hpp"
//
#include "result.hpp"
//
#include <optional>
//
//
namespace nylon::scan
{
	//
	struct options_t
	{
		std::filesystem::path input;
		std::optional<uint32_t> image_index;
	};
	//
	status_t run( const options_t& options );
	//
}
//
//
//
#endif // NYLON_SCAN_SCAN_HPP
