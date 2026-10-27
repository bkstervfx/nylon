#include "tasks.hpp"

#include "../database/database.hpp"
#include "../xml/xml.hpp"

#include <filesystem>
#include <string>

namespace nylon::tasks
{
	void scan( const std::filesystem::path& image_root, database::database_t& database )
	{
		database.note( L"tasks", L"info", L"scan started" );

		const auto root = image_root / L"Windows" / L"System32" / L"Tasks";

		if ( !std::filesystem::is_directory( root ) )
		{
			database.note( L"tasks", L"warning", L"scheduled-task directory is absent" );
			return;
		}

		std::error_code error;

		for ( std::filesystem::recursive_directory_iterator iterator(
			root,
			std::filesystem::directory_options::skip_permission_denied,
			error ), end;
			iterator != end;
			iterator.increment( error ) )
		{
			if ( error )
			{
				error.clear();
				continue;
			}

			if ( !iterator->is_regular_file( error ) || error )
			{
				error.clear();
				continue;
			}

			const auto path = iterator->path();
			const auto task_id = database.object(
				L"scheduled_task",
				std::filesystem::relative( path, root, error ).native(),
				path.native() );

			error.clear();

			try
			{
				xml::read(
					path,
					[]( std::wstring_view, const auto& )
					{
					},
					[&]( std::wstring_view element, std::wstring_view text )
					{
						if ( element == L"Command" && !text.empty() )
						{
							database.property( task_id, L"command", text );

							const auto command_id = database.object( L"command_reference", text );
							database.edge( task_id, L"executes", command_id, L"task XML Command" );
						}
						else if ( element == L"Arguments" && !text.empty() )
						{
							database.property( task_id, L"arguments", text );
						}
						else if ( element == L"ClassId" && !text.empty() )
						{
							const auto clsid_id = database.object( L"com_clsid_reference", text );
							database.edge( task_id, L"activates", clsid_id, L"task XML ClassId" );
						}
						else if ( element == L"URI" && !text.empty() )
						{
							database.property( task_id, L"uri", text );
						}
					} );
			}
			catch ( const std::exception& )
			{
				database.note( L"tasks", L"warning", L"task XML parse failed: " + path.native() );
			}
		}

		database.note( L"tasks", L"info", L"scan completed" );
	}
}
