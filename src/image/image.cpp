#include "image.hpp"

#include "../appx/appx.hpp"
#include "../com/com.hpp"
#include "../database/database.hpp"
#include "../filesystem/filesystem.hpp"
#include "../pe/pe.hpp"
#include "../registry/registry.hpp"
#include "../servicing/servicing.hpp"
#include "../services/services.hpp"
#include "../shell/shell.hpp"
#include "../tasks/tasks.hpp"

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string>

namespace
{
	[[nodiscard]] std::wstring lower( std::wstring value )
	{
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[]( wchar_t character )
			{
				return static_cast<wchar_t>( std::towlower( character ) );
			} );

		return value;
	}

	[[nodiscard]] bool path_is_under(
		const std::filesystem::path& child,
		const std::filesystem::path& parent )
	{
		const std::wstring child_text = lower( std::filesystem::weakly_canonical( child ).native() );
		std::wstring parent_text = lower( std::filesystem::weakly_canonical( parent ).native() );

		if ( !parent_text.ends_with( L'\\' ) )
		{
			parent_text.push_back( L'\\' );
		}

		return child_text == parent_text.substr( 0, parent_text.size() - 1 ) ||
			child_text.starts_with( parent_text );
	}
}

namespace nylon::image
{
	paths_t resolve_paths(
		const std::filesystem::path& executable,
		const std::filesystem::path& requested_image )
	{
		const auto executable_path = std::filesystem::weakly_canonical( executable );
		const auto bin_root = executable_path.parent_path();
		const auto project_root = bin_root.parent_path();

		if ( lower( project_root.filename().native() ) != L"nylon" )
		{
			throw std::runtime_error( "nylon.exe must run from the project's bin directory" );
		}

		const auto image_root = requested_image.empty()
			? project_root / L"image"
			: std::filesystem::weakly_canonical( requested_image );

		if ( !path_is_under( image_root, project_root ) )
		{
			throw std::runtime_error( "the audited image must be located under the Nylon project root" );
		}

		paths_t result
		{
			.project_root = project_root,
			.image_root = image_root,
			.temp_root = project_root / L"temp",
			.cache_root = project_root / L"cache",
			.output_root = project_root / L"output"
		};

		std::filesystem::create_directories( result.temp_root );
		std::filesystem::create_directories( result.cache_root );
		std::filesystem::create_directories( result.output_root );

		if ( !std::filesystem::is_directory( result.image_root / L"Windows" ) )
		{
			throw std::runtime_error( "image root does not contain a Windows directory" );
		}

		return result;
	}

	void audit( const paths_t& paths, database::database_t& database )
	{
		database.begin();

		try
		{
			filesystem::scan( paths.image_root, database );
			registry::scan( paths.image_root, paths.temp_root, database );
			services::scan( paths.image_root, database );
			appx::scan( paths.image_root, database );
			servicing::scan( paths, database );
			tasks::scan( paths.image_root, database );
			pe::scan( paths.image_root, database );
			com::scan( paths.image_root, database );
			shell::scan( database );

			database.commit();
		}
		catch ( ... )
		{
			database.rollback();
			throw;
		}
	}
}
