#include "servicing.hpp"

#include "../database/database.hpp"
#include "../win32/delta.hpp"
#include "../win32/dism.hpp"
#include "../xml/xml.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{
	constexpr std::uint64_t MAX_MANIFEST_SIZE = 128ULL * 1024ULL * 1024ULL;

	struct assembly_t final
	{
		std::wstring name;
		std::wstring version;
		std::wstring architecture;
		std::wstring token;
		std::wstring language;
		std::vector<std::wstring> dependencies;
		std::vector<std::wstring> components;
		std::vector<std::wstring> files;
		std::vector<std::wstring> registry_keys;
	};

	[[nodiscard]] std::optional<std::vector<std::byte>> read_bytes(
		const std::filesystem::path& path )
	{
		std::error_code error;
		const std::uint64_t size = std::filesystem::file_size( path, error );

		if ( error || size == 0 || size > MAX_MANIFEST_SIZE )
		{
			return std::nullopt;
		}

		std::ifstream stream( path, std::ios::binary );

		if ( !stream )
		{
			return std::nullopt;
		}

		std::vector<std::byte> result( static_cast<std::size_t>( size ) );
		stream.read(
			reinterpret_cast<char*>( result.data() ),
			static_cast<std::streamsize>( result.size() ) );

		if ( !stream )
		{
			return std::nullopt;
		}

		return result;
	}

	template <typename reader_t>
	[[nodiscard]] assembly_t parse_assembly( reader_t&& reader )
	{
		assembly_t result;
		bool saw_identity = false;
		std::size_t dependency_depth = 0;

		const auto on_start =
			[&]( std::wstring_view element, const auto& attribute )
			{
				if ( element == L"dependency" )
				{
					++dependency_depth;
					return;
				}

				if ( element == L"assemblyIdentity" )
				{
					const auto name = attribute( L"name" );

					if ( name.empty() )
					{
						return;
					}

					if ( !saw_identity )
					{
						saw_identity = true;
						result.name = name;
						result.version = attribute( L"version" );
						result.architecture = attribute( L"processorArchitecture" );
						result.token = attribute( L"publicKeyToken" );
						result.language = attribute( L"language" );
					}
					else if ( dependency_depth != 0 )
					{
						result.dependencies.push_back( name );
					}
					else
					{
						result.components.push_back( name );
					}

					return;
				}

				if ( element == L"file" )
				{
					const auto name = attribute( L"name" );

					if ( !name.empty() )
					{
						result.files.push_back( name );
					}

					return;
				}

				if ( element == L"registryKey" )
				{
					const auto key = attribute( L"keyName" );

					if ( !key.empty() )
					{
						result.registry_keys.push_back( key );
					}
				}
			};

		const auto on_end =
			[&]( std::wstring_view element )
			{
				if ( element == L"dependency" && dependency_depth != 0 )
				{
					--dependency_depth;
				}
			};

		reader( on_start, on_end );
		return result;
	}

	[[nodiscard]] std::optional<assembly_t> parse_manifest(
		const std::filesystem::path& path,
		const std::filesystem::path& offline_wcp )
	{
		const auto raw = read_bytes( path );

		if ( !raw )
		{
			return std::nullopt;
		}

		const bool delta_encoded =
			raw->size() >= 4U &&
			( *raw )[0] == static_cast<std::byte>( 'D' ) &&
			( *raw )[1] == static_cast<std::byte>( 'C' ) &&
			( ( *raw )[2] == static_cast<std::byte>( 'M' ) ||
				( *raw )[2] == static_cast<std::byte>( 'N' ) ||
				( *raw )[2] == static_cast<std::byte>( 'S' ) ) &&
			( *raw )[3] == static_cast<std::byte>( 1 );

		if ( delta_encoded )
		{
			const auto decoded = nylon::win32::decode_dcm_manifest( offline_wcp, *raw );

			if ( !decoded )
			{
				return std::nullopt;
			}

			return parse_assembly(
				[&]( const auto& callback, const auto& end_callback )
				{
					nylon::xml::read(
						std::span<const std::byte>( *decoded ),
						callback,
						{},
						end_callback );
				} );
		}

		return parse_assembly(
			[&]( const auto& callback, const auto& end_callback )
			{
				nylon::xml::read( path, callback, {}, end_callback );
			} );
	}

	void scan_cbs_packages(
		const std::filesystem::path& image_root,
		nylon::database::database_t& database )
	{
		const auto root = image_root / L"Windows" / L"servicing" / L"Packages";

		if ( !std::filesystem::is_directory( root ) )
		{
			return;
		}

		std::error_code error;

		for ( const auto& entry : std::filesystem::directory_iterator(
			root,
			std::filesystem::directory_options::skip_permission_denied,
			error ) )
		{
			if ( error )
			{
				error.clear();
				continue;
			}

			if ( !entry.is_regular_file( error ) ||
				error ||
				entry.path().extension() != L".mum" )
			{
				error.clear();
				continue;
			}

			try
			{
				const auto parsed = parse_assembly(
					[&]( const auto& callback, const auto& end_callback )
					{
						nylon::xml::read( entry.path(), callback, {}, end_callback );
					} );

				const std::wstring name = parsed.name.empty()
					? entry.path().stem().native()
					: parsed.name;

				const auto package_id = database.object(
					L"cbs_package",
					name,
					entry.path().native() );

				if ( !parsed.version.empty() )
				{
					database.property( package_id, L"version", parsed.version );
				}

				if ( !parsed.architecture.empty() )
				{
					database.property( package_id, L"architecture", parsed.architecture );
				}

				const auto manifest_file_id = database.find_object_by_path(
					L"file",
					entry.path().native() );

				if ( manifest_file_id != 0 )
				{
					database.edge( package_id, L"manifest", manifest_file_id, L"CBS .mum" );
				}

				const auto catalog = entry.path().parent_path() /
					( entry.path().stem().native() + L".cat" );

				const auto catalog_id = database.find_object_by_path( L"file", catalog.native() );

				if ( catalog_id != 0 )
				{
					database.edge( package_id, L"catalog", catalog_id, L"CBS package catalog" );
				}

				for ( const auto& dependency : parsed.dependencies )
				{
					const auto dependency_id = database.object( L"cbs_package_reference", dependency );
					database.edge( package_id, L"depends_on", dependency_id, L"CBS .mum dependency" );
				}

				for ( const auto& component : parsed.components )
				{
					const auto component_id = database.object( L"component_reference", component );
					database.edge( package_id, L"contains_component", component_id, L"CBS .mum component" );
				}
			}
			catch ( const std::exception& )
			{
				database.note( L"servicing", L"warning", L"CBS package parse failed: " + entry.path().native() );
			}
		}
	}

	void scan_winsxs_manifests(
		const nylon::image::paths_t& paths,
		nylon::database::database_t& database )
	{
		const auto winsxs = paths.image_root / L"Windows" / L"WinSxS";
		const auto manifests = winsxs / L"Manifests";
		const auto offline_wcp = paths.image_root / L"Windows" / L"System32" / L"wcp.dll";

		if ( !std::filesystem::is_directory( manifests ) )
		{
			return;
		}

		std::error_code error;

		for ( const auto& entry : std::filesystem::directory_iterator(
			manifests,
			std::filesystem::directory_options::skip_permission_denied,
			error ) )
		{
			if ( error )
			{
				error.clear();
				continue;
			}

			if ( !entry.is_regular_file( error ) ||
				error ||
				entry.path().extension() != L".manifest" )
			{
				error.clear();
				continue;
			}

			const std::wstring servicing_name = entry.path().stem().native();
			const auto payload = winsxs / servicing_name;

			try
			{
				const auto parsed = parse_manifest( entry.path(), offline_wcp );
				const std::wstring identity = parsed && !parsed->name.empty()
					? parsed->name
					: servicing_name;

				const auto component_id = database.object(
					L"winsxs_component",
					identity,
					std::filesystem::is_directory( payload ) ? payload.native() : std::wstring{} );

				database.property( component_id, L"servicing_name", servicing_name );

				const auto manifest_file_id = database.find_object_by_path(
					L"file",
					entry.path().native() );

				if ( manifest_file_id != 0 )
				{
					database.edge( component_id, L"manifest", manifest_file_id, L"WinSxS manifest" );
				}

				if ( std::filesystem::is_directory( payload ) )
				{
					std::wstring prefix = payload.native();

					if ( !prefix.ends_with( L'\\' ) )
					{
						prefix.push_back( L'\\' );
					}

					database.link_path_prefix( component_id, L"owns", prefix, L"WinSxS payload" );
				}

				const auto registry_key_id = database.find_object_by_path(
					L"registry_key",
					L"COMPONENTS\\DerivedData\\Components\\" + servicing_name );

				if ( registry_key_id != 0 )
				{
					database.edge( component_id, L"registered_at", registry_key_id, L"COMPONENTS DerivedData Components" );
				}

				if ( parsed )
				{
					if ( !parsed->version.empty() )
					{
						database.property( component_id, L"version", parsed->version );
					}

					if ( !parsed->architecture.empty() )
					{
						database.property( component_id, L"architecture", parsed->architecture );
					}

					if ( !parsed->token.empty() )
					{
						database.property( component_id, L"public_key_token", parsed->token );
					}

					if ( !parsed->language.empty() )
					{
						database.property( component_id, L"language", parsed->language );
					}

					for ( const auto& dependency : parsed->dependencies )
					{
						const auto dependency_id = database.object( L"component_reference", dependency );
						database.edge( component_id, L"depends_on", dependency_id, L"WinSxS manifest dependency" );
					}

					for ( const auto& nested_component : parsed->components )
					{
						const auto nested_id = database.object( L"component_reference", nested_component );
						database.edge(
							component_id,
							L"contains_component",
							nested_id,
							L"WinSxS manifest assembly identity" );
					}

					for ( const auto& file_name : parsed->files )
					{
						const auto file_path = payload / file_name;
						const auto file_id = database.find_object_by_path( L"file", file_path.native() );

						if ( file_id != 0 )
						{
							database.edge( component_id, L"declares_file", file_id, L"WinSxS manifest file" );
						}
						else
						{
							const auto reference_id = database.object( L"file_reference", file_name );
							database.edge( component_id, L"declares_file", reference_id, L"WinSxS manifest file" );
						}
					}

					for ( const auto& registry_key : parsed->registry_keys )
					{
						const auto reference_id = database.object( L"registry_reference", registry_key );
						database.edge( component_id, L"declares_registry", reference_id, L"WinSxS manifest registryKey" );
					}
				}
				else
				{
					database.note(
						L"servicing",
						L"warning",
						L"manifest could not be decoded/parsed: " + entry.path().native() );
				}
			}
			catch ( const std::exception& )
			{
				database.note( L"servicing", L"warning", L"manifest scan failed: " + entry.path().native() );
			}
		}
	}

	void scan_servicing_registry(
		nylon::database::database_t& database )
	{
		const std::wstring winner_prefix =
			L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SideBySide\\Winners\\";

		for ( const auto& key : database.objects_with_path_prefix( L"registry_key", winner_prefix ) )
		{
			const std::wstring remainder = key.path.substr( winner_prefix.size() );

			if ( remainder.empty() || remainder.find( L'\\' ) != std::wstring::npos )
			{
				continue;
			}

			const auto winner_id = database.object( L"sxs_winner", remainder, key.path );
			database.edge( winner_id, L"registered_at", key.id, L"SOFTWARE SideBySide Winners" );
		}

		const std::wstring family_marker = L"\\ComponentFamilies\\";

		for ( const auto& key : database.objects_with_path_prefix(
			L"registry_key",
			L"COMPONENTS\\DerivedData\\VersionedIndex\\" ) )
		{
			const std::size_t marker = key.path.find( family_marker );

			if ( marker == std::wstring::npos )
			{
				continue;
			}

			const std::wstring family = key.path.substr( marker + family_marker.size() );

			if ( family.empty() || family.find( L'\\' ) != std::wstring::npos )
			{
				continue;
			}

			const auto family_id = database.object( L"component_family", family, key.path );
			database.edge( family_id, L"registered_at", key.id, L"COMPONENTS ComponentFamilies" );
		}
	}

	void scan_dism(
		const nylon::image::paths_t& paths,
		nylon::database::database_t& database )
	{
		const auto scratch = paths.temp_root / L"dism";
		std::filesystem::create_directories( scratch );

		const auto inventory = nylon::win32::dism_inventory(
			paths.image_root,
			scratch,
			paths.output_root / L"dism.log" );

		if ( !inventory.available )
		{
			database.note( L"dism", L"warning", inventory.unavailable_reason );
			return;
		}

		for ( const auto& package : inventory.packages )
		{
			const auto id = database.object( L"dism_package", package.name );
			database.property( id, L"state", std::to_wstring( package.state ) );
			database.property( id, L"release_type", std::to_wstring( package.release_type ) );
		}

		for ( const auto& feature : inventory.features )
		{
			const auto id = database.object( L"windows_feature", feature.name );
			database.property( id, L"state", std::to_wstring( feature.state ) );
		}

		for ( const auto& capability : inventory.capabilities )
		{
			const auto id = database.object( L"windows_capability", capability.name );
			database.property( id, L"state", std::to_wstring( capability.state ) );
		}

		for ( const auto& driver : inventory.drivers )
		{
			const auto id = database.object( L"driver_package", driver.published_name );

			if ( !driver.original_file_name.empty() )
			{
				database.property( id, L"original_file", driver.original_file_name );
			}

			if ( !driver.class_name.empty() )
			{
				database.property( id, L"class", driver.class_name );
			}

			if ( !driver.class_guid.empty() )
			{
				database.property( id, L"class_guid", driver.class_guid );
			}

			if ( !driver.provider_name.empty() )
			{
				database.property( id, L"provider", driver.provider_name );
			}

			database.property( id, L"inbox", driver.inbox ? L"1" : L"0" );
			database.property( id, L"boot_critical", driver.boot_critical ? L"1" : L"0" );
		}
	}
}

namespace nylon::servicing
{
	void scan( const image::paths_t& paths, database::database_t& database )
	{
		database.note( L"servicing", L"info", L"scan started" );

		scan_cbs_packages( paths.image_root, database );
		scan_winsxs_manifests( paths, database );
		scan_servicing_registry( database );
		scan_dism( paths, database );

		database.note( L"servicing", L"info", L"scan completed" );
	}
}
