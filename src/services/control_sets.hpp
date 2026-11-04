#ifndef NYLON_SERVICES_CONTROL_SETS_HPP
#define NYLON_SERVICES_CONTROL_SETS_HPP

#include "../database/database.hpp"

namespace nylon::services
{

[[nodiscard]] status_t collect_control_sets(database::database_t& database);
[[nodiscard]] status_t verify_control_sets(database::database_t& database);

}

#endif
