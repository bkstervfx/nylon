#include "database.hpp"

#include <bit>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
	class statement_t final
	{
	public:
		explicit statement_t( sqlite3_stmt* value ) noexcept
			: value_( value )
		{
		}

		statement_t( const statement_t& ) = delete;
		statement_t& operator=( const statement_t& ) = delete;

		~statement_t()
		{
			if ( value_ != nullptr )
			{
				sqlite3_finalize( value_ );
			}
		}

		[[nodiscard]] sqlite3_stmt* get() const noexcept
		{
			return value_;
		}

	private:
		sqlite3_stmt* value_ = nullptr;
	};
}

namespace nylon::database
{
	database_t::database_t( const std::filesystem::path& path )
	{
		if ( sqlite3_open16( path.c_str(), &db_ ) != SQLITE_OK )
		{
			const std::string message = db_ != nullptr ? sqlite3_errmsg( db_ ) : "unknown SQLite error";

			if ( db_ != nullptr )
			{
				sqlite3_close( db_ );
				db_ = nullptr;
			}

			throw std::runtime_error( "sqlite3_open16 failed: " + message );
		}

		exec( "PRAGMA journal_mode=OFF;" );
		exec( "PRAGMA synchronous=OFF;" );
		exec( "PRAGMA temp_store=MEMORY;" );
		exec( "PRAGMA foreign_keys=ON;" );

		exec(
			"CREATE TABLE IF NOT EXISTS objects("
			"id INTEGER PRIMARY KEY,"
			"type TEXT NOT NULL,"
			"name TEXT NOT NULL,"
			"path TEXT NOT NULL DEFAULT '',"
			"UNIQUE(type,name,path));" );

		exec(
			"CREATE INDEX IF NOT EXISTS objects_path_idx "
			"ON objects(type,path);" );

		exec(
			"CREATE TABLE IF NOT EXISTS properties("
			"object_id INTEGER NOT NULL,"
			"key TEXT NOT NULL,"
			"value TEXT NOT NULL,"
			"UNIQUE(object_id,key,value),"
			"FOREIGN KEY(object_id) REFERENCES objects(id));" );

		exec(
			"CREATE TABLE IF NOT EXISTS edges("
			"from_id INTEGER NOT NULL,"
			"relation TEXT NOT NULL,"
			"to_id INTEGER NOT NULL,"
			"evidence TEXT NOT NULL,"
			"UNIQUE(from_id,relation,to_id,evidence),"
			"FOREIGN KEY(from_id) REFERENCES objects(id),"
			"FOREIGN KEY(to_id) REFERENCES objects(id));" );

		exec(
			"CREATE INDEX IF NOT EXISTS edges_to_idx "
			"ON edges(to_id,relation);" );

		exec(
			"CREATE TABLE IF NOT EXISTS files("
			"object_id INTEGER PRIMARY KEY,"
			"size INTEGER NOT NULL,"
			"attributes INTEGER NOT NULL,"
			"volume_serial INTEGER NOT NULL,"
			"file_id BLOB NOT NULL,"
			"link_count INTEGER NOT NULL,"
			"FOREIGN KEY(object_id) REFERENCES objects(id));" );

		exec(
			"CREATE INDEX IF NOT EXISTS files_identity_idx "
			"ON files(volume_serial,file_id);" );

		exec(
			"CREATE TABLE IF NOT EXISTS registry_values("
			"key_id INTEGER NOT NULL,"
			"name TEXT NOT NULL,"
			"type INTEGER NOT NULL,"
			"data BLOB NOT NULL,"
			"UNIQUE(key_id,name),"
			"FOREIGN KEY(key_id) REFERENCES objects(id));" );

		exec(
			"CREATE TABLE IF NOT EXISTS notes("
			"collector TEXT NOT NULL,"
			"level TEXT NOT NULL,"
			"message TEXT NOT NULL);" );

		exec(
			"CREATE VIEW IF NOT EXISTS object_counts AS "
			"SELECT type,COUNT(*) AS count FROM objects GROUP BY type;" );

		exec(
			"CREATE VIEW IF NOT EXISTS hardlink_groups AS "
			"SELECT volume_serial,hex(file_id) AS file_id,COUNT(*) AS paths "
			"FROM files GROUP BY volume_serial,file_id HAVING COUNT(*)>1;" );

		exec(
			"CREATE VIEW IF NOT EXISTS dependency_edges AS "
			"SELECT f.type AS from_type,f.name AS from_name,e.relation,"
			"t.type AS to_type,t.name AS to_name,e.evidence "
			"FROM edges e "
			"JOIN objects f ON f.id=e.from_id "
			"JOIN objects t ON t.id=e.to_id;" );
	}

