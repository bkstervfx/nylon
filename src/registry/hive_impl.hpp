#ifndef NYLON_REGISTRY_HIVE_IMPL_HPP
#define NYLON_REGISTRY_HIVE_IMPL_HPP

#include <map>
#include <span>
#include <vector>

#include "detail.hpp"
#include "registry.hpp"
#include "../platform/win32.hpp"

namespace nylon::registry
{

using detail::decode_registry_name;
using detail::equal_ordinal_ignore_case;
using detail::hive_header_size;
using detail::inline_data_flag;
using detail::invalid_cell_offset;
using detail::max_subkey_list_depth;
using detail::nk_compressed_name;
using detail::ordinal_less_t;
using detail::read_number;
using detail::vk_compressed_name;

struct hive_t::impl_t
{
	struct hbin_range_t
	{
		std::uint32_t cell_begin = 0;
		std::uint32_t end = 0;
	};

	platform::mapped_file_t file;
	std::uint32_t root_offset = 0;
	std::uint32_t data_size = 0;
	std::vector<hbin_range_t> hbins;
	mutable std::map<std::wstring, std::uint32_t, ordinal_less_t> key_cache;

	[[nodiscard]] std::span<const std::byte> bytes() const noexcept
	{
		return file.bytes();
	}

	result_t<std::span<const std::byte>> cell(std::uint32_t offset) const
	{
		const auto all = bytes();
		const std::size_t relative = static_cast<std::size_t>(offset);

		if (offset == invalid_cell_offset || (offset & 0x7U) != 0)
		{
			return result_t<std::span<const std::byte>>::failure(
				"Registry cell offset is invalid",
				ERROR_BADDB);
		}

		const hbin_range_t* owner = nullptr;
		for (const auto& range : hbins)
		{
			if (offset >= range.cell_begin && offset < range.end)
			{
				owner = &range;
				break;
			}
		}

		if (owner == nullptr ||
			relative > owner->end ||
			sizeof(std::int32_t) > static_cast<std::size_t>(owner->end) - relative)
		{
			return result_t<std::span<const std::byte>>::failure(
				"Registry cell offset is outside an hbin cell region",
				ERROR_BADDB);
		}

		const std::size_t absolute = hive_header_size + relative;

		auto size_result = read_number<std::int32_t>(all, absolute, "Registry cell size");

		if (!size_result.ok())
		{
			return result_t<std::span<const std::byte>>::failure(
				size_result.error().message,
				size_result.error().code);
		}

		const std::int32_t signed_size = size_result.value();

		if (signed_size >= 0 || signed_size == std::numeric_limits<std::int32_t>::min())
		{
			return result_t<std::span<const std::byte>>::failure(
				"Registry reference points to a free or invalid cell",
				ERROR_BADDB);
		}

		const std::size_t cell_size = static_cast<std::size_t>(-signed_size);

		if (cell_size < 8U ||
			(cell_size & 0x7U) != 0 ||
			cell_size > all.size() - absolute ||
			cell_size > static_cast<std::size_t>(owner->end) - relative)
		{
			return result_t<std::span<const std::byte>>::failure(
				"Registry cell size exceeds hive bounds",
				ERROR_BADDB);
		}

		return result_t<std::span<const std::byte>>::success(
			all.subspan(absolute + sizeof(std::int32_t), cell_size - sizeof(std::int32_t)));
	}

	result_t<std::wstring> key_name(std::uint32_t key_offset) const
	{
		auto cell_result = cell(key_offset);

		if (!cell_result.ok())
		{
			return result_t<std::wstring>::failure(
				cell_result.error().message,
				cell_result.error().code);
		}

		const auto key = cell_result.value();

		if (key.size() < 0x4CU ||
			std::to_integer<unsigned char>(key[0]) != 'n' ||
			std::to_integer<unsigned char>(key[1]) != 'k')
		{
			return result_t<std::wstring>::failure("Registry key cell is malformed", ERROR_BADDB);
		}

		auto flags = read_number<std::uint16_t>(key, 0x02U, "Registry key flags");
		auto length = read_number<std::uint16_t>(key, 0x48U, "Registry key name length");

		if (!flags.ok() || !length.ok())
		{
			return result_t<std::wstring>::failure("Registry key header is truncated", ERROR_BADDB);
		}

		const std::size_t name_length = length.value();

		if (name_length > key.size() - 0x4CU)
		{
			return result_t<std::wstring>::failure("Registry key name exceeds cell bounds", ERROR_BADDB);
		}

		return decode_registry_name(
			key.subspan(0x4CU, name_length),
			(flags.value() & nk_compressed_name) != 0);
	}

