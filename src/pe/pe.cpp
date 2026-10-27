#include "pe.hpp"

#include "../database/database.hpp"
#include "../win32/mapped_file.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace
{
	constexpr std::uint64_t MAX_PE_SIZE = 1024ULL * 1024ULL * 1024ULL;
	constexpr std::size_t MAX_IMPORT_NAME = 4096;

	template <typename value_t>
	[[nodiscard]] bool read_at(
		std::span<const std::byte> data,
		std::size_t offset,
		value_t& value ) noexcept
	{
		if ( offset > data.size() || sizeof( value_t ) > data.size() - offset )
		{
			return false;
		}

		std::memcpy( &value, data.data() + offset, sizeof( value_t ) );
		return true;
	}

	[[nodiscard]] bool checked_add(
		std::size_t left,
		std::size_t right,
		std::size_t& result ) noexcept
	{
		if ( right > std::numeric_limits<std::size_t>::max() - left )
		{
			return false;
		}

		result = left + right;
		return true;
	}

	[[nodiscard]] bool checked_multiply(
		std::size_t left,
		std::size_t right,
		std::size_t& result ) noexcept
	{
		if ( left != 0 && right > std::numeric_limits<std::size_t>::max() / left )
		{
			return false;
		}

		result = left * right;
		return true;
	}

	[[nodiscard]] std::wstring lower_extension( const std::filesystem::path& path )
	{
		std::wstring extension = path.extension().native();

		std::transform(
			extension.begin(),
			extension.end(),
			extension.begin(),
			[]( wchar_t character )
			{
				return static_cast<wchar_t>( std::towlower( character ) );
			} );

		return extension;
	}

	[[nodiscard]] bool candidate( const std::filesystem::path& path )
	{
		const auto extension = lower_extension( path );
		return extension == L".exe" || extension == L".dll" || extension == L".sys" ||
			extension == L".ocx" || extension == L".cpl" || extension == L".scr";
	}

	struct section_t final
	{
		std::uint32_t virtual_address = 0;
		std::uint32_t virtual_size = 0;
		std::uint32_t raw_offset = 0;
		std::uint32_t raw_size = 0;
	};

	[[nodiscard]] bool rva_to_offset(
		std::uint32_t rva,
		const std::vector<section_t>& sections,
		std::size_t file_size,
		std::size_t& result ) noexcept
	{
		for ( const auto& section : sections )
		{
			const std::uint64_t span = std::max(
				static_cast<std::uint64_t>( section.virtual_size ),
				static_cast<std::uint64_t>( section.raw_size ) );

			const std::uint64_t start = section.virtual_address;
			const std::uint64_t end = start + span;

			if ( rva < start || rva >= end )
			{
				continue;
			}

			const std::uint64_t offset =
				static_cast<std::uint64_t>( section.raw_offset ) +
				( static_cast<std::uint64_t>( rva ) - start );

			if ( offset >= file_size )
			{
				return false;
			}

			result = static_cast<std::size_t>( offset );
			return true;
		}

		return false;
	}

	[[nodiscard]] std::wstring ascii_string(
		std::span<const std::byte> data,
		std::size_t offset )
	{
		std::string value;

		for ( std::size_t index = 0; index < MAX_IMPORT_NAME; ++index )
		{
			std::size_t position = 0;

			if ( !checked_add( offset, index, position ) || position >= data.size() )
			{
				return {};
			}

			const unsigned char raw = std::to_integer<unsigned char>( data[position] );
			const char character = static_cast<char>( raw );

			if ( character == '\0' )
			{
				if ( value.empty() )
				{
					return {};
				}

				return std::wstring( value.begin(), value.end() );
			}

			if ( raw < 0x20U || raw > 0x7EU )
			{
				return {};
			}

			value.push_back( character );
		}

		return {};
	}

	void parse_import_directory(
		std::span<const std::byte> data,
		std::uint32_t directory_rva,
		const std::vector<section_t>& sections,
		std::int64_t image_id,
		const std::wstring& evidence,
		nylon::database::database_t& database )
	{
		if ( directory_rva == 0 )
		{
			return;
		}

		std::size_t offset = 0;

		if ( !rva_to_offset( directory_rva, sections, data.size(), offset ) )
		{
			return;
		}

		for ( std::size_t index = 0; index < 65536; ++index )
		{
			std::size_t descriptor_offset = 0;
			std::size_t descriptor_delta = 0;

			if ( !checked_multiply(
				index,
				sizeof( IMAGE_IMPORT_DESCRIPTOR ),
				descriptor_delta ) ||
				!checked_add( offset, descriptor_delta, descriptor_offset ) )
			{
				return;
			}

			IMAGE_IMPORT_DESCRIPTOR descriptor{};

			if ( !read_at( data, descriptor_offset, descriptor ) )
			{
				return;
			}

			if ( descriptor.Name == 0 &&
				descriptor.FirstThunk == 0 &&
				descriptor.OriginalFirstThunk == 0 )
			{
				return;
			}

			std::size_t name_offset = 0;

			if ( !rva_to_offset( descriptor.Name, sections, data.size(), name_offset ) )
			{
				continue;
			}

			const auto name = ascii_string( data, name_offset );

			if ( name.empty() )
			{
				continue;
			}

			const auto dependency_id = database.object( L"pe_module", name );
			database.edge( image_id, L"imports", dependency_id, evidence );
		}
	}

	void parse_delay_import_directory(
		std::span<const std::byte> data,
		std::uint32_t directory_rva,
		const std::vector<section_t>& sections,
		std::int64_t image_id,
		nylon::database::database_t& database )
	{
		if ( directory_rva == 0 )
		{
			return;
		}

		std::size_t offset = 0;

		if ( !rva_to_offset( directory_rva, sections, data.size(), offset ) )
		{
			return;
		}

		struct delay_descriptor_t final
		{
			std::uint32_t attributes;
			std::uint32_t name;
			std::uint32_t module_handle;
			std::uint32_t import_address_table;
			std::uint32_t import_name_table;
			std::uint32_t bound_import_address_table;
			std::uint32_t unload_import_address_table;
			std::uint32_t timestamp;
		};

		for ( std::size_t index = 0; index < 65536; ++index )
		{
			std::size_t descriptor_offset = 0;
			std::size_t descriptor_delta = 0;

			if ( !checked_multiply(
				index,
				sizeof( delay_descriptor_t ),
				descriptor_delta ) ||
				!checked_add( offset, descriptor_delta, descriptor_offset ) )
			{
				return;
			}

			delay_descriptor_t descriptor{};

			if ( !read_at( data, descriptor_offset, descriptor ) )
			{
				return;
			}

			if ( descriptor.name == 0 )
			{
				return;
			}

			if ( ( descriptor.attributes & 1U ) == 0 )
			{
				continue;
			}

			std::size_t name_offset = 0;

			if ( !rva_to_offset( descriptor.name, sections, data.size(), name_offset ) )
			{
				continue;
			}

			const auto name = ascii_string( data, name_offset );

			if ( name.empty() )
			{
				continue;
			}

			const auto dependency_id = database.object( L"pe_module", name );
			database.edge( image_id, L"delay_imports", dependency_id, L"PE delay import directory" );
		}
	}

	void parse_file(
		const std::filesystem::path& path,
		nylon::database::database_t& database )
	{
		std::error_code error;
		const std::uint64_t size = std::filesystem::file_size( path, error );

		if ( error || size < sizeof( IMAGE_DOS_HEADER ) || size > MAX_PE_SIZE )
		{
			return;
		}

		nylon::win32::mapped_file_t mapped( path );
		const std::span<const std::byte> data = mapped.bytes();

		IMAGE_DOS_HEADER dos{};

		if ( !read_at( data, 0, dos ) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 )
		{
			return;
		}

		const auto pe_offset = static_cast<std::size_t>( dos.e_lfanew );
		DWORD signature = 0;

		if ( !read_at( data, pe_offset, signature ) || signature != IMAGE_NT_SIGNATURE )
		{
			return;
		}

		std::size_t file_header_offset = 0;

		if ( !checked_add( pe_offset, sizeof( DWORD ), file_header_offset ) )
		{
			return;
		}

		IMAGE_FILE_HEADER file_header{};

		if ( !read_at( data, file_header_offset, file_header ) )
		{
			return;
		}

		std::size_t optional_offset = 0;

		if ( !checked_add( file_header_offset, sizeof( IMAGE_FILE_HEADER ), optional_offset ) )
		{
			return;
		}

		WORD magic = 0;

		if ( !read_at( data, optional_offset, magic ) )
		{
			return;
		}

		std::uint32_t import_rva = 0;
		std::uint32_t delay_rva = 0;

		if ( magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC )
		{
			IMAGE_OPTIONAL_HEADER64 optional{};

			if ( file_header.SizeOfOptionalHeader < sizeof( optional ) ||
				!read_at( data, optional_offset, optional ) )
			{
				return;
			}

			if ( optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT )
			{
				import_rva = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
			}

			if ( optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT )
			{
				delay_rva = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
			}
		}
		else if ( magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC )
		{
			IMAGE_OPTIONAL_HEADER32 optional{};

			if ( file_header.SizeOfOptionalHeader < sizeof( optional ) ||
				!read_at( data, optional_offset, optional ) )
			{
				return;
			}

			if ( optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT )
			{
				import_rva = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
			}

			if ( optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT )
			{
				delay_rva = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
			}
		}
		else
		{
			return;
		}

		std::size_t section_offset = 0;

		if ( !checked_add(
			optional_offset,
			file_header.SizeOfOptionalHeader,
			section_offset ) )
		{
			return;
		}

		if ( file_header.NumberOfSections > 512 )
		{
			return;
		}

		std::vector<section_t> sections;
		sections.reserve( file_header.NumberOfSections );

		for ( WORD index = 0; index < file_header.NumberOfSections; ++index )
		{
			std::size_t current_offset = 0;

			if ( !checked_add(
				section_offset,
				static_cast<std::size_t>( index ) * sizeof( IMAGE_SECTION_HEADER ),
				current_offset ) )
			{
				return;
			}

			IMAGE_SECTION_HEADER section{};

			if ( !read_at( data, current_offset, section ) )
			{
				return;
			}

			sections.push_back(
				{
					.virtual_address = section.VirtualAddress,
					.virtual_size = section.Misc.VirtualSize,
					.raw_offset = section.PointerToRawData,
					.raw_size = section.SizeOfRawData
				} );
		}

		const auto image_id = database.find_object_by_path( L"file", path.native() );

		if ( image_id == 0 )
		{
			return;
		}

		database.property( image_id, L"pe_machine", std::to_wstring( file_header.Machine ) );
		parse_import_directory(
			data,
			import_rva,
			sections,
			image_id,
			L"PE import directory",
			database );
		parse_delay_import_directory( data, delay_rva, sections, image_id, database );
	}
}

namespace nylon::pe
{
	void scan( const std::filesystem::path& image_root, database::database_t& database )
	{
		database.note( L"pe", L"info", L"scan started" );

		std::error_code error;

		for ( std::filesystem::recursive_directory_iterator iterator(
			image_root,
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

			if ( !iterator->is_regular_file( error ) || error || !candidate( iterator->path() ) )
			{
				error.clear();
				continue;
			}

			try
			{
				parse_file( iterator->path(), database );
			}
			catch ( const std::exception& )
			{
				database.note( L"pe", L"warning", L"PE parse failed: " + iterator->path().native() );
			}
		}

		database.note( L"pe", L"info", L"scan completed" );
	}
}
