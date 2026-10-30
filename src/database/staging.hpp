#ifndef NYLON_DATABASE_STAGING_HPP
#define NYLON_DATABASE_STAGING_HPP

#include "database.hpp"

namespace nylon::database
{

status_t prepare_staging_database(database_t& database);
status_t activate_staging_database(database_t& database);
status_t quick_check(database_t& database);

}

#endif
