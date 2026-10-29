#ifndef NYLON_REGISTRY_DETAIL_HPP
#define NYLON_REGISTRY_DETAIL_HPP

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../result.hpp"

namespace nylon::registry::detail
{

constexpr std::size_t hive_header_size = 0x1000U;
constexpr std::size_t hbin_header_size = 0x20U;
constexpr std::size_t hbin_alignment = 0x1000U;
constexpr std::size_t max_big_data_segment_size = 16344U;
constexpr std::uint16_t nk_compressed_name = 0x0020U;
constexpr std::uint16_t vk_compressed_name = 0x0001U;
constexpr std::uint32_t inline_data_flag = 0x80000000U;
constexpr std::uint32_t invalid_cell_offset = 0xFFFFFFFFU;
constexpr std::size_t max_subkey_list_depth = 64U;

struct ordinal_less_t
{
	bool operator()(const std::wstring& left, const std::wstring& right) const
	{
		const int result = CompareStringOrdinal(
			left.data(),
			static_cast<int>(left.size()),
			right.data(),
			static_cast<int>(right.size()),
			TRUE);

		return result == CSTR_LESS_THAN;
	}
};

inline bool equal_ordinal_ignore_case(
	std::wstring_view left,
	std::wstring_view right)
{
	if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
		right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return false;
	}

	return CompareStringOrdinal(
		left.data(),
		static_cast<int>(left.size()),
		right.data(),
		static_cast<int>(right.size()),
		TRUE) == CSTR_EQUAL;
}

template <typename T>
result_t<T> read_number(
	std::span<const std::byte> bytes,
	std::size_t offset,
	std::string_view context)
{
	static_assert(std::is_trivially_copyable_v<T>);

	if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
	{
		return result_t<T>::failure(
			std::string(context) + " is outside the hive",
			ERROR_BADDB);
	}

	T value{};
	std::memcpy(&value, bytes.data() + offset, sizeof(value));
	return result_t<T>::success(value);
}

inline result_t<std::wstring> decode_utf16(std::span<const std::byte> data)
{
	if ((data.size() % sizeof(wchar_t)) != 0)
	{
		return result_t<std::wstring>::failure(
			"Registry UTF-16 value has an odd byte length",
			ERROR_BADDB);
	}

	const std::size_t count = data.size() / sizeof(wchar_t);
	std::wstring result(count, L'\0');

	if (!data.empty())
	{
		std::memcpy(result.data(), data.data(), data.size());
	}

	return result_t<std::wstring>::success(std::move(result));
}

inline result_t<std::wstring> decode_registry_name(
	std::span<const std::byte> bytes,
	bool compressed)
{
	if (!compressed)
	{
		return decode_utf16(bytes);
	}

	std::wstring result;
	result.reserve(bytes.size());

	for (const std::byte byte : bytes)
	{
		result.push_back(static_cast<wchar_t>(std::to_integer<unsigned char>(byte)));
	}

	return result_t<std::wstring>::success(std::move(result));
}

}

#endif
