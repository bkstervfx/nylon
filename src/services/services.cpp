#include "services.hpp"

#include "../database/database.hpp"
#include "../registry/value.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

	[[nodiscard]] std::filesystem::path resolve_image_path(
		const std::filesystem::path& image_root,
		std::wstring_view command )
	{
		std::wstring value = first_command_token( command );

		if ( value.empty() )
		{
			return {};
		}

		std::replace( value.begin(), value.end(), L'/', L'\\' );

		const std::wstring_view system_root_prefix = L"\\SystemRoot\\";
		const std::wstring_view env_system_root_prefix = L"%SystemRoot%\\";
		const std::wstring_view env_windir_prefix = L"%windir%\\";

		if ( starts_with_insensitive( value, system_root_prefix ) )
		{
			return image_root / L"Windows" / value.substr( system_root_prefix.size() );
		}

		if ( starts_with_insensitive( value, env_system_root_prefix ) )
		{
			return image_root / L"Windows" / value.substr( env_system_root_prefix.size() );
		}

		if ( starts_with_insensitive( value, env_windir_prefix ) )
		{
			return image_root / L"Windows" / value.substr( env_windir_prefix.size() );
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
}

namespace nylon::services
{
	void scan( const std::filesystem::path& image_root, database::database_t& database )
	{
		database.note( L"services", L"info", L"scan started" );

		const auto select_id = database.find_object_by_path( L"registry_key", L"SYSTEM\\Select" );
		std::uint32_t control_set = 1;

		if ( select_id != 0 )
		{
			control_set = nylon::registry::dword( database.registry_value( select_id, L"Current" ) ).value_or( 1 );
		}

		wchar_t prefix_buffer[96]{};
		swprintf_s(
			prefix_buffer,
			L"SYSTEM\\ControlSet%03u\\Services\\",
			control_set );

		const std::wstring services_prefix = prefix_buffer;
		const auto keys = database.objects_with_path_prefix( L"registry_key", services_prefix );

		for ( const auto& key : keys )
		{
			const std::wstring remainder = key.path.substr( services_prefix.size() );

			if ( remainder.empty() || remainder.find( L'\\' ) != std::wstring::npos )
			{
				continue;
			}

			const auto type = nylon::registry::dword( database.registry_value( key.id, L"Type" ) ).value_or( 0 );
			const bool is_driver =
				( type & SERVICE_KERNEL_DRIVER ) != 0 ||
				( type & SERVICE_FILE_SYSTEM_DRIVER ) != 0 ||
				( type & SERVICE_RECOGNIZER_DRIVER ) != 0;

			const auto object_id = database.object(
				is_driver ? L"driver_service" : L"service",
				remainder );

			database.edge( object_id, L"registered_at", key.id, L"SYSTEM Services" );
			database.property( object_id, L"type", std::to_wstring( type ) );

			if ( const auto start = nylon::registry::dword( database.registry_value( key.id, L"Start" ) ) )
			{
				database.property( object_id, L"start", std::to_wstring( *start ) );
			}

			const auto image_path = nylon::registry::string( database.registry_value( key.id, L"ImagePath" ) );

			if ( !image_path.empty() )
			{
				database.property( object_id, L"image_path", image_path );
				const auto resolved = resolve_image_path( image_root, image_path );

				if ( !resolved.empty() )
				{
					const auto file_id = database.find_object_by_path( L"file", resolved.native() );

					if ( file_id != 0 )
					{
						database.edge( object_id, L"uses", file_id, L"SYSTEM Services ImagePath" );
					}
				}
			}

			const auto object_name = nylon::registry::string( database.registry_value( key.id, L"ObjectName" ) );

			if ( !object_name.empty() )
			{
				database.property( object_id, L"account", object_name );
			}

			for ( const auto& dependency : nylon::registry::multi_string( database.registry_value( key.id, L"DependOnService" ) ) )
			{
				const auto dependency_id = database.object( L"service_reference", dependency );
				database.edge( object_id, L"depends_on", dependency_id, L"SYSTEM DependOnService" );
			}

			for ( const auto& dependency : nylon::registry::multi_string( database.registry_value( key.id, L"DependOnGroup" ) ) )
			{
				const auto dependency_id = database.object( L"service_group", dependency );
				database.edge( object_id, L"depends_on_group", dependency_id, L"SYSTEM DependOnGroup" );
			}

			const auto parameters_id = database.find_object_by_path(
				L"registry_key",
				key.path + L"\\Parameters" );

			if ( parameters_id != 0 )
			{
				const auto service_dll = nylon::registry::string( database.registry_value( parameters_id, L"ServiceDll" ) );

				if ( !service_dll.empty() )
				{
					database.property( object_id, L"service_dll", service_dll );
					const auto resolved = resolve_image_path( image_root, service_dll );

					if ( !resolved.empty() )
					{
						const auto file_id = database.find_object_by_path( L"file", resolved.native() );

						if ( file_id != 0 )
						{
							database.edge( object_id, L"uses", file_id, L"SYSTEM Parameters ServiceDll" );
						}
					}
				}
			}
		}

		database.note( L"services", L"info", L"scan completed" );
	}
}
