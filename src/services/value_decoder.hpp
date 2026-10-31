#ifndef NYLON_SERVICES_VALUE_DECODER_HPP
#define NYLON_SERVICES_VALUE_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../platform/win32.hpp"

namespace nylon::services
{

inline constexpr std::string_view service_value_parser_version = "service-value-v1";
inline constexpr std::size_t max_service_value_bytes = 2U * 1024U * 1024U;

struct decoded_value_t
{
	std::string state = "UNSUPPORTED_FORMAT";
	std::string kind = "UNSUPPORTED";
	std::string detail = "Registry type not interpreted by this parser";
	std::optional<std::uint32_t> number;
	std::vector<std::string> strings;
};

[[nodiscard]] inline decoded_value_t decode_service_value(std::int64_t type, std::span<const unsigned char> bytes)
{
	decoded_value_t result;
	if (bytes.size() > max_service_value_bytes)
	{
		result.detail = "Value exceeds explicit 2 MiB parser limit; raw bytes retained";
		return result;
	}
	if (type != 1 && type != 2 && type != 4 && type != 7) return result;
	result.kind = type == 4 ? "DWORD" : type == 7 ? "MULTI_SZ" : type == 2 ? "EXPAND_SZ" : "SZ";
	result.state = "PARSE_FAILED";
	if (type == 4)
	{
		result.detail = "REG_DWORD requires exactly four bytes";
		if (bytes.size() != 4) return result;
		result.number = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
			(static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
	}
	else
	{
		result.detail = "Expected terminated UTF-16LE string data with no trailing payload";
		if (bytes.empty() || bytes.size() % 2 != 0) return result;
		std::wstring units;
		units.reserve(bytes.size() / 2);
		for (std::size_t offset = 0; offset < bytes.size(); offset += 2)
			units.push_back(static_cast<wchar_t>(static_cast<unsigned>(bytes[offset]) | (static_cast<unsigned>(bytes[offset + 1]) << 8U)));
		if (units.back() != L'\0') return result;
		std::vector<std::string> strings;
		if (type == 7)
		{
			if (units.size() < 2 || units[units.size() - 2] != L'\0') return result;
			if (units.size() != 2)
			{
				std::size_t start = 0;
				while (start < units.size() - 1)
				{
					const auto end = units.find(L'\0', start);
					if (end == start || end == std::wstring::npos) return result;
					auto converted = platform::utf8(std::wstring_view(units).substr(start, end - start));
					if (!converted.ok()) { result.detail = "Invalid UTF-16LE string encoding"; return result; }
					strings.push_back(std::move(converted).value());
					start = end + 1;
				}
			}
			else if (units[0] != L'\0') return result;
		}
		else
		{
			if (units.find(L'\0') != units.size() - 1) return result;
			auto converted = platform::utf8(std::wstring_view(units).substr(0, units.size() - 1));
			if (!converted.ok()) { result.detail = "Invalid UTF-16LE string encoding"; return result; }
			strings.push_back(std::move(converted).value());
		}
		result.strings = std::move(strings);
	}
	result.state = "PRESENT";
	result.detail = "Strict typed decoding only; no environment expansion, service validity or dependency inference";
	return result;
}

}

#endif
