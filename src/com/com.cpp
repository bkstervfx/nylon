#include "com.hpp"

#include "../database/database.hpp"
#include "../registry/value.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
	[[nodiscard]] std::wstring first_command_token( std::wstring_view command )
	{
		std::size_t start = 0;

		while ( start < command.size() && iswspace( command[start] ) )
		{
			++start;
		}

		if ( start >= command.size() )
		{
			return {};
		}

		if ( command[start] == L'"' )
		{
			const std::size_t end = command.find( L'"', start + 1U );

			if ( end == std::wstring_view::npos )
			{
				return {};
			}

			return std::wstring( command.substr( start + 1U, end - start - 1U ) );
		}

		const std::size_t end = command.find_first_of( L" \t", start );

		return std::wstring(
			command.substr(
				start,
				end == std::wstring_view::npos ? command.size() - start : end - start ) );
	}

	[[nodiscard]] bool starts_with_insensitive(
		std::wstring_view text,
		std::wstring_view prefix )
	{
		if ( text.size() < prefix.size() )
		{
			return false;
		}

		for ( std::size_t index = 0; index < prefix.size(); ++index )
		{
			if ( towlower( text[index] ) != towlower( prefix[index] ) )
			{
				return false;
			}
		}

		return true;
	}

	[[nodiscard]] std::filesystem::path resolve_server(
		const std::filesystem::path& image_root,
		std::wstring_view command )
	{
		std::wstring value = first_command_token( command );

		if ( value.empty() )
		{
			return {};
		}

		std::replace( value.begin(), value.end(), L'/', L'\\' );

		if ( starts_with_insensitive( value, L"%SystemRoot%\\" ) )
		{
			return image_root / L"Windows" / value.substr( 13U );
		}

		if ( starts_with_insensitive( value, L"%windir%\\" ) )
		{
			return image_root / L"Windows" / value.substr( 9U );
		}

		if ( starts_with_insensitive( value, L"System32\\" ) )
		{
			return image_root / L"Windows" / value;
		}

		if ( value.size() >= 3U && value[1] == L':' && value[2] == L'\\' )
		{
			return image_root / value.substr( 3U );
		}

		return {};
	}

	void scan_clsid_root(
		std::wstring_view prefix,
		const std::filesystem::path& image_root,
		nylon::database::database_t& database )
	{
		const auto keys = database.objects_with_path_prefix( L"registry_key", prefix );

		for ( const auto& key : keys )
		{
			const std::wstring remainder = key.path.substr( prefix.size() );

			if ( remainder.empty() || remainder.find( L'\\' ) != std::wstring::npos )
			{
				continue;
			}

			const auto class_id = database.object( L"com_class", remainder );
			database.edge( class_id, L"registered_at", key.id, L"registry CLSID" );

			const auto app_id = nylon::registry::string( database.registry_value( key.id, L"AppID" ) );

			if ( !app_id.empty() )
			{
				const auto app_object = database.object( L"com_appid_reference", app_id );
				database.edge( class_id, L"uses_appid", app_object, L"CLSID AppID value" );
			}

			const std::pair<const wchar_t*, const wchar_t*> servers[] =
			{
				{ L"InprocServer32", L"inproc_server" },
				{ L"LocalServer32", L"local_server" }
			};

			for ( const auto& [subkey_name, relation] : servers )
			{
				const auto server_key_id = database.find_object_by_path(
					L"registry_key",
					key.path + L"\\" + subkey_name );

				if ( server_key_id == 0 )
				{
					continue;
				}

				const auto server = nylon::registry::string(
					database.registry_value( server_key_id, L"" ) );

				if ( server.empty() )
				{
					continue;
				}

				database.property( class_id, relation, server );
				const auto resolved = resolve_server( image_root, server );

				if ( !resolved.empty() )
				{
					const auto file_id = database.find_object_by_path( L"file", resolved.native() );

					if ( file_id != 0 )
					{
						database.edge( class_id, relation, file_id, L"COM server registration" );
					}
				}
			}
		}
	}
}

namespace nylon::com
{
	void scan( const std::filesystem::path& image_root, database::database_t& database )
	{
		database.note( L"com", L"info", L"scan started" );

		const std::wstring_view roots[] =
		{
			L"SOFTWARE\\Classes\\CLSID\\",
			L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\",
			L"DEFAULT_USER\\Software\\Classes\\CLSID\\",
			L"DEFAULT_USER\\Software\\Classes\\WOW6432Node\\CLSID\\",
			L"DEFAULT_USER_CLASSES\\CLSID\\",
			L"DEFAULT_USER_CLASSES\\WOW6432Node\\CLSID\\"
		};

		for ( const auto root : roots )
		{
			scan_clsid_root( root, image_root, database );
		}

		database.note( L"com", L"info", L"scan completed" );
	}
}
