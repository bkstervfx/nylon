#include "registry.hpp"

#include "../database/database.hpp"
#include "../win32/privilege.hpp"
#include "../win32/resource.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr std::size_t MAX_KEY_NAME = 32767;
	constexpr std::size_t MAX_VALUE_NAME = 32767;
	constexpr std::size_t MAX_VALUE_DATA = 64U * 1024U * 1024U;

	[[nodiscard]] bool starts_with_insensitive(
		std::wstring_view text,
		std::wstring_view prefix )
	{
		if ( text.size() < prefix.size() )
		{
			return false;
		}

		for ( std::size_t index = 0; index < prefix.size(); ++index )
		{
			if ( std::towlower( text[index] ) != std::towlower( prefix[index] ) )
			{
				return false;
			}
		}

		return true;
	}

	[[nodiscard]] std::filesystem::path copy_hive_family(
		const std::filesystem::path& source,
		const std::filesystem::path& temp_registry_root,
		const std::wstring& hive_name )
	{
		const auto destination_root = temp_registry_root / hive_name;
		std::error_code error;

		std::filesystem::remove_all( destination_root, error );

		if ( error )
		{
			throw std::runtime_error( "could not clear temporary registry directory" );
		}

		std::filesystem::create_directories( destination_root );

		const std::wstring prefix = source.filename().native();
		bool copied_primary = false;

		for ( const auto& entry : std::filesystem::directory_iterator(
			source.parent_path(),
			std::filesystem::directory_options::skip_permission_denied,
			error ) )
		{
			if ( error )
			{
				throw std::runtime_error( "could not enumerate registry hive companions" );
			}

			if ( !entry.is_regular_file( error ) )
			{
				if ( error )
				{
					throw std::runtime_error( "could not inspect registry hive companion" );
				}

				continue;
			}

			const std::wstring name = entry.path().filename().native();

			if ( !starts_with_insensitive( name, prefix ) )
			{
				continue;
			}

			const auto destination = destination_root / entry.path().filename();
			std::filesystem::copy_file(
				entry.path(),
				destination,
				std::filesystem::copy_options::overwrite_existing );

			if ( entry.path().filename() == source.filename() )
			{
				copied_primary = true;
			}
		}

		if ( !copied_primary )
		{
			throw std::runtime_error( "registry hive primary file was not copied" );
		}

		return destination_root / source.filename();
	}

	class mounted_hive_t final
	{
	public:
		explicit mounted_hive_t( const std::filesystem::path& path )
		{
			static std::atomic<std::uint32_t> sequence{ 0 };

			mount_name_ =
				L"NYLON_AUDIT_" +
				std::to_wstring( ::GetCurrentProcessId() ) +
				L"_" +
				std::to_wstring( sequence.fetch_add( 1, std::memory_order_relaxed ) );

			const LSTATUS load_status = ::RegLoadKeyW(
				HKEY_LOCAL_MACHINE,
				mount_name_.c_str(),
				path.c_str() );

			if ( load_status != ERROR_SUCCESS )
			{
				throw std::runtime_error(
					"RegLoadKeyW failed with error " +
					std::to_string( load_status ) );
			}

			HKEY raw_root = nullptr;
			const LSTATUS open_status = ::RegOpenKeyExW(
				HKEY_LOCAL_MACHINE,
				mount_name_.c_str(),
				0,
				KEY_READ,
				&raw_root );

			if ( open_status != ERROR_SUCCESS )
			{
				::RegUnLoadKeyW( HKEY_LOCAL_MACHINE, mount_name_.c_str() );

				throw std::runtime_error(
					"RegOpenKeyExW failed after RegLoadKeyW with error " +
					std::to_string( open_status ) );
			}

			root_.reset( raw_root );
		}

		mounted_hive_t( const mounted_hive_t& ) = delete;
		mounted_hive_t& operator=( const mounted_hive_t& ) = delete;

		~mounted_hive_t()
		{
			root_.reset();

			if ( !mount_name_.empty() )
			{
				::RegUnLoadKeyW( HKEY_LOCAL_MACHINE, mount_name_.c_str() );
			}
		}

		[[nodiscard]] HKEY get() const noexcept
		{
			return root_.get();
		}

	private:
		std::wstring mount_name_;
		nylon::win32::hkey_t root_;
	};

	void enumerate_key(
		HKEY key,
		const std::wstring& hive_name,
		const std::wstring& relative_path,
		nylon::database::database_t& database,
		std::int64_t parent_id,
		std::size_t depth )
	{
		const std::wstring full_name = relative_path.empty()
			? hive_name
			: hive_name + L"\\" + relative_path;

		if ( depth > 512U )
		{
			database.note( L"registry", L"warning", L"registry depth limit reached: " + full_name );
			return;
		}

		const auto key_id = database.object( L"registry_key", full_name, full_name );

		if ( parent_id != 0 )
		{
			database.edge( parent_id, L"contains", key_id, L"registry" );
		}

		DWORD subkey_count = 0;
		DWORD max_subkey_length = 0;
		DWORD value_count = 0;
		DWORD max_value_name_length = 0;
		DWORD max_value_data_length = 0;

		const LSTATUS info_status = ::RegQueryInfoKeyW(
			key,
			nullptr,
			nullptr,
			nullptr,
			&subkey_count,
			&max_subkey_length,
			nullptr,
			&value_count,
			&max_value_name_length,
			&max_value_data_length,
			nullptr,
			nullptr );

		if ( info_status != ERROR_SUCCESS )
		{
			database.note( L"registry", L"warning", L"RegQueryInfoKeyW failed: " + full_name );
			return;
		}

		if ( max_value_data_length > MAX_VALUE_DATA )
		{
			database.note( L"registry", L"warning", L"registry value exceeds audit bound: " + full_name );
			max_value_data_length = static_cast<DWORD>( MAX_VALUE_DATA );
		}

		std::vector<wchar_t> value_name(
			std::min<std::size_t>(
				static_cast<std::size_t>( max_value_name_length ) + 2U,
				MAX_VALUE_NAME + 1U ) );

		std::vector<std::byte> value_data(
			std::max<std::size_t>(
				static_cast<std::size_t>( max_value_data_length ),
				1U ) );

		for ( DWORD index = 0; index < value_count; ++index )
		{
			DWORD name_length = static_cast<DWORD>( value_name.size() );
			DWORD data_length = static_cast<DWORD>( value_data.size() );
			DWORD type = 0;

			const LSTATUS value_status = ::RegEnumValueW(
				key,
				index,
				value_name.data(),
				&name_length,
				nullptr,
				&type,
				reinterpret_cast<BYTE*>( value_data.data() ),
				&data_length );

			if ( value_status == ERROR_MORE_DATA )
			{
				database.note(
					L"registry",
					L"warning",
					L"registry value skipped because it exceeded audit bound: " + full_name );
				continue;
			}

			if ( value_status != ERROR_SUCCESS )
			{
				database.note( L"registry", L"warning", L"RegEnumValueW failed: " + full_name );
				continue;
			}

			const std::wstring_view name( value_name.data(), name_length );
			const std::span<const std::byte> data( value_data.data(), data_length );
			database.registry_value( key_id, name, type, data );
		}

		if ( max_subkey_length >= MAX_KEY_NAME )
		{
			database.note( L"registry", L"warning", L"registry key name exceeds audit bound: " + full_name );
			return;
		}

		std::vector<wchar_t> subkey_name( static_cast<std::size_t>( max_subkey_length ) + 2U );

		for ( DWORD index = 0; index < subkey_count; ++index )
		{
			DWORD name_length = static_cast<DWORD>( subkey_name.size() );

			const LSTATUS enum_status = ::RegEnumKeyExW(
				key,
				index,
				subkey_name.data(),
				&name_length,
				nullptr,
				nullptr,
				nullptr,
				nullptr );

			if ( enum_status != ERROR_SUCCESS )
			{
				database.note( L"registry", L"warning", L"RegEnumKeyExW failed: " + full_name );
				continue;
			}

			const std::wstring child_name( subkey_name.data(), name_length );
			HKEY raw_child = nullptr;

			const LSTATUS open_status = ::RegOpenKeyExW(
				key,
				child_name.c_str(),
				0,
				KEY_READ,
				&raw_child );

			if ( open_status != ERROR_SUCCESS )
			{
				database.note(
					L"registry",
					L"warning",
					L"RegOpenKeyExW failed: " + full_name + L"\\" + child_name );
				continue;
			}

			nylon::win32::hkey_t child( raw_child );
			const std::wstring child_path = relative_path.empty()
				? child_name
				: relative_path + L"\\" + child_name;

			enumerate_key( child.get(), hive_name, child_path, database, key_id, depth + 1U );
		}
	}

	void scan_hive(
		const std::filesystem::path& path,
		const std::filesystem::path& temp_registry_root,
		const std::wstring& hive_name,
		nylon::database::database_t& database )
	{
		if ( !std::filesystem::is_regular_file( path ) )
		{
			return;
		}

		try
		{
			const auto copy = copy_hive_family( path, temp_registry_root, hive_name );
			mounted_hive_t hive( copy );
			enumerate_key( hive.get(), hive_name, L"", database, 0, 0 );
		}
		catch ( const std::exception& error )
		{
			std::wstring message = L"hive load failed for " + hive_name + L": ";
			const std::string narrow = error.what();
			message.append( narrow.begin(), narrow.end() );
			database.note( L"registry", L"warning", message );
		}
	}
}