	database_t::~database_t()
	{
		if ( transaction_open_ )
		{
			rollback();
		}

		if ( db_ != nullptr )
		{
			sqlite3_close( db_ );
		}
	}

	void database_t::begin()
	{
		if ( transaction_open_ )
		{
			throw std::logic_error( "database transaction already open" );
		}

		exec( "BEGIN IMMEDIATE;" );
		transaction_open_ = true;
	}

	void database_t::commit()
	{
		if ( !transaction_open_ )
		{
			throw std::logic_error( "database transaction is not open" );
		}

		exec( "COMMIT;" );
		transaction_open_ = false;
	}

	void database_t::rollback() noexcept
	{
		if ( !transaction_open_ )
		{
			return;
		}

		char* error_message = nullptr;
		sqlite3_exec( db_, "ROLLBACK;", nullptr, nullptr, &error_message );

		if ( error_message != nullptr )
		{
			sqlite3_free( error_message );
		}

		transaction_open_ = false;
	}

	std::int64_t database_t::object(
		std::wstring_view type,
		std::wstring_view name,
		std::wstring_view path )
	{
		{
			statement_t statement( prepare(
				"INSERT OR IGNORE INTO objects(type,name,path) VALUES(?,?,?);" ) );

			bind_text( statement.get(), 1, type );
			bind_text( statement.get(), 2, name );
			bind_text( statement.get(), 3, path );

			require_ok( sqlite3_step( statement.get() ), db_, "insert object" );
		}

		statement_t statement( prepare(
			"SELECT id FROM objects WHERE type=? AND name=? AND path=?;" ) );

		bind_text( statement.get(), 1, type );
		bind_text( statement.get(), 2, name );
		bind_text( statement.get(), 3, path );

		const int result = sqlite3_step( statement.get() );

		if ( result != SQLITE_ROW )
		{
			require_ok( result, db_, "select object" );
			throw std::runtime_error( "object insert/select invariant failed" );
		}

		return sqlite3_column_int64( statement.get(), 0 );
	}

	void database_t::property(
		std::int64_t object_id,
		std::wstring_view key,
		std::wstring_view value )
	{
		statement_t statement( prepare(
			"INSERT OR IGNORE INTO properties(object_id,key,value) VALUES(?,?,?);" ) );

		require_ok( sqlite3_bind_int64( statement.get(), 1, object_id ), db_, "bind property object" );
		bind_text( statement.get(), 2, key );
		bind_text( statement.get(), 3, value );

		require_ok( sqlite3_step( statement.get() ), db_, "insert property" );
	}

	void database_t::edge(
		std::int64_t from_id,
		std::wstring_view relation,
		std::int64_t to_id,
		std::wstring_view evidence )
	{
		statement_t statement( prepare(
			"INSERT OR IGNORE INTO edges(from_id,relation,to_id,evidence) VALUES(?,?,?,?);" ) );

		require_ok( sqlite3_bind_int64( statement.get(), 1, from_id ), db_, "bind edge source" );
		bind_text( statement.get(), 2, relation );
		require_ok( sqlite3_bind_int64( statement.get(), 3, to_id ), db_, "bind edge target" );
		bind_text( statement.get(), 4, evidence );

		require_ok( sqlite3_step( statement.get() ), db_, "insert edge" );
	}

	void database_t::file(
		std::int64_t object_id,
		std::uint64_t size,
		std::uint32_t attributes,
		std::uint64_t volume_serial,
		std::span<const std::byte, 16> file_id,
		std::uint32_t link_count )
	{
		if ( size > static_cast<std::uint64_t>( std::numeric_limits<sqlite3_int64>::max() ) )
		{
			throw std::overflow_error( "file size exceeds SQLite signed integer range" );
		}

		const auto stored_volume_serial = std::bit_cast<sqlite3_int64>( volume_serial );

		statement_t statement( prepare(
			"INSERT OR REPLACE INTO files("
			"object_id,size,attributes,volume_serial,file_id,link_count)"
			"VALUES(?,?,?,?,?,?);" ) );

		require_ok( sqlite3_bind_int64( statement.get(), 1, object_id ), db_, "bind file object" );
		require_ok( sqlite3_bind_int64( statement.get(), 2, static_cast<sqlite3_int64>( size ) ), db_, "bind file size" );
		require_ok( sqlite3_bind_int64( statement.get(), 3, attributes ), db_, "bind file attributes" );
		require_ok( sqlite3_bind_int64( statement.get(), 4, stored_volume_serial ), db_, "bind file volume" );
		require_ok(
			sqlite3_bind_blob(
				statement.get(),
				5,
				file_id.data(),
				static_cast<int>( file_id.size_bytes() ),
				SQLITE_TRANSIENT ),
			db_,
			"bind file id" );
		require_ok( sqlite3_bind_int64( statement.get(), 6, link_count ), db_, "bind file link count" );

		require_ok( sqlite3_step( statement.get() ), db_, "insert file" );
	}

