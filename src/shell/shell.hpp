#ifndef NYLON_SHELL_SHELL_HPP
#define NYLON_SHELL_SHELL_HPP

namespace nylon::database
{
	class database_t;
}

namespace nylon::shell
{
	void scan( database::database_t& database );
}

#endif
