#include "registry.hpp"
#include "bulk_writer.hpp"
#include "detail.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../platform/win32.hpp"
#include "../progress/progress.hpp"

namespace nylon::registry
{

namespace
{

constexpr std::size_t max_registry_key_depth = 512U;
constexpr std::uint64_t progress_batch_size = 128U;

struct pending_key_t
{
	database::object_id_t id = 0;
	database::object_id_t hive_id = 0;
	std::optional<database::object_id_t> parent_id;
	std::uint32_t offset = 0;
	std::string path;
	std::string name;
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
			cursor = end + 1U;
		}
	}
	else
	{
		std::size_t length = text.size();
		while (length > 0 && text[length - 1U] == L'\0') --length;
		joined.assign(text.data(), length);
	}

	auto converted = platform::utf8(joined);
	if (!converted.ok())
	{
		return result_t<std::optional<std::string>>::failure(converted.error().message, converted.error().code);
	}
	return result_t<std::optional<std::string>>::success(
		std::optional<std::string>(std::move(converted).value()));
}

bool is_user_hive_candidate(const std::filesystem::path& path)
{
	const auto filename = path.filename().native();
	return equal_ordinal_ignore_case(filename, L"NTUSER.DAT") ||
		equal_ordinal_ignore_case(filename, L"UsrClass.dat");
}

bool path_not_found(const std::error_code& error) noexcept
{
	return error == std::errc::no_such_file_or_directory ||
		error.value() == ERROR_FILE_NOT_FOUND ||
		error.value() == ERROR_PATH_NOT_FOUND;
}

