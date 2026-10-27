#include "dism.hpp"

#include "resource.hpp"

#include <Windows.h>

#if __has_include(<DismApi.h>)
#include <DismApi.h>
#define NYLON_HAS_DISM_HEADER 1
#else
#define NYLON_HAS_DISM_HEADER 0
#endif

#include <bit>
#include <stdexcept>
#include <utility>

namespace
{
#if NYLON_HAS_DISM_HEADER
	using initialize_fn_t = decltype( &::DismInitialize );
	using shutdown_fn_t = decltype( &::DismShutdown );
	using open_session_fn_t = decltype( &::DismOpenSession );
	using close_session_fn_t = decltype( &::DismCloseSession );
	using delete_fn_t = decltype( &::DismDelete );
	using get_packages_fn_t = decltype( &::DismGetPackages );
	using get_features_fn_t = decltype( &::DismGetFeatures );
	using get_capabilities_fn_t = decltype( &::DismGetCapabilities );
	using get_drivers_fn_t = decltype( &::DismGetDrivers );

	template <typename function_t>
	[[nodiscard]] function_t proc( HMODULE module, const char* name )
	{
		const FARPROC raw = ::GetProcAddress( module, name );

		if ( raw == nullptr )
		{
			throw std::runtime_error( std::string( "DismAPI.dll missing export: " ) + name );
		}

		return std::bit_cast<function_t>( raw );
	}

	class dism_buffer_t final
	{
	public:
		dism_buffer_t( void* value, delete_fn_t deleter ) noexcept
			: value_( value ),
			  deleter_( deleter )
		{
		}

		dism_buffer_t( const dism_buffer_t& ) = delete;
		dism_buffer_t& operator=( const dism_buffer_t& ) = delete;

		~dism_buffer_t()
		{
			if ( value_ != nullptr && deleter_ != nullptr )
			{
				deleter_( value_ );
			}
		}

	private:
		void* value_ = nullptr;
		delete_fn_t deleter_ = nullptr;
	};

	[[nodiscard]] std::wstring safe_text( PCWSTR value )
	{
		return value == nullptr ? std::wstring{} : std::wstring( value );
	}
#endif
}

namespace nylon::win32
{
	dism_inventory_t dism_inventory(
		const std::filesystem::path& image_root,
		const std::filesystem::path& scratch_root,
		const std::filesystem::path& log_file )
	{
		dism_inventory_t result;

#if !NYLON_HAS_DISM_HEADER
		(void)image_root;
		(void)scratch_root;
		(void)log_file;
		result.unavailable_reason = L"DismApi.h is not installed with this build environment";
		return result;
#else
		module_t module( ::LoadLibraryExW(
			L"DismAPI.dll",
			nullptr,
			LOAD_LIBRARY_SEARCH_SYSTEM32 ) );

		if ( !module )
		{
			result.unavailable_reason = L"DismAPI.dll is not available on the host";
			return result;
		}

		const auto initialize = proc<initialize_fn_t>( module.get(), "DismInitialize" );
		const auto shutdown = proc<shutdown_fn_t>( module.get(), "DismShutdown" );
		const auto open_session = proc<open_session_fn_t>( module.get(), "DismOpenSession" );
		const auto close_session = proc<close_session_fn_t>( module.get(), "DismCloseSession" );
		const auto dism_delete = proc<delete_fn_t>( module.get(), "DismDelete" );
		const auto get_packages = proc<get_packages_fn_t>( module.get(), "DismGetPackages" );
		const auto get_features = proc<get_features_fn_t>( module.get(), "DismGetFeatures" );
		const auto get_capabilities = proc<get_capabilities_fn_t>( module.get(), "DismGetCapabilities" );
		const auto get_drivers = proc<get_drivers_fn_t>( module.get(), "DismGetDrivers" );

		const HRESULT initialize_result = initialize(
			DismLogErrorsWarnings,
			log_file.c_str(),
			scratch_root.c_str() );

		if ( FAILED( initialize_result ) )
		{
			result.unavailable_reason = L"DismInitialize failed";
			return result;
		}

		struct shutdown_guard_t final
		{
			shutdown_fn_t function = nullptr;

			~shutdown_guard_t()
			{
				if ( function != nullptr )
				{
					function();
				}
			}
		} shutdown_guard{ shutdown };

		DismSession session = 0;
		const HRESULT open_result = open_session(
			image_root.c_str(),
			nullptr,
			nullptr,
			&session );

		if ( FAILED( open_result ) )
		{
			result.unavailable_reason = L"DismOpenSession failed";
			return result;
		}

		struct session_guard_t final
		{
			DismSession session = 0;
			close_session_fn_t close = nullptr;

			~session_guard_t()
			{
				if ( session != 0 && close != nullptr )
				{
					close( session );
				}
			}
		} session_guard{ session, close_session };

		result.available = true;

		DismPackage* packages = nullptr;
		UINT package_count = 0;

		if ( SUCCEEDED( get_packages( session, &packages, &package_count ) ) )
		{
			dism_buffer_t buffer( packages, dism_delete );
			result.packages.reserve( package_count );

			for ( UINT index = 0; index < package_count; ++index )
			{
				result.packages.push_back(
					{
						.name = safe_text( packages[index].PackageName ),
						.state = static_cast<std::uint32_t>( packages[index].PackageState ),
						.release_type = static_cast<std::uint32_t>( packages[index].ReleaseType )
					} );
			}
		}

		DismFeature* features = nullptr;
		UINT feature_count = 0;

		if ( SUCCEEDED( get_features(
			session,
			nullptr,
			PackageNone,
			&features,
			&feature_count ) ) )
		{
			dism_buffer_t buffer( features, dism_delete );
			result.features.reserve( feature_count );

			for ( UINT index = 0; index < feature_count; ++index )
			{
				result.features.push_back(
					{
						.name = safe_text( features[index].FeatureName ),
						.state = static_cast<std::uint32_t>( features[index].State )
					} );
			}
		}

		DismCapability* capabilities = nullptr;
		UINT capability_count = 0;

		if ( SUCCEEDED( get_capabilities( session, &capabilities, &capability_count ) ) )
		{
			dism_buffer_t buffer( capabilities, dism_delete );
			result.capabilities.reserve( capability_count );

			for ( UINT index = 0; index < capability_count; ++index )
			{
				result.capabilities.push_back(
					{
						.name = safe_text( capabilities[index].Name ),
						.state = static_cast<std::uint32_t>( capabilities[index].State )
					} );
			}
		}

		DismDriverPackage* drivers = nullptr;
		UINT driver_count = 0;

		if ( SUCCEEDED( get_drivers( session, TRUE, &drivers, &driver_count ) ) )
		{
			dism_buffer_t buffer( drivers, dism_delete );
			result.drivers.reserve( driver_count );

			for ( UINT index = 0; index < driver_count; ++index )
			{
				result.drivers.push_back(
					{
						.published_name = safe_text( drivers[index].PublishedName ),
						.original_file_name = safe_text( drivers[index].OriginalFileName ),
						.class_name = safe_text( drivers[index].ClassName ),
						.class_guid = safe_text( drivers[index].ClassGuid ),
						.provider_name = safe_text( drivers[index].ProviderName ),
						.inbox = drivers[index].InBox != FALSE,
						.boot_critical = drivers[index].BootCritical != FALSE
					} );
			}
		}

		return result;
#endif
	}
}
