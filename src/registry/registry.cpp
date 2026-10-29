#include "registry.hpp"
#include "detail.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "../platform/win32.hpp"

namespace nylon::registry
{

namespace
{

constexpr std::size_t max_registry_key_depth = 512U;

struct pending_key_t
{
	std::wstring path;
	database::object_id_t parent = 0;
	std::size_t depth = 0;
};

using detail::decode_utf16;
using detail::equal_ordinal_ignore_case;
using detail::ordinal_less_t;
using detail::read_number;

result_t<std::optional<std::string>> value_text(const value_t& value)
{
	if (value.type == REG_DWORD && value.data.size() == sizeof(std::uint32_t))
	{
		std::uint32_t number = 0;
		std::memcpy(&number, value.data.data(), sizeof(number));
		return result_t<std::optional<std::string>>::success(std::to_string(number));
	}

	if (value.type == REG_QWORD && value.data.size() == sizeof(std::uint64_t))
	{
		std::uint64_t number = 0;
		std::memcpy(&number, value.data.data(), sizeof(number));
		return result_t<std::optional<std::string>>::success(std::to_string(number));
	}

	if (value.type != REG_SZ && value.type != REG_EXPAND_SZ && value.type != REG_MULTI_SZ)
	{
		return result_t<std::optional<std::string>>::success(std::nullopt);
	}

	auto decoded = decode_utf16(std::span<const std::byte>(value.data.data(), value.data.size()));
	if (!decoded.ok())
	{
		return result_t<std::optional<std::string>>::failure(decoded.error().message, decoded.error().code);
	}

	const std::wstring& text = decoded.value();
	std::wstring joined;

	if (value.type == REG_MULTI_SZ)
	{
		std::size_t cursor = 0;
		while (cursor < text.size())
		{
			std::size_t end = cursor;
			while (end < text.size() && text[end] != L'\0') ++end;
			if (end == cursor) break;
			if (!joined.empty()) joined.push_back(L'\n');
			joined.append(text.data() + cursor, end - cursor);
			cursor = end + 1;
		}
	}
	else
	{
		std::size_t length = text.size();
		while (length > 0 && text[length - 1] == L'\0') --length;
		joined.assign(text.data(), length);
	}

	auto converted = platform::utf8(joined);
	if (!converted.ok())
	{
		return result_t<std::optional<std::string>>::failure(converted.error().message, converted.error().code);
	}
	return result_t<std::optional<std::string>>::success(std::optional<std::string>(std::move(converted).value()));
}

std::string join_key(std::string_view left, std::string_view right)
{
	if (right.empty()) return std::string(left);
	std::string value(left);
	value.push_back('\\');
	value.append(right);
	return value;
}

void append_identity_component(std::string& output, std::string_view value)
{
	output.append(std::to_string(value.size()));
	output.push_back(':');
	output.append(value);
}

bool is_user_hive_candidate(const std::filesystem::path& path)
{
	const auto filename = path.filename().native();
	return equal_ordinal_ignore_case(filename, L"NTUSER.DAT") ||
		equal_ordinal_ignore_case(filename, L"UsrClass.dat");
}

status_t record_hive_error(
	database::database_t& database,
	std::string_view hive_name,
	bool required,
	const error_t& error)
{
	auto error_object = database.object(
		"audit_error",
		std::string("registry:") + std::string(hive_name),
		std::string(hive_name));
	if (!error_object.ok())
	{
		return status_t::failure(error_object.error().message, error_object.error().code);
	}

	for (const auto& [key, value] : {
		std::pair<std::string_view, std::string>{ "collector", "registry" },
		{ "message", error.message },
		{ "code", std::to_string(error.code) },
		{ "required", required ? "true" : "false" } })
	{
		auto status = database.property(error_object.value(), key, value);
		if (!status.ok()) return status;
	}
	return status_t::success();
}

status_t scan_hive(
	const std::filesystem::path& path,
	std::string_view hive_name,
	bool required,
	database::database_t& database)
{
	auto hive_result = hive_t::open(path);
	if (!hive_result.ok())
	{
		auto record_status = record_hive_error(database, hive_name, required, hive_result.error());
		if (!record_status.ok()) return record_status;
		if (required) return status_t::failure(hive_result.error().message, hive_result.error().code);
		return status_t::success();
	}

	auto hive = std::move(hive_result).value();
	auto hive_object = database.object("registry_hive", std::string(hive_name), std::string(hive_name));
	if (!hive_object.ok()) return status_t::failure(hive_object.error().message, hive_object.error().code);

	std::deque<pending_key_t> pending;
	pending.push_back(pending_key_t{ L"", hive_object.value(), 0U });
	while (!pending.empty())
	{
		auto current = std::move(pending.front());
		pending.pop_front();
		if (current.depth > max_registry_key_depth)
		{
			return status_t::failure("Registry key hierarchy exceeds the supported depth", ERROR_BADDB);
		}

		auto current_utf8 = platform::utf8(current.path);
		if (!current_utf8.ok()) return status_t::failure(current_utf8.error().message, current_utf8.error().code);

		const std::string object_key = key_object_key(hive_name, current_utf8.value());
		const std::string display_path = join_key(hive_name, current_utf8.value());
		const std::string display_name = current.path.empty() ? std::string(hive_name) : current_utf8.value();
		auto key_object = database.object("registry_key", object_key, display_name, display_path, current.parent);
		if (!key_object.ok()) return status_t::failure(key_object.error().message, key_object.error().code);

		auto values_result = hive.values(current.path);
		if (!values_result.ok()) return status_t::failure(values_result.error().message, values_result.error().code);
		for (const auto& value : values_result.value())
		{
			auto value_name = platform::utf8(value.name);
			if (!value_name.ok()) return status_t::failure(value_name.error().message, value_name.error().code);
			const std::string value_key = value_object_key(hive_name, current_utf8.value(), value_name.value());
			std::string value_path = display_path;
			value_path.append("::");
			value_path.append(value_name.value());
			auto value_object = database.object(
				"registry_value",
				value_key,
				value_name.value().empty() ? "(Default)" : value_name.value(),
				value_path,
				key_object.value());
			if (!value_object.ok()) return status_t::failure(value_object.error().message, value_object.error().code);

			auto raw = database.registry_data(value_object.value(), value.type, std::span<const std::byte>(value.data.data(), value.data.size()));
			if (!raw.ok()) return raw;
			auto type = database.property(value_object.value(), "type", std::to_string(value.type));
			if (!type.ok()) return type;
			auto text_result = value_text(value);
			if (!text_result.ok()) return status_t::failure(text_result.error().message, text_result.error().code);
			if (text_result.value().has_value())
			{
				auto property = database.property(value_object.value(), "text", *text_result.value());
				if (!property.ok()) return property;
			}
		}

		auto subkeys_result = hive.subkeys(current.path);
		if (!subkeys_result.ok()) return status_t::failure(subkeys_result.error().message, subkeys_result.error().code);
		if (!subkeys_result.value().empty() && current.depth >= max_registry_key_depth)
		{
			return status_t::failure("Registry key hierarchy exceeds the supported depth", ERROR_BADDB);
		}

		for (const auto& subkey : subkeys_result.value())
		{
			std::wstring child = current.path;
			if (!child.empty()) child.push_back(L'\\');
			child.append(subkey);
			pending.push_back(pending_key_t{ std::move(child), key_object.value(), current.depth + 1U });
		}
	}
	return status_t::success();
}

bool has_primary_hive_header(const std::filesystem::path& path)
{
	auto mapped = platform::mapped_file_t::open_readonly(path);
	if (!mapped.ok()) return false;
	const auto bytes = mapped.value().bytes();
	if (bytes.size() < 0x24U ||
		std::to_integer<unsigned char>(bytes[0]) != 'r' ||
		std::to_integer<unsigned char>(bytes[1]) != 'e' ||
		std::to_integer<unsigned char>(bytes[2]) != 'g' ||
		std::to_integer<unsigned char>(bytes[3]) != 'f')
	{
		return false;
	}

	auto major = read_number<std::uint32_t>(bytes, 0x14U, "Registry major version");
	auto minor = read_number<std::uint32_t>(bytes, 0x18U, "Registry minor version");
	auto type = read_number<std::uint32_t>(bytes, 0x1CU, "Registry file type");
	auto format = read_number<std::uint32_t>(bytes, 0x20U, "Registry file format");
	return major.ok() && minor.ok() && type.ok() && format.ok() &&
		major.value() == 1U && minor.value() >= 3U && minor.value() <= 6U &&
		type.value() == 0U && format.value() == 1U;
}

status_t scan_optional_hive_path(
	const image::image_t& image,
	database::database_t& database,
	const std::filesystem::path& path,
	std::set<std::wstring, ordinal_less_t>& scanned)
{
	const std::wstring normalized = path.lexically_normal().native();
	if (!scanned.insert(normalized).second || !has_primary_hive_header(path)) return status_t::success();
	auto relative = image.relative_utf8(path);
	if (!relative.ok()) return status_t::failure(relative.error().message, relative.error().code);
	std::string label = "HIVE:";
	label.append(relative.value());
	return scan_hive(path, label, false, database);
}

}

std::string key_object_key(std::string_view hive, std::string_view relative_path)
{
	std::string result("K");
	append_identity_component(result, hive);
	append_identity_component(result, relative_path);
	return result;
}

std::string value_object_key(std::string_view hive, std::string_view relative_path, std::string_view value_name)
{
	std::string result("V");
	append_identity_component(result, hive);
	append_identity_component(result, relative_path);
	append_identity_component(result, value_name);
	return result;
}

status_t scan_all(const image::image_t& image, database::database_t& database)
{
	struct hive_spec_t { const wchar_t* relative_path; const char* name; bool required; };
	const hive_spec_t known_hives[] = {
		{ L"Windows\\System32\\config\\SOFTWARE", "SOFTWARE", true },
		{ L"Windows\\System32\\config\\SYSTEM", "SYSTEM", true },
		{ L"Windows\\System32\\config\\COMPONENTS", "COMPONENTS", true },
		{ L"Windows\\System32\\config\\DEFAULT", "DEFAULT", true },
		{ L"Windows\\System32\\config\\SAM", "SAM", true },
		{ L"Windows\\System32\\config\\SECURITY", "SECURITY", true },
		{ L"Boot\\BCD", "BCD", false },
		{ L"EFI\\Microsoft\\Boot\\BCD", "BCD_EFI", false },
		{ L"Windows\\AppCompat\\Programs\\Amcache.hve", "AMCACHE", false }
	};

	std::set<std::wstring, ordinal_less_t> scanned;
	for (const auto& specification : known_hives)
	{
		auto path_result = image.resolve(specification.relative_path);
		if (!path_result.ok()) return status_t::failure(path_result.error().message, path_result.error().code);
		const auto& path = path_result.value();
		std::error_code error;
		const bool exists = std::filesystem::is_regular_file(path, error);
		if (error) return status_t::failure("Unable to inspect registry hive path", error.value());
		if (!exists)
		{
			if (specification.required) return status_t::failure(std::string("Required offline registry hive is missing: ") + specification.name, ERROR_FILE_NOT_FOUND);
			continue;
		}
		scanned.insert(path.lexically_normal().native());
		auto result = scan_hive(path, specification.name, specification.required, database);
		if (!result.ok()) return result;
	}

	auto config_result = image.resolve(L"Windows\\System32\\config");
	if (!config_result.ok()) return status_t::failure(config_result.error().message, config_result.error().code);
	const auto& config_root = config_result.value();
	std::error_code error;
	std::vector<std::filesystem::path> candidates;
	for (std::filesystem::directory_iterator iterator(config_root, std::filesystem::directory_options::skip_permission_denied, error); !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error))
	{
		std::error_code entry_error;
		if (iterator->is_regular_file(entry_error) && !entry_error && has_primary_hive_header(iterator->path())) candidates.push_back(iterator->path());
	}
	if (error) return status_t::failure("Unable to enumerate offline registry config directory", error.value());
	std::sort(candidates.begin(), candidates.end());
	for (const auto& candidate : candidates)
	{
		auto result = scan_optional_hive_path(image, database, candidate, scanned);
		if (!result.ok()) return result;
	}

