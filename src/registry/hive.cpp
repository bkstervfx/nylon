#include "registry.hpp"
#include "detail.hpp"
#include "hive_impl.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../platform/win32.hpp"

namespace nylon::registry
{

using detail::decode_utf16;
using detail::equal_ordinal_ignore_case;
using detail::hive_header_size;
using detail::ordinal_less_t;
using detail::read_number;

namespace
{

result_t<std::uint32_t> base_block_checksum(std::span<const std::byte> bytes)
{
	if (bytes.size() < 0x200U)
	{
		return result_t<std::uint32_t>::failure("Registry hive base block is truncated", ERROR_BADDB);
	}

	std::uint32_t checksum = 0;
	for (std::size_t offset = 0; offset < 0x1FCU; offset += sizeof(std::uint32_t))
	{
		auto value = read_number<std::uint32_t>(bytes, offset, "Registry base-block checksum data");
		if (!value.ok())
		{
			return result_t<std::uint32_t>::failure(value.error().message, value.error().code);
		}
		checksum ^= value.value();
	}

	if (checksum == 0xFFFFFFFFU)
	{
		checksum = 0xFFFFFFFEU;
	}
	else if (checksum == 0U)
	{
		checksum = 1U;
	}

	return result_t<std::uint32_t>::success(checksum);
}

}

hive_t::hive_t(std::unique_ptr<impl_t> impl) noexcept
	: impl_(std::move(impl))
{
}

hive_t::~hive_t() = default;
hive_t::hive_t(hive_t&&) noexcept = default;
hive_t& hive_t::operator=(hive_t&&) noexcept = default;

result_t<hive_t> hive_t::open(const std::filesystem::path& path)
{
	auto mapped = platform::mapped_file_t::open_readonly(path);

	if (!mapped.ok())
	{
		return result_t<hive_t>::failure(mapped.error().message, mapped.error().code);
	}

	const auto bytes = mapped.value().bytes();

	if (bytes.size() < hive_header_size ||
		std::to_integer<unsigned char>(bytes[0]) != 'r' ||
		std::to_integer<unsigned char>(bytes[1]) != 'e' ||
		std::to_integer<unsigned char>(bytes[2]) != 'g' ||
		std::to_integer<unsigned char>(bytes[3]) != 'f')
	{
		return result_t<hive_t>::failure("Registry hive header is invalid", ERROR_NOT_REGISTRY_FILE);
	}

	auto sequence_primary = read_number<std::uint32_t>(bytes, 0x04U, "Registry primary sequence");
	auto sequence_secondary = read_number<std::uint32_t>(bytes, 0x08U, "Registry secondary sequence");
	auto major_version = read_number<std::uint32_t>(bytes, 0x14U, "Registry major version");
	auto minor_version = read_number<std::uint32_t>(bytes, 0x18U, "Registry minor version");
	auto file_type = read_number<std::uint32_t>(bytes, 0x1CU, "Registry file type");
	auto file_format = read_number<std::uint32_t>(bytes, 0x20U, "Registry file format");
	auto root = read_number<std::uint32_t>(bytes, 0x24U, "Registry root cell offset");
	auto data_size = read_number<std::uint32_t>(bytes, 0x28U, "Registry hive data size");
	auto stored_checksum = read_number<std::uint32_t>(bytes, 0x1FCU, "Registry base-block checksum");

	if (!sequence_primary.ok() || !sequence_secondary.ok() ||
		!major_version.ok() || !minor_version.ok() || !file_type.ok() || !file_format.ok() ||
		!root.ok() || !data_size.ok() || !stored_checksum.ok())
	{
		return result_t<hive_t>::failure("Registry hive header is truncated", ERROR_BADDB);
	}

	if (major_version.value() != 1U || minor_version.value() < 3U || minor_version.value() > 6U ||
		file_type.value() != 0U || file_format.value() != 1U)
	{
		return result_t<hive_t>::failure("Registry file is not a supported primary hive", ERROR_NOT_REGISTRY_FILE);
	}

	auto computed_checksum = base_block_checksum(bytes);
	if (!computed_checksum.ok())
	{
		return result_t<hive_t>::failure(computed_checksum.error().message, computed_checksum.error().code);
	}
	if (computed_checksum.value() != stored_checksum.value())
	{
		return result_t<hive_t>::failure(
			"Registry hive base-block checksum is invalid; transaction-log recovery is required",
			ERROR_BADDB);
	}

	if (sequence_primary.value() != sequence_secondary.value())
	{
		return result_t<hive_t>::failure(
			"Registry hive has unreplayed transaction-log state",
			ERROR_BADDB);
	}

	if (data_size.value() == 0 ||
		data_size.value() > bytes.size() - hive_header_size ||
		(data_size.value() % detail::hbin_alignment) != 0 ||
		(root.value() & 0x7U) != 0 ||
		root.value() >= data_size.value())
	{
		return result_t<hive_t>::failure("Registry hive data region is invalid", ERROR_BADDB);
	}

	std::vector<impl_t::hbin_range_t> hbins;
	std::size_t cell_count = 0;
	std::size_t relative = 0;
	while (relative < data_size.value())
	{
		const std::size_t absolute = detail::hive_header_size + relative;
		if (absolute > bytes.size() || detail::hbin_header_size > bytes.size() - absolute ||
			std::to_integer<unsigned char>(bytes[absolute]) != 'h' ||
			std::to_integer<unsigned char>(bytes[absolute + 1]) != 'b' ||
			std::to_integer<unsigned char>(bytes[absolute + 2]) != 'i' ||
			std::to_integer<unsigned char>(bytes[absolute + 3]) != 'n')
		{
			return result_t<hive_t>::failure("Registry hbin header is invalid", ERROR_BADDB);
		}

		auto recorded_offset = read_number<std::uint32_t>(bytes, absolute + 0x04U, "Registry hbin offset");
		auto bin_size = read_number<std::uint32_t>(bytes, absolute + 0x08U, "Registry hbin size");
		if (!recorded_offset.ok() || !bin_size.ok() ||
			recorded_offset.value() != relative ||
			bin_size.value() < detail::hbin_alignment ||
			(bin_size.value() % detail::hbin_alignment) != 0 ||
			bin_size.value() > data_size.value() - relative)
		{
			return result_t<hive_t>::failure("Registry hbin range is invalid", ERROR_BADDB);
		}

		const std::size_t bin_end = relative + bin_size.value();
		std::size_t cell_relative = relative + detail::hbin_header_size;
		while (cell_relative < bin_end)
		{
			const std::size_t cell_absolute = detail::hive_header_size + cell_relative;
			auto cell_size_result = read_number<std::int32_t>(bytes, cell_absolute, "Registry hbin cell size");
			if (!cell_size_result.ok() || cell_size_result.value() == 0 ||
				cell_size_result.value() == std::numeric_limits<std::int32_t>::min())
			{
				return result_t<hive_t>::failure("Registry hbin cell size is invalid", ERROR_BADDB);
			}

			const std::size_t cell_size = static_cast<std::size_t>(
				cell_size_result.value() < 0 ? -cell_size_result.value() : cell_size_result.value());
			if (cell_size < 8U || (cell_size & 0x7U) != 0 || cell_size > bin_end - cell_relative)
			{
				return result_t<hive_t>::failure("Registry hbin cell crosses its bin boundary", ERROR_BADDB);
			}
			++cell_count;
			cell_relative += cell_size;
		}

		if (cell_relative != bin_end)
		{
			return result_t<hive_t>::failure("Registry hbin cell region is malformed", ERROR_BADDB);
		}

		hbins.push_back(impl_t::hbin_range_t{
			static_cast<std::uint32_t>(relative + detail::hbin_header_size),
			static_cast<std::uint32_t>(bin_end) });
		relative = bin_end;
	}

	if (cell_count == 0)
	{
		return result_t<hive_t>::failure("Registry hive contains no cells", ERROR_BADDB);
	}

	auto impl = std::make_unique<impl_t>();
	impl->file = std::move(mapped).value();
	impl->root_offset = root.value();
	impl->data_size = data_size.value();
	impl->cell_count = cell_count;
	impl->hbins = std::move(hbins);
	impl->key_cache.emplace(L"", root.value());

	auto root_name = impl->key_name(root.value());

	if (!root_name.ok())
	{
		return result_t<hive_t>::failure(root_name.error().message, root_name.error().code);
	}

	return result_t<hive_t>::success(hive_t(std::move(impl)));
}

result_t<std::vector<std::wstring>> hive_t::subkeys(std::wstring_view path) const
{
	if (!impl_)
	{
		return result_t<std::vector<std::wstring>>::failure("Registry hive is not open", ERROR_INVALID_HANDLE);
	}

	auto key = impl_->key_offset(path);

	if (!key.ok())
	{
		return result_t<std::vector<std::wstring>>::failure(key.error().message, key.error().code);
	}

	auto offsets = impl_->subkey_offsets(key.value());

	if (!offsets.ok())
	{
		return result_t<std::vector<std::wstring>>::failure(offsets.error().message, offsets.error().code);
	}

	std::vector<std::wstring> result;
	result.reserve(offsets.value().size());
	const std::wstring parent(path);

	for (const std::uint32_t offset : offsets.value())
	{
		auto name = impl_->key_name(offset);

		if (!name.ok())
		{
			return result_t<std::vector<std::wstring>>::failure(name.error().message, name.error().code);
		}

		std::wstring full = parent;
		if (!full.empty())
		{
			full.push_back(L'\\');
		}
		full.append(name.value());
		impl_->key_cache.emplace(std::move(full), offset);
		result.push_back(std::move(name).value());
	}

	std::sort(result.begin(), result.end(), ordinal_less_t{});
	for (std::size_t index = 1; index < result.size(); ++index)
	{
		if (equal_ordinal_ignore_case(result[index - 1U], result[index]))
		{
			return result_t<std::vector<std::wstring>>::failure(
				"Registry key contains duplicate subkey names",
				ERROR_BADDB);
		}
	}
	return result_t<std::vector<std::wstring>>::success(std::move(result));
}

result_t<std::vector<value_t>> hive_t::values(std::wstring_view path) const
{
	if (!impl_)
	{
		return result_t<std::vector<value_t>>::failure("Registry hive is not open", ERROR_INVALID_HANDLE);
	}

	auto key = impl_->key_offset(path);

	if (!key.ok())
	{
		return result_t<std::vector<value_t>>::failure(key.error().message, key.error().code);
	}

	auto offsets = impl_->value_offsets(key.value());

	if (!offsets.ok())
	{
		return result_t<std::vector<value_t>>::failure(offsets.error().message, offsets.error().code);
	}

	std::vector<value_t> result;
	result.reserve(offsets.value().size());

	for (const std::uint32_t offset : offsets.value())
	{
		auto parsed = impl_->parse_value(offset);

		if (!parsed.ok())
		{
			return result_t<std::vector<value_t>>::failure(parsed.error().message, parsed.error().code);
		}

		result.push_back(std::move(parsed).value());
	}

	std::sort(
		result.begin(),
		result.end(),
		[](const value_t& left, const value_t& right)
		{
			return ordinal_less_t{}(left.name, right.name);
		});
	for (std::size_t index = 1; index < result.size(); ++index)
	{
		if (equal_ordinal_ignore_case(result[index - 1U].name, result[index].name))
		{
			return result_t<std::vector<value_t>>::failure(
				"Registry key contains duplicate value names",
				ERROR_BADDB);
		}
	}

	return result_t<std::vector<value_t>>::success(std::move(result));
}

result_t<std::optional<value_t>> hive_t::value(
	std::wstring_view path,
	std::wstring_view name) const
{
	auto values_result = values(path);

	if (!values_result.ok())
	{
		if (values_result.error().code == ERROR_FILE_NOT_FOUND)
		{
			return result_t<std::optional<value_t>>::success(std::nullopt);
		}

		return result_t<std::optional<value_t>>::failure(
			values_result.error().message,
			values_result.error().code);
	}

	for (auto& item : values_result.value())
	{
		if (equal_ordinal_ignore_case(item.name, name))
		{
			return result_t<std::optional<value_t>>::success(std::move(item));
		}
	}

	return result_t<std::optional<value_t>>::success(std::nullopt);
}

result_t<std::optional<std::wstring>> hive_t::string(
	std::wstring_view path,
	std::wstring_view name) const
{
	auto value_result = value(path, name);

	if (!value_result.ok())
	{
		return result_t<std::optional<std::wstring>>::failure(
			value_result.error().message,
			value_result.error().code);
	}

	if (!value_result.value().has_value())
	{
		return result_t<std::optional<std::wstring>>::success(std::nullopt);
	}

	const auto& data = *value_result.value();

	if (data.type != REG_SZ && data.type != REG_EXPAND_SZ)
	{
		return result_t<std::optional<std::wstring>>::success(std::nullopt);
	}

	auto decoded = decode_utf16(
		std::span<const std::byte>(data.data.data(), data.data.size()));

	if (!decoded.ok())
	{
		return result_t<std::optional<std::wstring>>::failure(decoded.error().message, decoded.error().code);
	}

	auto text = std::move(decoded).value();

	while (!text.empty() && text.back() == L'\0')
	{
		text.pop_back();
	}

	return result_t<std::optional<std::wstring>>::success(std::move(text));
}

result_t<std::optional<std::uint32_t>> hive_t::dword(
	std::wstring_view path,
	std::wstring_view name) const
{
	auto value_result = value(path, name);

	if (!value_result.ok())
	{
		return result_t<std::optional<std::uint32_t>>::failure(
			value_result.error().message,
			value_result.error().code);
	}

	if (!value_result.value().has_value())
	{
		return result_t<std::optional<std::uint32_t>>::success(std::nullopt);
	}

	const auto& data = *value_result.value();

	if (data.type != REG_DWORD || data.data.size() != sizeof(std::uint32_t))
	{
		return result_t<std::optional<std::uint32_t>>::success(std::nullopt);
	}

	std::uint32_t result = 0;
	std::memcpy(&result, data.data.data(), sizeof(result));
	return result_t<std::optional<std::uint32_t>>::success(result);
}

result_t<std::vector<std::wstring>> hive_t::multi_string(
	std::wstring_view path,
	std::wstring_view name) const
{
	auto value_result = value(path, name);

	if (!value_result.ok())
	{
		return result_t<std::vector<std::wstring>>::failure(
			value_result.error().message,
			value_result.error().code);
	}

	if (!value_result.value().has_value() || value_result.value()->type != REG_MULTI_SZ)
	{
		return result_t<std::vector<std::wstring>>::success({});
	}

	auto decoded = decode_utf16(std::span<const std::byte>(
		value_result.value()->data.data(),
		value_result.value()->data.size()));

	if (!decoded.ok())
	{
		return result_t<std::vector<std::wstring>>::failure(decoded.error().message, decoded.error().code);
	}

	const std::wstring& text = decoded.value();
	std::vector<std::wstring> result;
	std::size_t cursor = 0;

	while (cursor < text.size())
	{
		std::size_t end = cursor;

		while (end < text.size() && text[end] != L'\0')
		{
			++end;
		}

		if (end == cursor)
		{
			break;
		}

		result.emplace_back(text.data() + cursor, end - cursor);
		cursor = end + 1;
	}

	return result_t<std::vector<std::wstring>>::success(std::move(result));
}

}
