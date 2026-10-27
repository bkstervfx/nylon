#ifndef NYLON_REGISTRY_VALUE_HPP
#define NYLON_REGISTRY_VALUE_HPP

#include "../database/database.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace nylon::registry
{
	[[nodiscard]] inline std::optional<std::uint32_t> dword(
		const std::optional<database::registry_value_t>& value )
	{
		if ( !value ||
			value->type != REG_DWORD ||
			value->data.size() != sizeof( std::uint32_t ) )
		{
			return std::nullopt;
		}

		std::uint32_t result = 0;
		std::memcpy( &result, value->data.data(), sizeof( result ) );
		return result;
	}

	[[nodiscard]] inline std::wstring string(
		const std::optional<database::registry_value_t>& value )
	{
		if ( !value ||
			( value->type != REG_SZ && value->type != REG_EXPAND_SZ ) ||
			value->data.size() < sizeof( wchar_t ) ||
			( value->data.size() % sizeof( wchar_t ) ) != 0 )
		{
			return {};
		}

		const std::size_t characters = value->data.size() / sizeof( wchar_t );
		std::vector<wchar_t> buffer( characters );
		std::memcpy( buffer.data(), value->data.data(), value->data.size() );

		const std::size_t length = wcsnlen_s( buffer.data(), buffer.size() );

		if ( length >= buffer.size() )
		{
			return {};
		}

		return std::wstring( buffer.data(), length );
	}

	[[nodiscard]] inline std::vector<std::wstring> multi_string(
		const std::optional<database::registry_value_t>& value )
	{
		if ( !value ||
			value->type != REG_MULTI_SZ ||
			value->data.size() < 2U * sizeof( wchar_t ) ||
			( value->data.size() % sizeof( wchar_t ) ) != 0 )
		{
			return {};
		}

		const std::size_t characters = value->data.size() / sizeof( wchar_t );
		std::vector<wchar_t> buffer( characters );
		std::memcpy( buffer.data(), value->data.data(), value->data.size() );

		std::vector<std::wstring> result;
		std::size_t offset = 0;

		while ( offset < buffer.size() && buffer[offset] != L'\0' )
		{
			const std::size_t remaining = buffer.size() - offset;
			const std::size_t length = wcsnlen_s( buffer.data() + offset, remaining );

			if ( length == 0 || length >= remaining )
			{
				break;
			}

			result.emplace_back( buffer.data() + offset, length );
			offset += length + 1U;
		}

		return result;
	}
}

#endif