status_t record_hive_error(
	database::database_t& database,
	std::string_view hive_name,
	bool required,
	const error_t& error)
{
	auto error_object = database.insert_object(
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
	database::database_t& database,
	bulk_writer_t& writer,
	progress::registry_state_t* progress_state)
{
	auto hive_result = hive_t::open(path);
	if (!hive_result.ok())
	{
		auto record_status = record_hive_error(database, hive_name, required, hive_result.error());
		if (!record_status.ok()) return record_status;
		if (required) return status_t::failure(hive_result.error().message, hive_result.error().code);
		return status_t::success();
	}

	if (progress_state != nullptr) progress_state->hive(hive_name);

	auto source_path_result = platform::utf8(path.generic_wstring());
	if (!source_path_result.ok())
	{
		return status_t::failure(source_path_result.error().message, source_path_result.error().code);
	}
	auto hive_id_result = writer.add_hive(hive_name, source_path_result.value(), required);
	if (!hive_id_result.ok())
	{
		return status_t::failure(hive_id_result.error().message, hive_id_result.error().code);
	}
	const database::object_id_t hive_id = hive_id_result.value();

	auto hive = std::move(hive_result).value();
	auto root_offset = hive.root_key_offset();
	if (!root_offset.ok()) return status_t::failure(root_offset.error().message, root_offset.error().code);

	std::uint64_t unpublished_keys = 0U;
	std::uint64_t unpublished_values = 0U;
	std::vector<pending_key_t> pending;
	pending.reserve(4096U);
	pending.push_back(pending_key_t{
		writer.allocate_key_id(),
		hive_id,
		std::nullopt,
		root_offset.value(),
		std::string(hive_name),
		std::string(hive_name),
		0U });

	while (!pending.empty())
	{
		auto current = std::move(pending.back());
		pending.pop_back();
		if (current.depth > max_registry_key_depth)
		{
			return status_t::failure("Registry key hierarchy exceeds the supported depth", ERROR_BADDB);
		}

		auto values_result = hive.values(current.offset);
		if (!values_result.ok())
		{
			return status_t::failure(values_result.error().message, values_result.error().code);
		}

		auto subkeys_result = hive.subkeys(current.offset);
		if (!subkeys_result.ok())
		{
			return status_t::failure(subkeys_result.error().message, subkeys_result.error().code);
		}
		if (!subkeys_result.value().empty() && current.depth >= max_registry_key_depth)
		{
			return status_t::failure("Registry key hierarchy exceeds the supported depth", ERROR_BADDB);
		}

		++unpublished_keys;
		unpublished_values += static_cast<std::uint64_t>(values_result.value().size());
		if (progress_state != nullptr && unpublished_keys >= progress_batch_size)
		{
			progress_state->batch(unpublished_keys, unpublished_values, current.path);
			unpublished_keys = 0U;
			unpublished_values = 0U;
		}

		for (auto& value : values_result.value())
		{
			auto value_name_result = platform::utf8(value.name);
			if (!value_name_result.ok())
			{
				return status_t::failure(value_name_result.error().message, value_name_result.error().code);
			}
			auto text_result = value_text(value);
			if (!text_result.ok())
			{
				return status_t::failure(text_result.error().message, text_result.error().code);
			}

			auto status = writer.add_value(
				current.id,
				0U,
				std::move(value_name_result).value(),
				value.type,
				std::move(value.data),
				std::move(text_result).value());
			if (!status.ok()) return status;
		}

		for (auto iterator = subkeys_result.value().rbegin(); iterator != subkeys_result.value().rend(); ++iterator)
		{
			auto child_name_result = platform::utf8(iterator->name);
			if (!child_name_result.ok())
			{
				return status_t::failure(child_name_result.error().message, child_name_result.error().code);
			}
			std::string child_name = std::move(child_name_result).value();
			std::string child_path;
			child_path.reserve(current.path.size() + 1U + child_name.size());
			child_path.append(current.path);
			child_path.push_back('\\');
			child_path.append(child_name);
			pending.push_back(pending_key_t{
				writer.allocate_key_id(),
				hive_id,
				current.id,
				iterator->offset,
				std::move(child_path),
				std::move(child_name),
				current.depth + 1U });
		}

		auto status = writer.add_key(
			current.id,
			current.hive_id,
			current.parent_id,
			current.offset,
			std::move(current.name),
			std::move(current.path));
		if (!status.ok()) return status;
	}

	if (progress_state != nullptr && (unpublished_keys != 0U || unpublished_values != 0U))
	{
		progress_state->batch(unpublished_keys, unpublished_values);
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
	bulk_writer_t& writer,
	const std::filesystem::path& path,
	std::set<std::wstring, ordinal_less_t>& scanned,
	progress::registry_state_t* progress_state)
{
	const std::wstring normalized = path.lexically_normal().native();
	if (!scanned.insert(normalized).second) return status_t::success();
	auto relative = image.relative_utf8(path);
	if (!relative.ok()) return status_t::failure(relative.error().message, relative.error().code);
	std::string label = "HIVE:";
	label.append(relative.value());
	return scan_hive(path, label, false, database, writer, progress_state);
}

}

status_t scan_all(
	const image::image_t& image,
	database::database_t& database,
	progress::registry_state_t* progress_state)
{
	auto writer_result = bulk_writer_t::create(database);
	if (!writer_result.ok())
	{
		return status_t::failure(writer_result.error().message, writer_result.error().code);
	}
	auto writer = std::move(writer_result).value();

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
		if (error)
		{
			if (path_not_found(error))
			{
				if (specification.required)
				{
					return status_t::failure(
						std::string("Required offline registry hive is missing: ") + specification.name,
						ERROR_FILE_NOT_FOUND);
				}
				continue;
			}
			return status_t::failure("Unable to inspect registry hive path", error.value());
		}
		if (!exists)
		{
			if (specification.required)
			{
				return status_t::failure(
					std::string("Required offline registry hive is missing: ") + specification.name,
					ERROR_FILE_NOT_FOUND);
			}
			continue;
		}
		scanned.insert(path.lexically_normal().native());
		auto result = scan_hive(
			path,
			specification.name,
			specification.required,
			database,
			writer,
			progress_state);
		if (!result.ok()) return result;
	}

	auto config_result = image.resolve(L"Windows\\System32\\config");
	if (!config_result.ok()) return status_t::failure(config_result.error().message, config_result.error().code);
	const auto& config_root = config_result.value();
	std::error_code error;
	std::vector<std::filesystem::path> candidates;
	for (std::filesystem::directory_iterator iterator(
		config_root,
		std::filesystem::directory_options::skip_permission_denied,
		error);
		!error && iterator != std::filesystem::directory_iterator();
		iterator.increment(error))
	{
		std::error_code entry_error;
		if (iterator->is_regular_file(entry_error) && !entry_error && has_primary_hive_header(iterator->path()))
		{
			candidates.push_back(iterator->path());
		}
	}
	if (error) return status_t::failure("Unable to enumerate offline registry config directory", error.value());
	std::sort(candidates.begin(), candidates.end());
	for (const auto& candidate : candidates)
	{
		auto result = scan_optional_hive_path(image, database, writer, candidate, scanned, progress_state);
		if (!result.ok()) return result;
	}

	for (const auto* relative_root : { L"Users", L"Windows\\ServiceProfiles" })
	{
		auto root_result = image.resolve(relative_root);
		if (!root_result.ok()) return status_t::failure(root_result.error().message, root_result.error().code);
		const auto& root = root_result.value();
		error.clear();
		const bool is_directory = std::filesystem::is_directory(root, error);
		if (error)
		{
			if (path_not_found(error))
			{
				error.clear();
				continue;
			}
			return status_t::failure("Unable to inspect registry hive search root", error.value());
		}
		if (!is_directory) continue;

		candidates.clear();
		for (std::filesystem::recursive_directory_iterator iterator(
			root,
			std::filesystem::directory_options::skip_permission_denied,
			error);
			!error && iterator != std::filesystem::recursive_directory_iterator();
			iterator.increment(error))
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
			if (iterator->is_regular_file(entry_error) && !entry_error && is_user_hive_candidate(iterator->path()))
			{
				candidates.push_back(iterator->path());
			}
		}
		if (error) return status_t::failure("Unable to enumerate offline user registry hives", error.value());
		std::sort(candidates.begin(), candidates.end());
		for (const auto& candidate : candidates)
		{
			auto result = scan_optional_hive_path(image, database, writer, candidate, scanned, progress_state);
			if (!result.ok()) return result;
		}
	}

	if (progress_state != nullptr)
	{
		progress_state->batch(0U, 0U, "building registry indexes");
	}
	return writer.finalize();
}

}
