#include "delta.hpp"

#include "resource.hpp"

#include <Windows.h>

#if __has_include(<msdelta.h>)
#include <msdelta.h>
#define NYLON_HAS_MSDELTA 1
#else
#define NYLON_HAS_MSDELTA 0
#endif

#include <bit>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace
{
#if NYLON_HAS_MSDELTA
	using apply_delta_b_fn_t = decltype( &::ApplyDeltaB );
	using delta_free_fn_t = decltype( &::DeltaFree );

	class delta_output_t final
	{
	public:
		delta_output_t( DELTA_OUTPUT value, delta_free_fn_t free_function ) noexcept
			: value_( value ),
			  free_( free_function )
		{
		}

		delta_output_t( const delta_output_t& ) = delete;
		delta_output_t& operator=( const delta_output_t& ) = delete;

		~delta_output_t()
		{
			if ( value_.lpStart != nullptr && free_ != nullptr )
			{
				free_( value_.lpStart );
			}
		}

		[[nodiscard]] const DELTA_OUTPUT& get() const noexcept
		{
			return value_;
		}

	private:
		DELTA_OUTPUT value_{};
		delta_free_fn_t free_ = nullptr;
	};

	[[nodiscard]] std::vector<std::byte> load_dictionary(
		const std::filesystem::path& offline_wcp )
	{
		nylon::win32::module_t module( ::LoadLibraryExW(
			offline_wcp.c_str(),
			nullptr,
			LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE ) );

		if ( !module )
		{
			throw std::runtime_error( "LoadLibraryExW failed for offline wcp.dll data image" );
		}

		HRSRC resource = ::FindResourceW(
			module.get(),
			MAKEINTRESOURCEW( 1 ),
			MAKEINTRESOURCEW( 614 ) );

		if ( resource == nullptr )
		{
			throw std::runtime_error( "wcp.dll resource 614/1 was not found" );
		}

		HGLOBAL loaded = ::LoadResource( module.get(), resource );

		if ( loaded == nullptr )
		{
			throw std::runtime_error( "LoadResource failed for wcp.dll dictionary" );
		}

		const DWORD size = ::SizeofResource( module.get(), resource );
		const void* bytes = ::LockResource( loaded );

		if ( size == 0 || bytes == nullptr )
		{
			throw std::runtime_error( "wcp.dll dictionary resource is empty" );
		}

		std::vector<std::byte> result( size );
		std::memcpy( result.data(), bytes, result.size() );
		return result;
	}

	[[nodiscard]] nylon::win32::module_t load_delta_engine()
	{
		nylon::win32::module_t module( ::LoadLibraryExW(
			L"UpdateCompression.dll",
			nullptr,
			LOAD_LIBRARY_SEARCH_SYSTEM32 ) );

		if ( module )
		{
			return module;
		}

		module.reset( ::LoadLibraryExW(
			L"msdelta.dll",
			nullptr,
			LOAD_LIBRARY_SEARCH_SYSTEM32 ) );
		return module;
	}
#endif
}

namespace nylon::win32
{
	std::optional<std::vector<std::byte>> decode_dcm_manifest(
		const std::filesystem::path& offline_wcp,
		std::span<const std::byte> manifest )
	{
#if !NYLON_HAS_MSDELTA
		(void)offline_wcp;
		(void)manifest;
		return std::nullopt;
#else
		constexpr std::byte d = static_cast<std::byte>( 'D' );
		constexpr std::byte c = static_cast<std::byte>( 'C' );
		constexpr std::byte m = static_cast<std::byte>( 'M' );
		constexpr std::byte version = static_cast<std::byte>( 1 );

		const bool supported_magic =
			manifest.size() >= 4U &&
			manifest[0] == d &&
			manifest[1] == c &&
			( manifest[2] == m ||
				manifest[2] == static_cast<std::byte>( 'N' ) ||
				manifest[2] == static_cast<std::byte>( 'S' ) ) &&
			manifest[3] == version;

		if ( !supported_magic )
		{
			return std::nullopt;
		}

		const auto dictionary = load_dictionary( offline_wcp );
		auto engine = load_delta_engine();

		if ( !engine )
		{
			throw std::runtime_error( "neither UpdateCompression.dll nor msdelta.dll could be loaded" );
		}

		const auto apply_delta = std::bit_cast<apply_delta_b_fn_t>(
			::GetProcAddress( engine.get(), "ApplyDeltaB" ) );
		const auto delta_free = std::bit_cast<delta_free_fn_t>(
			::GetProcAddress( engine.get(), "DeltaFree" ) );

		if ( apply_delta == nullptr || delta_free == nullptr )
		{
			throw std::runtime_error( "delta engine does not export ApplyDeltaB and DeltaFree" );
		}

		DELTA_INPUT source{};
		source.lpcStart = dictionary.data();
		source.uSize = dictionary.size();
		source.Editable = FALSE;

		const auto delta_bytes = manifest.subspan( 4U );

		DELTA_INPUT delta{};
		delta.lpcStart = delta_bytes.data();
		delta.uSize = delta_bytes.size();
		delta.Editable = FALSE;

		DELTA_OUTPUT raw_output{};

		if ( apply_delta(
			DELTA_FLAG_NONE,
			source,
			delta,
			&raw_output ) == FALSE )
		{
			throw std::runtime_error( "ApplyDeltaB failed while decoding a DCM manifest" );
		}

		delta_output_t output( raw_output, delta_free );

		if ( output.get().lpStart == nullptr || output.get().uSize == 0 )
		{
			throw std::runtime_error( "ApplyDeltaB returned an empty manifest" );
		}

		std::vector<std::byte> result( static_cast<std::size_t>( output.get().uSize ) );
		std::memcpy( result.data(), output.get().lpStart, result.size() );
		return result;
#endif
	}
}
