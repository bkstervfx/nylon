#include "collector.hpp"
#include "bulk_writer.hpp"
#include "detail.hpp"
#include "../evidence/sql.hpp"

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

bool sensitive_label(std::string label)
{
	for (auto& character : label)
		if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
	for (const auto* pattern : { "PASSWORD", "SECRET", "CREDENTIAL", "PRIVATEKEY", "TOKEN", "\\SAM\\", "\\SECURITY\\", "\\POLICY\\" })
		if (label.find(pattern) != std::string::npos) return true;
	return false;
}

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

}

status_t scan_hive(
	const std::filesystem::path& path,
	std::string_view source_relative,
	std::string_view hive_name,
	bool required,
	database::database_t& database,
	bulk_writer_t& writer,
	progress::registry_state_t* progress_state)
{
	auto hive_result = hive_t::open(path);
	if (!hive_result.ok())
	{
		return status_t::failure(hive_result.error().message, hive_result.error().code);
	}

	if (progress_state != nullptr) progress_state->hive(hive_name);

	auto hive_id_result = writer.add_hive(hive_name, source_relative, required);
	if (!hive_id_result.ok())
	{
		return status_t::failure(hive_id_result.error().message, hive_id_result.error().code);
	}
	const database::object_id_t hive_id = hive_id_result.value();

	auto hive = std::move(hive_result).value();
	evidence::query_t omission(database.native_handle(), "INSERT INTO registry_omissions(key_id,name,value_type,state,reason) VALUES(?1,?2,?3,'REDACTED_POLICY','Sensitive-name policy; no raw value bytes retained or sent to semantic input');");
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

		const bool omitted_key = sensitive_label(current.path + "\\");
		auto values_result = omitted_key ? result_t<std::vector<value_t>>::success({}) : hive.values(current.offset);
		if (omitted_key)
		{
			omission.number(1, current.id);
			omission.done();
		}
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
			if (sensitive_label(value_name_result.value()))
			{
				omission.number(1, current.id);
				omission.text(2, value_name_result.value());
				omission.number(3, value.type);
				omission.done();
				continue;
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

}