	void database_t::registry_value(
		std::int64_t key_id,
		std::wstring_view name,
		std::uint32_t type,
		std::span<const std::byte> data )
	{
		if ( data.size_bytes() > static_cast<std::size_t>( INT_MAX ) )
		{
			throw std::overflow_error( "registry value is too large for SQLite binding" );
		}

		statement_t statement( prepare(
			"INSERT OR REPLACE INTO registry_values(key_id,name,type,data) VALUES(?,?,?,?);" ) );

		require_ok( sqlite3_bind_int64( statement.get(), 1, key_id ), db_, "bind registry key" );
		bind_text( statement.get(), 2, name );
		require_ok( sqlite3_bind_int64( statement.get(), 3, type ), db_, "bind registry type" );
		require_ok(
			sqlite3_bind_blob(
				statement.get(),
				4,
				data.empty() ? nullptr : data.data(),
				static_cast<int>( data.size_bytes() ),
				SQLITE_TRANSIENT ),
			db_,
			"bind registry data" );

		require_ok( sqlite3_step( statement.get() ), db_, "insert registry value" );
	}

	void database_t::materialize_file_identities()
	{
		exec(
			"INSERT OR IGNORE INTO objects(type,name,path) "
			"SELECT 'file_identity', "
			"printf('%lld:%s',volume_serial,hex(file_id)), '' "
			"FROM files GROUP BY volume_serial,file_id;" );

		exec(
			"INSERT OR IGNORE INTO edges(from_id,relation,to_id,evidence) "
			"SELECT f.object_id,'physical_identity',o.id,'FILE_ID_INFO' "
			"FROM files f "
			"JOIN objects o "
			"ON o.type='file_identity' "
			"AND o.path='' "
			"AND o.name=printf('%lld:%s',f.volume_serial,hex(f.file_id));" );
	}

	void database_t::link_path_prefix(
		std::int64_t owner_id,
		std::wstring_view relation,
		std::wstring_view path_prefix,
		std::wstring_view evidence )
	{
		statement_t statement( prepare(
			"INSERT OR IGNORE INTO edges(from_id,relation,to_id,evidence) "
			"SELECT ?,?,id,? FROM objects "
			"WHERE type='file' AND substr(path,1,length(?))=?;" ) );

		require_ok( sqlite3_bind_int64( statement.get(), 1, owner_id ), db_, "bind prefix owner" );
		bind_text( statement.get(), 2, relation );
		bind_text( statement.get(), 3, evidence );
		bind_text( statement.get(), 4, path_prefix );
		bind_text( statement.get(), 5, path_prefix );

		require_ok( sqlite3_step( statement.get() ), db_, "insert prefix edges" );
	}

	std::int64_t database_t::find_object_by_path(
		std::wstring_view type,
		std::wstring_view path ) const
	{
		statement_t statement( prepare(
			"SELECT id FROM objects WHERE type=? AND path=? ORDER BY id LIMIT 1;" ) );

		bind_text( statement.get(), 1, type );
		bind_text( statement.get(), 2, path );

		const int result = sqlite3_step( statement.get() );

		if ( result == SQLITE_DONE )
		{
			return 0;
		}

		require_ok( result, db_, "find object by path" );
		return sqlite3_column_int64( statement.get(), 0 );
	}

	std::vector<object_ref_t> database_t::objects_with_path_prefix(
		std::wstring_view type,
		std::wstring_view path_prefix ) const
	{
		statement_t statement( prepare(
			"SELECT id,name,path FROM objects "
			"WHERE type=? AND substr(path,1,length(?))=? ORDER BY path;" ) );

		bind_text( statement.get(), 1, type );
		bind_text( statement.get(), 2, path_prefix );
		bind_text( statement.get(), 3, path_prefix );

		std::vector<object_ref_t> result;

		for ( ;; )
		{
			const int step = sqlite3_step( statement.get() );

			if ( step == SQLITE_DONE )
			{
				break;
			}

			require_ok( step, db_, "enumerate objects by path prefix" );

			result.push_back(
				{
					.id = sqlite3_column_int64( statement.get(), 0 ),
					.name = column_text( statement.get(), 1 ),
					.path = column_text( statement.get(), 2 )
				} );
		}

		return result;
	}