	result_t<std::vector<std::uint32_t>> subkey_offsets_from_list(
		std::uint32_t list_offset,
		std::size_t depth = 0) const
	{
		if (depth >= max_subkey_list_depth)
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				"Registry subkey index recursion is too deep",
				ERROR_BADDB);
		}

		auto list_result = cell(list_offset);

		if (!list_result.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				list_result.error().message,
				list_result.error().code);
		}

		const auto list = list_result.value();

		if (list.size() < 4U)
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				"Registry subkey list is truncated",
				ERROR_BADDB);
		}

		const char first = static_cast<char>(std::to_integer<unsigned char>(list[0]));
		const char second = static_cast<char>(std::to_integer<unsigned char>(list[1]));
		auto count_result = read_number<std::uint16_t>(list, 2U, "Registry subkey list count");

		if (!count_result.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				count_result.error().message,
				count_result.error().code);
		}

		const std::size_t count = count_result.value();
		std::vector<std::uint32_t> result;

		if ((first == 'l' && second == 'f') || (first == 'l' && second == 'h'))
		{
			if (count > (list.size() - 4U) / 8U)
			{
				return result_t<std::vector<std::uint32_t>>::failure(
					"Registry hashed subkey list exceeds cell bounds",
					ERROR_BADDB);
			}

			result.reserve(count);

			for (std::size_t index = 0; index < count; ++index)
			{
				auto offset = read_number<std::uint32_t>(
					list,
					4U + index * 8U,
					"Registry subkey offset");

				if (!offset.ok())
				{
					return result_t<std::vector<std::uint32_t>>::failure(
						offset.error().message,
						offset.error().code);
				}

				result.push_back(offset.value());
			}
		}
		else if ((first == 'l' && second == 'i') || (first == 'r' && second == 'i'))
		{
			if (count > (list.size() - 4U) / 4U)
			{
				return result_t<std::vector<std::uint32_t>>::failure(
					"Registry subkey list exceeds cell bounds",
					ERROR_BADDB);
			}

			for (std::size_t index = 0; index < count; ++index)
			{
				auto offset = read_number<std::uint32_t>(
					list,
					4U + index * 4U,
					"Registry subkey offset");

				if (!offset.ok())
				{
					return result_t<std::vector<std::uint32_t>>::failure(
						offset.error().message,
						offset.error().code);
				}

				if (first == 'r')
				{
					auto nested = subkey_offsets_from_list(offset.value(), depth + 1U);

					if (!nested.ok())
					{
						return result_t<std::vector<std::uint32_t>>::failure(
							nested.error().message,
							nested.error().code);
					}

					result.insert(result.end(), nested.value().begin(), nested.value().end());
				}
				else
				{
					result.push_back(offset.value());
				}
			}
		}
		else
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				"Registry subkey list has an unknown signature",
				ERROR_BADDB);
		}

		return result_t<std::vector<std::uint32_t>>::success(std::move(result));
	}

	result_t<std::vector<std::uint32_t>> subkey_offsets(std::uint32_t key_offset) const
	{
		auto key_result = cell(key_offset);

		if (!key_result.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				key_result.error().message,
				key_result.error().code);
		}

		const auto key = key_result.value();

		if (key.size() < 0x4CU)
		{
			return result_t<std::vector<std::uint32_t>>::failure("Registry key cell is truncated", ERROR_BADDB);
		}

		auto count = read_number<std::uint32_t>(key, 0x14U, "Registry subkey count");
		auto list = read_number<std::uint32_t>(key, 0x1CU, "Registry subkey list offset");

		if (!count.ok() || !list.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure("Registry key subkey metadata is truncated", ERROR_BADDB);
		}

		if (count.value() == 0)
		{
			return result_t<std::vector<std::uint32_t>>::success({});
		}

		auto offsets = subkey_offsets_from_list(list.value());

		if (!offsets.ok())
		{
			return offsets;
		}

		if (offsets.value().size() != static_cast<std::size_t>(count.value()))
		{
			return result_t<std::vector<std::uint32_t>>::failure(
				"Registry subkey index count does not match key metadata",
				ERROR_BADDB);
		}

		return offsets;
	}

	result_t<std::uint32_t> key_offset(std::wstring_view path) const
	{
		const std::wstring full_path(path);
		const auto cached = key_cache.find(full_path);

		if (cached != key_cache.end())
		{
			return result_t<std::uint32_t>::success(cached->second);
		}

		std::uint32_t current = root_offset;
		std::wstring resolved_path;
		std::size_t cursor = 0;

		while (cursor < path.size())
		{
			while (cursor < path.size() && path[cursor] == L'\\')
			{
				++cursor;
			}

			if (cursor >= path.size())
			{
				break;
			}

			const std::size_t separator = path.find(L'\\', cursor);
			const std::size_t end = separator == std::wstring_view::npos
				? path.size()
				: separator;
			const std::wstring_view segment = path.substr(cursor, end - cursor);

			auto children = subkey_offsets(current);

			if (!children.ok())
			{
				return result_t<std::uint32_t>::failure(
					children.error().message,
					children.error().code);
			}

			bool found = false;

			for (const std::uint32_t child : children.value())
			{
				auto name = key_name(child);

				if (!name.ok())
				{
					return result_t<std::uint32_t>::failure(
						name.error().message,
						name.error().code);
				}

				std::wstring candidate = resolved_path;
				if (!candidate.empty()) candidate.push_back(L'\\');
				candidate.append(name.value());
				key_cache.emplace(candidate, child);

				if (equal_ordinal_ignore_case(name.value(), segment))
				{
					current = child;
					resolved_path = std::move(candidate);
					found = true;
					break;
				}
			}

			if (!found)
			{
				return result_t<std::uint32_t>::failure(
					"Registry key was not found",
					ERROR_FILE_NOT_FOUND);
			}

			cursor = separator == std::wstring_view::npos ? path.size() : separator + 1U;
		}

		key_cache.emplace(full_path, current);
		return result_t<std::uint32_t>::success(current);
	}

	result_t<std::vector<std::uint32_t>> value_offsets(std::uint32_t key_offset) const
	{
		auto key_result = cell(key_offset);

		if (!key_result.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure(key_result.error().message, key_result.error().code);
		}

		const auto key = key_result.value();
		auto count = read_number<std::uint32_t>(key, 0x24U, "Registry value count");
		auto list_offset = read_number<std::uint32_t>(key, 0x28U, "Registry value list offset");

		if (!count.ok() || !list_offset.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure("Registry value metadata is truncated", ERROR_BADDB);
		}

		if (count.value() == 0)
		{
			return result_t<std::vector<std::uint32_t>>::success({});
		}

		auto list_result = cell(list_offset.value());

		if (!list_result.ok())
		{
			return result_t<std::vector<std::uint32_t>>::failure(list_result.error().message, list_result.error().code);
		}

		const auto list = list_result.value();
		const std::size_t value_count = count.value();

		if (value_count > list.size() / sizeof(std::uint32_t))
		{
			return result_t<std::vector<std::uint32_t>>::failure("Registry value list exceeds cell bounds", ERROR_BADDB);
		}

		std::vector<std::uint32_t> result;
		result.reserve(value_count);

		for (std::size_t index = 0; index < value_count; ++index)
		{
			auto offset = read_number<std::uint32_t>(list, index * sizeof(std::uint32_t), "Registry value offset");

			if (!offset.ok())
			{
				return result_t<std::vector<std::uint32_t>>::failure(offset.error().message, offset.error().code);
			}

			result.push_back(offset.value());
		}

		return result_t<std::vector<std::uint32_t>>::success(std::move(result));
	}

	result_t<std::vector<std::byte>> data_bytes(
		std::uint32_t data_offset,
		std::uint32_t data_size) const
	{
		if (data_size == 0)
		{
			return result_t<std::vector<std::byte>>::success({});
		}

		auto data_cell = cell(data_offset);

		if (!data_cell.ok())
		{
			return result_t<std::vector<std::byte>>::failure(data_cell.error().message, data_cell.error().code);
		}

		const auto payload = data_cell.value();

		if (data_size <= payload.size())
		{
			return result_t<std::vector<std::byte>>::success(
				std::vector<std::byte>(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(data_size)));
		}

		if (payload.size() < 8U ||
			std::to_integer<unsigned char>(payload[0]) != 'd' ||
			std::to_integer<unsigned char>(payload[1]) != 'b')
		{
			return result_t<std::vector<std::byte>>::failure("Registry value data exceeds its cell", ERROR_BADDB);
		}

		auto segment_count = read_number<std::uint16_t>(payload, 2U, "Registry big-data segment count");
		auto segment_list = read_number<std::uint32_t>(payload, 4U, "Registry big-data list offset");

		if (!segment_count.ok() || !segment_list.ok())
		{
			return result_t<std::vector<std::byte>>::failure("Registry big-data header is truncated", ERROR_BADDB);
		}

		auto list_cell = cell(segment_list.value());

		if (!list_cell.ok())
		{
			return result_t<std::vector<std::byte>>::failure(list_cell.error().message, list_cell.error().code);
		}

		const auto list = list_cell.value();
		const std::size_t count = segment_count.value();

		if (count > list.size() / sizeof(std::uint32_t))
		{
			return result_t<std::vector<std::byte>>::failure("Registry big-data list exceeds cell bounds", ERROR_BADDB);
		}

		std::vector<std::byte> result;
		result.reserve(data_size);

		for (std::size_t index = 0; index < count && result.size() < data_size; ++index)
		{
			auto segment_offset = read_number<std::uint32_t>(list, index * sizeof(std::uint32_t), "Registry big-data segment offset");

			if (!segment_offset.ok())
			{
				return result_t<std::vector<std::byte>>::failure(segment_offset.error().message, segment_offset.error().code);
			}

			auto segment = cell(segment_offset.value());

			if (!segment.ok())
			{
				return result_t<std::vector<std::byte>>::failure(segment.error().message, segment.error().code);
			}

			const std::size_t remaining = static_cast<std::size_t>(data_size) - result.size();
			const std::size_t take = std::min(remaining, segment.value().size());
			result.insert(
				result.end(),
				segment.value().begin(),
				segment.value().begin() + static_cast<std::ptrdiff_t>(take));
		}

		if (result.size() != data_size)
		{
			return result_t<std::vector<std::byte>>::failure("Registry big-data segments are incomplete", ERROR_BADDB);
		}

		return result_t<std::vector<std::byte>>::success(std::move(result));
	}

	result_t<value_t> parse_value(std::uint32_t value_offset) const
	{
		auto value_cell = cell(value_offset);

		if (!value_cell.ok())
		{
			return result_t<value_t>::failure(value_cell.error().message, value_cell.error().code);
		}

		const auto value = value_cell.value();

		if (value.size() < 0x14U ||
			std::to_integer<unsigned char>(value[0]) != 'v' ||
			std::to_integer<unsigned char>(value[1]) != 'k')
		{
			return result_t<value_t>::failure("Registry value cell is malformed", ERROR_BADDB);
		}

		auto name_length = read_number<std::uint16_t>(value, 0x02U, "Registry value name length");
		auto data_length = read_number<std::uint32_t>(value, 0x04U, "Registry value data length");
		auto data_offset = read_number<std::uint32_t>(value, 0x08U, "Registry value data offset");
		auto type = read_number<std::uint32_t>(value, 0x0CU, "Registry value type");
		auto flags = read_number<std::uint16_t>(value, 0x10U, "Registry value flags");

		if (!name_length.ok() || !data_length.ok() || !data_offset.ok() || !type.ok() || !flags.ok())
		{
			return result_t<value_t>::failure("Registry value header is truncated", ERROR_BADDB);
		}

		const std::size_t name_size = name_length.value();

		if (name_size > value.size() - 0x14U)
		{
			return result_t<value_t>::failure("Registry value name exceeds cell bounds", ERROR_BADDB);
		}

		auto name = decode_registry_name(
			value.subspan(0x14U, name_size),
			(flags.value() & vk_compressed_name) != 0);

		if (!name.ok())
		{
			return result_t<value_t>::failure(name.error().message, name.error().code);
		}

		const bool inline_data = (data_length.value() & inline_data_flag) != 0;
		const std::uint32_t actual_size = data_length.value() & ~inline_data_flag;
		std::vector<std::byte> data;

		if (inline_data)
		{
			if (actual_size > sizeof(std::uint32_t))
			{
				return result_t<value_t>::failure("Registry inline value is larger than four bytes", ERROR_BADDB);
			}

			data.assign(
				value.begin() + 0x08,
				value.begin() + 0x08 + static_cast<std::ptrdiff_t>(actual_size));
		}
		else
		{
			auto data_result = data_bytes(data_offset.value(), actual_size);

			if (!data_result.ok())
			{
				return result_t<value_t>::failure(data_result.error().message, data_result.error().code);
			}

			data = std::move(data_result).value();
		}

		value_t result;
		result.name = std::move(name).value();
		result.type = type.value();
		result.data = std::move(data);
		return result_t<value_t>::success(std::move(result));
	}
};

}

#endif
