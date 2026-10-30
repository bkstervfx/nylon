#ifndef NYLON_EVIDENCE_EVIDENCE_HPP
#define NYLON_EVIDENCE_EVIDENCE_HPP

#include <array>
#include <string_view>

#include "../database/database.hpp"
#include "../media/media.hpp"

namespace nylon::evidence
{

inline constexpr std::array<std::string_view, 17> required_collectors = {
	"source_media", "filesystem", "registry", "servicing", "components", "features_capabilities",
	"appx", "services", "tasks", "drivers", "pe_signatures", "com_winrt_shell", "startup",
	"boot_recovery", "configuration", "certificates", "relationships"
};

inline constexpr std::array<std::string_view, 11> original_metadata_fields = {
	"security_descriptors", "original_attributes", "original_timestamps", "original_reparse_data",
	"named_streams", "extended_attributes", "sparse_compression", "original_hardlink_identity",
	"container_resource_identity", "image_catalog", "setup_boot_media"
};

[[nodiscard]] status_t initialize(database::database_t& database, const media::prepared_input_t& input);
[[nodiscard]] status_t inventory(database::database_t& database, const std::filesystem::path& root);
[[nodiscard]] status_t verify(database::database_t& database);
[[nodiscard]] status_t finalize(database::database_t& database);
[[nodiscard]] status_t initialize_source_media(database::database_t& database, const media::prepared_input_t& input);
[[nodiscard]] status_t verify_source_media(database::database_t& database);

}

#endif