	std::optional<registry_value_t> database_t::registry_value(
		std::int64_t key_id,
		std::wstring_view name ) const
	{
		statement_t statement( prepare(
			"SELECT type,data FROM registry_values WHERE key_id=? AND name=? LIMIT 1;" ) );

		require_ok( sqlite3_bind_int64( statement.get(), 1, key_id ), db_, "bind registry query key" );
		bind_text( statement.get(), 2, name );

		const int step = sqlite3_step( statement.get() );

		if ( step == SQLITE_DONE )
		{
			return std::nullopt;
		}

		require_ok( step, db_, "query registry value" );

		const int bytes = sqlite3_column_bytes( statement.get(), 1 );

		if ( bytes < 0 )
		{
			throw std::runtime_error( "SQLite returned a negative blob size" );
		}

		registry_value_t result;
		result.type = static_cast<std::uint32_t>( sqlite3_column_int64( statement.get(), 0 ) );
		result.data.resize( static_cast<std::size_t>( bytes ) );

		if ( bytes != 0 )
		{
			const void* source = sqlite3_column_blob( statement.get(), 1 );

			if ( source == nullptr )
			{
				throw std::runtime_error( "SQLite returned a null registry blob" );
			}

			std::memcpy( result.data.data(), source, result.data.size() );
		}

		return result;
	}

	void database_t::note(
		std::wstring_view collector,
		std::wstring_view level,
		std::wstring_view message )
	{
		statement_t statement( prepare(
			"INSERT INTO notes(collector,level,message) VALUES(?,?,?);" ) );

		bind_text( statement.get(), 1, collector );
		bind_text( statement.get(), 2, level );
		bind_text( statement.get(), 3, message );

		require_ok( sqlite3_step( statement.get() ), db_, "insert note" );
	}

	void database_t::exec( const char* sql )
	{
		char* error_message = nullptr;
		const int result = sqlite3_exec( db_, sql, nullptr, nullptr, &error_message );

		if ( result != SQLITE_OK )
		{
			const std::string message = error_message != nullptr ? error_message : "unknown SQLite error";

			if ( error_message != nullptr )
			{
				sqlite3_free( error_message );
			}

			throw std::runtime_error( "SQLite exec failed: " + message );
		}
	}

	sqlite3_stmt* database_t::prepare( const char* sql ) const
	{
		sqlite3_stmt* statement = nullptr;
		const int result = sqlite3_prepare_v2( db_, sql, -1, &statement, nullptr );

		require_ok( result, db_, "prepare statement" );
		return statement;
	}

	void database_t::bind_text(
		sqlite3_stmt* statement,
		int index,
		std::wstring_view value )
	{
		if ( value.size() > static_cast<std::size_t>( INT_MAX / sizeof( wchar_t ) ) )
		{
			throw std::overflow_error( "text is too large for SQLite binding" );
		}

		const int bytes = static_cast<int>( value.size() * sizeof( wchar_t ) );
		const int result = sqlite3_bind_text16(
			statement,
			index,
			value.empty() ? L"" : value.data(),
			bytes,
			SQLITE_TRANSIENT );

		if ( result != SQLITE_OK )
		{
			throw std::runtime_error( "sqlite3_bind_text16 failed" );
		}
	}

	std::wstring database_t::column_text( sqlite3_stmt* statement, int index )
	{
		const void* raw = sqlite3_column_text16( statement, index );

		if ( raw == nullptr )
		{
			return {};
		}

		const int bytes = sqlite3_column_bytes16( statement, index );

		if ( bytes < 0 || ( bytes % static_cast<int>( sizeof( wchar_t ) ) ) != 0 )
		{
			throw std::runtime_error( "SQLite returned invalid UTF-16 text" );
		}

		const auto* text = static_cast<const wchar_t*>( raw );
		return std::wstring( text, static_cast<std::size_t>( bytes ) / sizeof( wchar_t ) );
	}

	void database_t::require_ok(
		int code,
		sqlite3* db,
		const char* operation )
	{
		if ( code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW )
		{
			return;
		}

		throw std::runtime_error(
			std::string( operation ) + " failed: " +
			( db != nullptr ? sqlite3_errmsg( db ) : "unknown SQLite error" ) );
	}
}
