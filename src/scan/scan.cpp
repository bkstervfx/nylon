#include "scan.hpp"
//
//
#include "scan/database.hpp"
#include "evidence/evidence.hpp"
#include "platform/win32.hpp"
#include "progress/progress.hpp"
#include "registry/registry.hpp"
#include "services/control_sets.hpp"
//
#include <iostream>
//
//
namespace nylon::scan
{
	//
	namespace internal
	{
		//
		static bool is_project_root( const std::filesystem::path& path )
		{
			std::error_code error;
			//
			return std::filesystem::is_regular_file( path / L"build.cmd", error ) && !error;
		}
		//
		static result_t<std::filesystem::path> project_root()
		{
			auto executable = platform::executable_path();
			//
			if ( !executable.ok() )
			{
				return result_t<std::filesystem::path>::failure( executable.error().message, executable.error().code );
			}
			//
			const std::filesystem::path parent = executable.value().parent_path().lexically_normal();
			//
			if ( ::CompareStringOrdinal( parent.filename().c_str(), -1, L"bin", -1, TRUE ) == CSTR_EQUAL )
			{
				const std::filesystem::path root = parent.parent_path().lexically_normal();
				//
				if ( is_project_root( root ) )
				{
					return result_t<std::filesystem::path>::success( root );
				}
			}
			//
			return result_t<std::filesystem::path>::success( parent );
		}
		//
		static result_t<std::filesystem::path> runtime_root()
		{
			const DWORD required = ::GetEnvironmentVariableW( L"LOCALAPPDATA", nullptr, 0 );
			//
			if ( required == 0 )
			{
				return result_t<std::filesystem::path>::failure( "LOCALAPPDATA is not available", ::GetLastError() );
			}
			//
			std::wstring directory( required, L'\0' );
			const DWORD written = ::GetEnvironmentVariableW( L"LOCALAPPDATA", directory.data(), required );
			//
			if ( written == 0 || written >= required )
			{
				return result_t<std::filesystem::path>::failure( "Unable to read LOCALAPPDATA", ::GetLastError() );
			}
			//
			directory.resize( written );
			//
			return result_t<std::filesystem::path>::success
			(
				std::filesystem::path( std::move( directory ) ) / L"Temp" / L"nylon"
			);
		}
		//
		static status_t create_directories
		(
			const std::filesystem::path& output,
			const std::filesystem::path& temp,
			const std::filesystem::path& cache
		)
		{
			for ( const std::filesystem::path& directory : { output, temp, cache } )
			{
				std::error_code error;
				std::filesystem::create_directories( directory, error );
				//
				if ( error )
				{
					return status_t::failure
					(
						"Unable to create Nylon runtime directory",
						static_cast< uint32_t >( error.value() )
					);
				}
			}
			//
			return status_t::success();
		}
		//
		static result_t<platform::unique_handle_t> lock_source( const std::filesystem::path& path )
		{
			const std::wstring extension = path.extension().native();
			const bool iso = ::CompareStringOrdinal
			(
				extension.c_str(),
				static_cast< int >( extension.size() ),
				L".iso",
				4,
				TRUE
			) == CSTR_EQUAL;
			//
			std::error_code error;
			//
			if ( iso && std::filesystem::is_regular_file( path, error ) && !error )
			{
				return platform::open_read_lock( path );
			}
			//
			return result_t<platform::unique_handle_t>::success( platform::unique_handle_t{} );
		}
		//
		static void print_source( const media::prepared_input_t& source )
		{
			if ( source.source_kind != "iso" )
			{
				return;
			}
			//
			std::cout << "iso sha256: " << source.iso_sha256 << '\n';
			std::cout << "image index: " << source.image_index;
			//
			if ( !source.image_name.empty() )
			{
				std::cout << " (" << source.image_name << ")";
			}
			//
			std::cout << '\n';
			std::cout << "cache: " << ( source.cache_hit ? "hit" : "created" ) << '\n';
		}
		//
		static status_t collect_registry( image::image_t& image, database::database_t& database )
		{
			status_t result = database.collector_status( "registry", "running" );
			//
			if ( !result.ok() )
			{
				return result;
			}
			//
			result = database.begin();
			//
			if ( !result.ok() )
			{
				return result;
			}
			//
			progress::registry_state_t state;
			progress::registry_view_t view( state );
			//
			view.start();
			result = registry::scan_all( image, database, &state );
			view.finish( result.ok() );
			//
			if ( result.ok() )
			{
				result = database.commit();
			}
			//
			if ( !result.ok() )
			{
				database.rollback();
				database.collector_status( "registry", "failed", result.error().message );
				//
				return result;
			}
			//
			return finish_registry( database );
		}
		//
		static status_t collect_services( database::database_t& database )
		{
			status_t result = database.begin();
			//
			if ( result.ok() )
			{
				result = services::collect_control_sets( database );
			}
			//
			if ( result.ok() )
			{
				result = database.commit();
			}
			//
			if ( !result.ok() )
			{
				database.rollback();
			}
			//
			return result;
		}
		//
	}
	//
	status_t run( const options_t& options )
	{
		auto root = internal::project_root();
		//
		if ( !root.ok() )
		{
			return status_t::failure( root.error().message, root.error().code );
		}
		//
		auto runtime = internal::runtime_root();
		//
		if ( !runtime.ok() )
		{
			return status_t::failure( runtime.error().message, runtime.error().code );
		}
		//
		const std::filesystem::path output = root.value() / L"output";
		const std::filesystem::path temp = runtime.value() / L"temp";
		const std::filesystem::path cache = runtime.value() / L"cache";
		const std::filesystem::path destination = output / L"image.db";
		const std::filesystem::path staging = output / L"image.db.staging";
		//
		status_t result = internal::create_directories( output, temp, cache );
		//
		if ( !result.ok() )
		{
			return result;
		}
		// Keep the source lock alive through collection and database publication.
		auto lock = internal::lock_source( options.input );
		//
		if ( !lock.ok() )
		{
			return status_t::failure( lock.error().message, lock.error().code );
		}
		//
		media::prepare_options_t preparation;
		preparation.nylon_root = runtime.value();
		preparation.cache_root = cache;
		preparation.temp_root = temp;
		preparation.image_index = options.image_index;
		//
		std::cout << "input...\n";
		auto source = media::prepare( options.input, preparation );
		//
		if ( !source.ok() )
		{
			return status_t::failure( source.error().message, source.error().code );
		}
		//
		const media::prepared_input_t prepared = std::move( source ).value();
		internal::print_source( prepared );
		//
		result = media::verify_prepared_cache( prepared );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		auto opened = image::image_t::open( prepared.image_root );
		//
		if ( !opened.ok() )
		{
			return status_t::failure( opened.error().message, opened.error().code );
		}
		//
		image::image_t image = std::move( opened ).value();
		auto audit = internal::open_database( staging, image, prepared );
		//
		if ( !audit.ok() )
		{
			return status_t::failure( audit.error().message, audit.error().code );
		}
		//
		database::database_t database = std::move( audit ).value();
		//
		std::cout << "filesystem namespace...\n";
		result = evidence::inventory( database, image.root() );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		result = internal::collect_registry( image, database );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		std::cout << "offline control sets and service inventory...\n";
		result = internal::collect_services( database );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		result = internal::publish_database( database, staging, destination );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		std::cout << "current collector pass complete; whole-image collector migration pending\n";
		auto display = platform::utf8( destination.native() );
		//
		std::cout << "database: "
			<< ( display.ok() ? display.value() : std::string( "<path encoding error>" ) )
			<< '\n';
		//
		return status_t::success();
	}
	//
}
