#ifndef NYLON_SERVICING_SERVICING_HPP
#define NYLON_SERVICING_SERVICING_HPP

#include "../image/image.hpp"

namespace nylon::database
{
	class database_t;
}

namespace nylon::servicing
{
	void scan( const image::paths_t& paths, database::database_t& database );
}

#endif
