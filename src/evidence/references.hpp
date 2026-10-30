#ifndef NYLON_EVIDENCE_REFERENCES_HPP
#define NYLON_EVIDENCE_REFERENCES_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

namespace nylon::evidence
{

inline constexpr std::string_view reference_parser_version = "offline-reference-v1";
inline constexpr std::size_t max_reference_code_units = 1024U * 1024U;

struct reference_context_t
{
	std::wstring windows_directory;
	std::optional<std::wstring> relative_base;
	bool command_line = false;
};

struct reference_t
{
	std::wstring raw;
	std::wstring target;
	std::wstring arguments;
	std::optional<std::string> image_path;
	std::string state = "UNRESOLVED";
	std::string detail;
	std::vector<sqlite3_int64> matched_entries;
	std::optional<sqlite3_int64> entry_id;
};

[[nodiscard]] reference_t parse_reference(std::wstring raw, const reference_context_t& context);
[[nodiscard]] reference_t resolve_reference(sqlite3* database, std::wstring raw, const reference_context_t& context);

}

#endif