	for (const auto* relative_root : { L"Users", L"Windows\\ServiceProfiles" })
	{
		auto root_result = image.resolve(relative_root);
		if (!root_result.ok()) return status_t::failure(root_result.error().message, root_result.error().code);
		const auto& root = root_result.value();
		error.clear();
		if (!std::filesystem::is_directory(root, error))
		{
			if (error) return status_t::failure("Unable to inspect registry hive search root", error.value());
			continue;
		}
		candidates.clear();
		for (std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error); !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
		{
			auto attributes = platform::file_attributes(iterator->path());
			if (!attributes.ok()) return status_t::failure(attributes.error().message, attributes.error().code);
			if ((attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				std::error_code type_error;
				if (iterator->is_directory(type_error) && !type_error) iterator.disable_recursion_pending();
				continue;
			}
			std::error_code entry_error;
			if (iterator->is_regular_file(entry_error) && !entry_error &&
				is_user_hive_candidate(iterator->path()) &&
				has_primary_hive_header(iterator->path()))
			{
				candidates.push_back(iterator->path());
			}
		}
		if (error) return status_t::failure("Unable to enumerate offline user registry hives", error.value());
		std::sort(candidates.begin(), candidates.end());
		for (const auto& candidate : candidates)
		{
			auto result = scan_optional_hive_path(image, database, candidate, scanned);
			if (!result.ok()) return result;
		}
	}
	return status_t::success();
}

}
