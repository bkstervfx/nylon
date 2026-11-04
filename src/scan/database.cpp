#include "database.hpp"
//
//
#include "database/staging.hpp"
#include "evidence/evidence.hpp"
//
//
namespace nylon::scan::internal
{
	//
	static status_t remove_staging( const std::filesystem::path& path )
	{
		const std::filesystem::path files[] =
		{
			path,
			std::filesystem::path( path.native() + L"-wal" ),
			std::filesystem::path( path.native() + L"-journal" ),
			std::filesystem::path( path.native() + L"-shm" )
		};
		//
		for ( const std::filesystem::path& file : files )
		{
			std::error_code error;
			std::filesystem::remove( file, error );
			//
			if ( error )
			{
				return status_t::failure
				(
					"Unable to remove stale audit database staging files",
					static_cast< uint32_t >( error.value() )
				);
			}
		}
		//
		return status_t::success();
	}
	//
	static status_t write_metadata
	(
		database::database_t& database,
		const image::image_t& image,
		const media::prepared_input_t& source
	)
	{
		auto path = image.utf8_path( image.root() );
		//
		if ( !path.ok() )
		{
			return status_t::failure( path.error().message, path.error().code );
		}
		//
		const std::pair<std::string_view, std::string_view> fields[] =
		{
			{ "schema_version", "7" },
			{ "mode", "read_only_static_audit" },
			{ "image_root", path.value() },
			{ "source_kind", source.source_kind }
		};
		//
		for ( const auto& [key, value] : fields )
		{
			const status_t result = database.meta( key, value );
			//
			if ( !result.ok() )
			{
				return result;
			}
		}
		//
		if ( source.source_kind == "iso" )
		{
			const std::string index = std::to_string( source.image_index );
			const std::pair<std::string_view, std::string_view> fields[] =
			{
				{ "source_iso_sha256", source.iso_sha256 },
				{ "source_image_index", index },
				{ "source_image_name", source.image_name },
				{ "source_cache", source.cache_hit ? "hit" : "created" }
			};
			//
			for ( const auto& [key, value] : fields )
			{
				const status_t result = database.meta( key, value );
				//
				if ( !result.ok() )
				{
					return result;
				}
			}
		}
		//
		return evidence::initialize( database, source );
	}
	//
	result_t<database::database_t> open_database
	(
		const std::filesystem::path& path,
		const image::image_t& image,
		const media::prepared_input_t& source
	)
	{
		status_t result = remove_staging( path );
		//
		if ( !result.ok() )
		{
			return result_t<database::database_t>::failure( result.error().message, result.error().code );
		}
		//
		auto opened = database::database_t::open( path );
		//
		if ( !opened.ok() )
		{
			return opened;
		}
		//
		database::database_t database = std::move( opened ).value();
		result = database::prepare_staging_database( database );
		//
		if ( !result.ok() )
		{
			return result_t<database::database_t>::failure( result.error().message, result.error().code );
		}
		//
		result = database.initialize();
		//
		if ( !result.ok() )
		{
			return result_t<database::database_t>::failure( result.error().message, result.error().code );
		}
		//
		result = database::activate_staging_database( database );
		//
		if ( !result.ok() )
		{
			return result_t<database::database_t>::failure( result.error().message, result.error().code );
		}
		//
		result = write_metadata( database, image, source );
		//
		if ( !result.ok() )
		{
			return result_t<database::database_t>::failure( result.error().message, result.error().code );
		}
		//
		return result_t<database::database_t>::success( std::move( database ) );
	}
	//
	status_t finish_registry( database::database_t& database )
	{
		sqlite3_stmt* prepared = nullptr;
		int result = ::sqlite3_prepare_v2
		(
			database.native_handle(),
			"SELECT state FROM collector_runs WHERE collector='registry';",
			-1,
			&prepared,
			nullptr
		);
		//
		database::statement_t statement( prepared );
		//
		if ( result != SQLITE_OK )
		{
			return status_t::failure( "Unable to query registry accounting state", static_cast< uint32_t >( result ) );
		}
		//
		result = ::sqlite3_step( statement.get() );
		//
		if ( result != SQLITE_ROW )
		{
			return status_t::failure( "Registry accounting state did not return a result", static_cast< uint32_t >( result ) );
		}
		//
		const char* state = reinterpret_cast< const char* >( ::sqlite3_column_text( statement.get(), 0 ) );
		//
		if ( state == nullptr )
		{
			return status_t::failure( "Registry accounting state is null" );
		}
		//
		const bool gaps = std::string_view( state ) != "COMPLETE";
		result = ::sqlite3_step( statement.get() );
		//
		if ( result != SQLITE_DONE )
		{
			return status_t::failure( "Unable to finish registry accounting query", static_cast< uint32_t >( result ) );
		}
		//
		result = ::sqlite3_finalize( statement.release() );
		//
		if ( result != SQLITE_OK )
		{
			return status_t::failure( "Unable to finalize registry accounting query", static_cast< uint32_t >( result ) );
		}
		//
		return database.collector_status( "registry", gaps ? "complete_with_gaps" : "complete" );
	}
	//
	status_t publish_database
	(
		database::database_t& database,
		const std::filesystem::path& staging,
		const std::filesystem::path& destination
	)
	{
		status_t result = database.finalize_graph();
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		result = evidence::finalize( database );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		result = database::quick_check( database );
		//
		if ( !result.ok() )
		{
			return result;
		}
		//
		database.close();
		//
		const std::filesystem::path sidecars[] =
		{
			std::filesystem::path( destination.native() + L"-wal" ),
			std::filesystem::path( destination.native() + L"-shm" )
		};
		//
		for ( const std::filesystem::path& path : sidecars )
		{
			std::error_code error;
			std::filesystem::remove( path, error );
			//
			if ( error )
			{
				return status_t::failure
				(
					"Unable to remove stale audit database sidecar",
					static_cast< uint32_t >( error.value() )
				);
			}
		}
		//
		if ( !::MoveFileExW( staging.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
		{
			return status_t::failure( "Unable to publish audit database", ::GetLastError() );
		}
		//
		return status_t::success();
	}
	//
}