namespace nylon::registry
{
	void scan(
		const std::filesystem::path& image_root,
		const std::filesystem::path& temp_root,
		database::database_t& database )
	{
		database.note( L"registry", L"info", L"scan started" );

		nylon::win32::enable_privilege( SE_BACKUP_NAME );
		nylon::win32::enable_privilege( SE_RESTORE_NAME );

		const auto temp_registry_root = temp_root / L"registry";
		std::error_code temp_error;
		std::filesystem::remove_all( temp_registry_root, temp_error );

		if ( temp_error )
		{
			throw std::runtime_error( "could not clear registry temporary root" );
		}

		std::filesystem::create_directories( temp_registry_root );

		const auto config = image_root / L"Windows" / L"System32" / L"config";

		const std::pair<const wchar_t*, const wchar_t*> hives[] =
		{
			{ L"SOFTWARE", L"SOFTWARE" },
			{ L"SYSTEM", L"SYSTEM" },
			{ L"COMPONENTS", L"COMPONENTS" },
			{ L"DEFAULT", L"DEFAULT" },
			{ L"SAM", L"SAM" },
			{ L"SECURITY", L"SECURITY" }
		};

		for ( const auto& [file_name, hive_name] : hives )
		{
			scan_hive( config / file_name, temp_registry_root, hive_name, database );
		}

		scan_hive(
			image_root / L"Users" / L"Default" / L"NTUSER.DAT",
			temp_registry_root,
			L"DEFAULT_USER",
			database );

		scan_hive(
			image_root / L"Users" / L"Default" / L"AppData" / L"Local" /
				L"Microsoft" / L"Windows" / L"UsrClass.dat",
			temp_registry_root,
			L"DEFAULT_USER_CLASSES",
			database );

		scan_hive( image_root / L"Boot" / L"BCD", temp_registry_root, L"BCD_BOOT", database );
		scan_hive(
			image_root / L"EFI" / L"Microsoft" / L"Boot" / L"BCD",
			temp_registry_root,
			L"BCD_EFI",
			database );

		std::filesystem::remove_all( temp_registry_root, temp_error );

		if ( temp_error )
		{
			database.note( L"registry", L"warning", L"temporary hive copies could not be fully removed" );
		}

		database.note( L"registry", L"info", L"scan completed" );
	}
}
