#include "shell.hpp"

#include "../database/database.hpp"
#include "../registry/value.hpp"

#include <string>
#include <string_view>

namespace
{
	void scan_classes_root(
		std::wstring_view classes_prefix,
		nylon::database::database_t& database )
	{
		const auto keys = database.objects_with_path_prefix( L"registry_key", classes_prefix );

		for ( const auto& key : keys )
		{
			const std::wstring remainder = key.path.substr( classes_prefix.size() );

			if ( remainder.empty() )
			{
				continue;
			}

			const bool shell_key =
				remainder.find( L"\\shell\\" ) != std::wstring::npos ||
				remainder.find( L"\\shellex\\" ) != std::wstring::npos;

			if ( shell_key )
			{
				const auto registration_id = database.object(
					L"shell_registration",
					remainder,
					key.path );

				database.edge(
					registration_id,
					L"registered_at",
					key.id,
					L"registry Classes shell registration" );

				const auto default_value = nylon::registry::string(
					database.registry_value( key.id, L"" ) );

				if ( !default_value.empty() )
				{
					database.property( registration_id, L"default", default_value );
				}
			}

			if ( remainder.find( L'\\' ) == std::wstring::npos )
			{
				const auto protocol_marker = database.registry_value( key.id, L"URL Protocol" );

				if ( protocol_marker )
				{
					const auto protocol_id = database.object( L"url_protocol", remainder );
					database.edge(
						protocol_id,
						L"registered_at",
						key.id,
						L"registry Classes URL Protocol" );

					const auto command_id = database.find_object_by_path(
						L"registry_key",
						key.path + L"\\shell\\open\\command" );

					if ( command_id != 0 )
					{
						const auto command = nylon::registry::string(
							database.registry_value( command_id, L"" ) );

						if ( !command.empty() )
						{
							database.property( protocol_id, L"command", command );
						}
					}
				}
			}
		}
	}
}

namespace nylon::shell
{
	void scan( database::database_t& database )
	{
		database.note( L"shell", L"info", L"scan started" );

		const std::wstring_view roots[] =
		{
			L"SOFTWARE\\Classes\\",
			L"DEFAULT_USER\\Software\\Classes\\",
			L"DEFAULT_USER_CLASSES\\"
		};

		for ( const auto root : roots )
		{
			scan_classes_root( root, database );
		}

		database.note( L"shell", L"info", L"scan completed" );
	}
}
