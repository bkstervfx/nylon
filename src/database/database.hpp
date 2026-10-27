#ifndef NYLON_DATABASE_DATABASE_HPP
#define NYLON_DATABASE_DATABASE_HPP

#include <winsqlite3.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nylon::database
{
	struct object_ref_t final
	{
		std::int64_t id = 0;
		std::wstring name;
		std::wstring path;
	};

	struct registry_value_t final
	{
		std::uint32_t type = 0;
		std::vector<std::byte> data;
	};

	class database_t final
	{
	public:
		explicit database_t( const std::filesystem::path& path );

		database_t( const database_t& ) = delete;
		database_t& operator=( const database_t& ) = delete;

		~database_t();

		void begin();
		void commit();
		void rollback() noexcept;

		[[nodiscard]] std::int64_t object(
			std::wstring_view type,
			std::wstring_view name,
			std::wstring_view path = {} );

		void property( std::int64_t object_id, std::wstring_view key, std::wstring_view value );
		void edge(
			std::int64_t from_id,
			std::wstring_view relation,
			std::int64_t to_id,
			std::wstring_view evidence );

		void file(
			std::int64_t object_id,
			std::uint64_t size,
			std::uint32_t attributes,
			std::uint64_t volume_serial,
			std::span<const std::byte, 16> file_id,
			std::uint32_t link_count );

		void registry_value(
			std::int64_t key_id,
			std::wstring_view name,
			std::uint32_t type,
			std::span<const std::byte> data );

		void materialize_file_identities();

		void link_path_prefix(
			std::int64_t owner_id,
			std::wstring_view relation,
			std::wstring_view path_prefix,
			std::wstring_view evidence );

		[[nodiscard]] std::int64_t find_object_by_path(
			std::wstring_view type,
			std::wstring_view path ) const;

		[[nodiscard]] std::vector<object_ref_t> objects_with_path_prefix(
			std::wstring_view type,
			std::wstring_view path_prefix ) const;

		[[nodiscard]] std::optional<registry_value_t> registry_value(
			std::int64_t key_id,
			std::wstring_view name ) const;

		void note( std::wstring_view collector, std::wstring_view level, std::wstring_view message );

	private:
		void exec( const char* sql );
		[[nodiscard]] sqlite3_stmt* prepare( const char* sql ) const;
		static void bind_text( sqlite3_stmt* statement, int index, std::wstring_view value );
		[[nodiscard]] static std::wstring column_text( sqlite3_stmt* statement, int index );
		static void require_ok( int code, sqlite3* db, const char* operation );

		sqlite3* db_ = nullptr;
		bool transaction_open_ = false;
	};
}

#endif
