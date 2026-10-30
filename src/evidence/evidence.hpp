#ifndef NYLON_EVIDENCE_EVIDENCE_HPP
#define NYLON_EVIDENCE_EVIDENCE_HPP

#include "../database/database.hpp"
#include "../media/media.hpp"

namespace nylon::evidence
{

[[nodiscard]] status_t initialize(database::database_t& database, const media::prepared_input_t& input);
[[nodiscard]] status_t inventory(database::database_t& database, const std::filesystem::path& root);
[[nodiscard]] status_t verify(database::database_t& database);
[[nodiscard]] status_t finalize(database::database_t& database);

}

#endif
