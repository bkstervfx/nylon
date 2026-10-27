#include "filesystem.hpp"

#include "../database/database.hpp"
#include "../win32/resource.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace
{
	[[nodiscard]] nylon::win32::handle_t open_for_identity( const std::filesystem::path& path, bool directory )
	{
		const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
			( directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL );

		return nylon::win32::handle_t( ::CreateFileW(
			path.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			flags,
			nullptr ) );
	}

	[[nodiscard]] std::uint64_t file_size( const WIN32_FILE_ATTRIBUTE_DATA& data ) noexcept
	{
		return ( static_cast<std::uint64_t>( data.nFileSizeHigh ) << 32U ) |
			static_cast<std::uint64_t>( data.nFileSizeLow );
	}
}

namespace nylon::filesystem
{
	void scan( const std::filesystem::path& image_root, database::database_t& database )
	{
		database.note( L"filesystem", L"info", L"scan started" );

		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(
			image_root,
			std::filesystem::directory_options::skip_permission_denied,
			error );

		const std::filesystem::recursive_directory_iterator end;

		for ( ; iterator != end; iterator.increment( error ) )
		{
			if ( error )
			{
				database.note(
					L"filesystem",
					L"warning",
					L"directory enumeration error: " + std::to_wstring( error.value() ) );
				error.clear();
				continue;
			}

			const auto path = iterator->path();

			WIN32_FILE_ATTRIBUTE_DATA attributes{};

			if ( ::GetFileAttributesExW( path.c_str(), GetFileExInfoStandard, &attributes ) == FALSE )
			{
				database.note( L"filesystem", L"warning", L"GetFileAttributesExW failed: " + path.native() );
				continue;
			}

			const bool is_directory = ( attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
			const auto type = is_directory ? L"directory" : L"file";
			const auto object_id = database.object( type, path.filename().native(), path.native() );

			auto handle = open_for_identity( path, is_directory );

			if ( !handle )
			{
				database.note( L"filesystem", L"warning", L"CreateFileW failed: " + path.native() );
				continue;
			}

			FILE_ID_INFO id_info{};
			BY_HANDLE_FILE_INFORMATION basic_info{};

			if ( ::GetFileInformationByHandleEx(
				handle.get(),
				FileIdInfo,
				&id_info,
				sizeof( id_info ) ) == FALSE )
			{
				database.note( L"filesystem", L"warning", L"FileIdInfo unavailable: " + path.native() );
				continue;
			}

			if ( ::GetFileInformationByHandle( handle.get(), &basic_info ) == FALSE )
			{
				database.note( L"filesystem", L"warning", L"GetFileInformationByHandle failed: " + path.native() );
				continue;
			}

			if ( !is_directory )
			{
				std::array<std::byte, 16> file_id{};
				static_assert( sizeof( id_info.FileId.Identifier ) == file_id.size() );
				std::memcpy( file_id.data(), id_info.FileId.Identifier, file_id.size() );

				database.file(
					object_id,
					file_size( attributes ),
					attributes.dwFileAttributes,
					id_info.VolumeSerialNumber,
					std::span<const std::byte, 16>( file_id ),
					basic_info.nNumberOfLinks );
			}
			else
			{
				database.property( object_id, L"attributes", std::to_wstring( attributes.dwFileAttributes ) );
			}
		}

		database.materialize_file_identities();
		database.note( L"filesystem", L"info", L"scan completed" );
	}
}
