#include "common.hpp"
//
//
#include "scan/database.hpp"
//
#include <iostream>
#include <stdexcept>
//
//
namespace nylon::tests::internal
{
	//
	static void require( bool condition, const char* message )
	{
		if ( !condition )
		{
			throw std::runtime_error( message );
		}
	}
	//
	static void execute( database::database_t& database, const char* query )
	{
		const int result = ::sqlite3_exec( database.native_handle(), query, nullptr, nullptr, nullptr );
		//
		if ( result != SQLITE_OK )
		{
			throw std::runtime_error( ::sqlite3_errmsg( database.native_handle() ) );
		}
	}
	//
	static std::string registry_status( database::database_t& database )
	{
		sqlite3_stmt* prepared = nullptr;
		const int result = ::sqlite3_prepare_v2
		(
			database.native_handle(),
			"SELECT state FROM collector_status WHERE collector='registry';",
			-1,
			&prepared,
			nullptr
		);
		//
		database::statement_t statement( prepared );
		require( result == SQLITE_OK, "Unable to prepare fixture status query" );
		require( ::sqlite3_step( statement.get() ) == SQLITE_ROW, "Missing fixture status" );
		//
		const char* state = reinterpret_cast< const char* >( ::sqlite3_column_text( statement.get(), 0 ) );
		require( state != nullptr, "Null fixture status" );
		//
		return state;
	}
	//
	static void expect_failure( database::database_t& database )
	{
		const status_t result = scan::internal::finish_registry( database );
		//
		require( !result.ok(), "Invalid registry accounting was accepted" );
		require( registry_status( database ) == "running", "Failed query changed registry status" );
	}
	//
	static void query_failure( sqlite3_context* context, int, sqlite3_value** )
	{
		::sqlite3_result_error( context, "Synthetic accounting step failure", -1 );
	}
	//
	static void check_registry( database::database_t& database )
	{
		require( database.collector_status( "registry", "running" ).ok(), "Unable to set fixture status" );
		//
		expect_failure( database );
		execute( database, "CREATE TABLE collector_runs(collector TEXT,state TEXT);" );
		expect_failure( database );
		//
		execute( database, "INSERT INTO collector_runs VALUES('registry',NULL);" );
		expect_failure( database );
		//
		execute( database, "DELETE FROM collector_runs; INSERT INTO collector_runs VALUES('registry','COMPLETE'),('registry','PARTIAL');" );
		expect_failure( database );
		//
		execute( database, "DELETE FROM collector_runs; INSERT INTO collector_runs VALUES('registry','COMPLETE');" );
		require( scan::internal::finish_registry( database ).ok(), "Complete registry state was rejected" );
		require( registry_status( database ) == "complete", "Complete registry state was not preserved" );
		//
		execute( database, "UPDATE collector_runs SET state='COMPLETE_WITH_GAPS';" );
		require( scan::internal::finish_registry( database ).ok(), "Registry gaps were rejected" );
		require( registry_status( database ) == "complete_with_gaps", "Registry gaps were lost" );
		//
		require( database.collector_status( "registry", "running" ).ok(), "Unable to reset fixture status" );
		execute( database, "DROP TABLE collector_runs;" );
		//
		const int result = ::sqlite3_create_function_v2
		(
			database.native_handle(),
			"query_failure",
			0,
			SQLITE_UTF8,
			nullptr,
			query_failure,
			nullptr,
			nullptr,
			nullptr
		);
		//
		require( result == SQLITE_OK, "Unable to register fixture failure function" );
		execute( database, "CREATE VIEW collector_runs AS SELECT 'registry' AS collector,query_failure() AS state;" );
		expect_failure( database );
	}
	//
}
//
//
int main()
{
	try
	{
		auto opened = nylon::database::database_t::open( L":memory:" );
		nylon::tests::internal::require( opened.ok(), "Unable to open fixture database" );
		//
		nylon::database::database_t database = std::move( opened ).value();
		nylon::tests::internal::require( database.initialize().ok(), "Unable to initialize fixture database" );
		//
		nylon::tests::internal::check_registry( database );
		//
		std::cout << "PASS: registry accounting query failures, missing/null/duplicate states and compatibility status\n";
		//
		return 0;
	}
	catch ( const std::exception& error )
	{
		std::cerr << error.what() << '\n';
		//
		return 1;
	}
}
