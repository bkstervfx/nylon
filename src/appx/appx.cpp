#include "appx.hpp"

#include "../database/database.hpp"
#include "../xml/xml.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	struct package_manifest_t final
	{
		std::wstring identity_name;
		std::wstring publisher;
		std::wstring version;
		std::wstring architecture;
		std::vector<std::wstring> dependencies;
		std::vector<std::wstring> capabilities;
		std::vector<std::wstring> executables;
		std::vector<std::wstring> extensions;
	};

	[[nodiscard]] package_manifest_t parse_manifest( const std::filesystem::path& path )
	{
		package_manifest_t result;

		nylon::xml::read(
			path,
			[&]( std::wstring_view element, const auto& attribute )
			{
				if ( element == L"Identity" )
				{
					result.identity_name = attribute( L"Name" );
					result.publisher = attribute( L"Publisher" );
					result.version = attribute( L"Version" );
					result.architecture = attribute( L"ProcessorArchitecture" );
					return;
				}

				if ( element == L"PackageDependency" ||
					element == L"MainPackageDependency" ||
					element == L"HostRuntimeDependency" )
				{
					const auto name = attribute( L"Name" );

					if ( !name.empty() )
					{
						result.dependencies.push_back( name );
					}

					return;
				}

				if ( element == L"TargetDeviceFamily" )
				{
					const auto name = attribute( L"Name" );

					if ( !name.empty() )
					{
						result.dependencies.push_back( L"device-family:" + name );
					}

					return;
				}

				if ( element == L"Capability" ||
					element == L"DeviceCapability" ||
					element == L"CustomCapability" )
				{
					const auto name = attribute( L"Name" );

					if ( !name.empty() )
					{
						result.capabilities.push_back( name );
					}

					return;
				}

				if ( element == L"Extension" )
				{
					const auto category = attribute( L"Category" );

					if ( !category.empty() )
					{
						result.extensions.push_back( category );
					}

					return;
				}

				if ( element == L"Application" )
				{
					const auto executable = attribute( L"Executable" );

					if ( !executable.empty() )
					{
						result.executables.push_back( executable );
					}
				}
			} );

		return result;
	}

	void scan_block_map(
		const std::filesystem::path& package_root,
		std::int64_t package_id,
		nylon::database::database_t& database )
	{
		const auto block_map = package_root / L"AppxBlockMap.xml";

		if ( !std::filesystem::is_regular_file( block_map ) )
		{
			return;
		}

		const auto block_map_file_id = database.find_object_by_path(
			L"file",
			block_map.native() );

		if ( block_map_file_id != 0 )
		{
			database.edge( package_id, L"block_map", block_map_file_id, L"AppxBlockMap.xml" );
		}

		try
		{
			nylon::xml::read(
				block_map,
				[&]( std::wstring_view element, const auto& attribute )
				{
					if ( element != L"File" )
					{
						return;
					}

					std::wstring name = attribute( L"Name" );

					if ( name.empty() )
					{
						return;
					}

					std::replace( name.begin(), name.end(), L'/', L'\\' );
					const auto path = package_root / name;
					const auto file_id = database.find_object_by_path( L"file", path.native() );

					if ( file_id != 0 )
					{
						database.edge(
							package_id,
							L"declares_file",
							file_id,
							L"AppxBlockMap.xml" );
						return;
					}

					const auto reference_id = database.object(
						L"file_reference",
						name,
						path.native() );

					database.edge(
						package_id,
						L"declares_file",
						reference_id,
						L"AppxBlockMap.xml" );
				} );
		}
		catch ( const std::exception& )
		{
			database.note(
				L"appx",
				L"warning",
				L"AppxBlockMap.xml parse failed: " + block_map.native() );
		}
	}

	void scan_package_root(
		const std::filesystem::path& root,
		const std::wstring& source,
		nylon::database::database_t& database )
	{
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
				database.note( L"appx", L"warning", L"directory enumeration failed: " + root.native() );
				error.clear();
				continue;
			}

			if ( !entry.is_directory( error ) )
			{
				continue;
			}

			const auto manifest = entry.path() / L"AppxManifest.xml";

			if ( !std::filesystem::is_regular_file( manifest ) )
			{
				continue;
			}

			try
			{
				const auto parsed = parse_manifest( manifest );
				const std::wstring package_name = parsed.identity_name.empty()
					? entry.path().filename().native()
					: parsed.identity_name;

				const auto package_id = database.object(
					L"appx_package",
					package_name,
					entry.path().native() );

				database.property( package_id, L"source", source );

				if ( !parsed.publisher.empty() )
				{
					database.property( package_id, L"publisher", parsed.publisher );
				}

				if ( !parsed.version.empty() )
				{
					database.property( package_id, L"version", parsed.version );
				}

				if ( !parsed.architecture.empty() )
				{
					database.property( package_id, L"architecture", parsed.architecture );
				}

				const auto manifest_file_id = database.find_object_by_path( L"file", manifest.native() );

				if ( manifest_file_id != 0 )
				{
					database.edge( package_id, L"manifest", manifest_file_id, L"AppxManifest.xml" );
				}

				std::wstring prefix = entry.path().native();

				if ( !prefix.ends_with( L'\\' ) )
				{
					prefix.push_back( L'\\' );
				}

				database.link_path_prefix( package_id, L"owns", prefix, L"package directory" );

				for ( const auto& dependency : parsed.dependencies )
				{
					const auto dependency_id = database.object( L"appx_dependency", dependency );
					database.edge( package_id, L"depends_on", dependency_id, L"AppxManifest.xml" );
				}

				for ( const auto& capability : parsed.capabilities )
				{
					const auto capability_id = database.object( L"appx_capability", capability );
					database.edge( package_id, L"declares_capability", capability_id, L"AppxManifest.xml" );
				}

				for ( const auto& executable : parsed.executables )
				{
					const auto executable_path = entry.path() / executable;
					const auto file_id = database.find_object_by_path( L"file", executable_path.native() );

					if ( file_id != 0 )
					{
						database.edge( package_id, L"executes", file_id, L"AppxManifest.xml" );
					}
					else
					{
						database.property( package_id, L"declared_executable", executable );
					}
				}

				for ( const auto& extension : parsed.extensions )
				{
					const auto extension_id = database.object( L"appx_extension", extension );
					database.edge(
						package_id,
						L"declares_extension",
						extension_id,
						L"AppxManifest.xml" );
				}

				scan_block_map( entry.path(), package_id, database );
			}
			catch ( const std::exception& )
			{
				database.note( L"appx", L"warning", L"manifest parse failed: " + manifest.native() );
			}
		}
	}

	void scan_registry_state( nylon::database::database_t& database )
	{
		const std::pair<std::wstring_view, std::wstring_view> roots[] =
		{
			{
				L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Applications\\",
				L"application"
			},
			{
				L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\InboxApplications\\",
				L"inbox_application"
			},
			{
				L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Config\\",
				L"config"
			},
			{
				L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Staged\\",
				L"staged"
			},
			{
				L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Families\\",
				L"family"
			}
		};

		for ( const auto& [prefix, kind] : roots )
		{
			for ( const auto& key : database.objects_with_path_prefix( L"registry_key", prefix ) )
			{
				const std::wstring remainder = key.path.substr( prefix.size() );

				if ( remainder.empty() || remainder.find( L'\\' ) != std::wstring::npos )
				{
					continue;
				}

				const auto registration_id = database.object(
					L"appx_registration",
					remainder,
					key.path );

				database.property( registration_id, L"kind", kind );
				database.edge(
					registration_id,
					L"registered_at",
					key.id,
					L"AppxAllUserStore" );
			}
		}
	}

	void scan_provisioning(
		const std::filesystem::path& image_root,
		nylon::database::database_t& database )
	{
		const auto path = image_root / L"ProgramData" / L"Microsoft" / L"Windows" / L"AppxProvisioning.xml";

		if ( !std::filesystem::is_regular_file( path ) )
		{
			return;
		}

		try
		{
			nylon::xml::read(
				path,
				[&]( std::wstring_view element, const auto& attribute )
				{
					if ( element != L"Package" )
					{
						return;
					}

					std::wstring name = attribute( L"PackageFullName" );

					if ( name.empty() )
					{
						name = attribute( L"PackageName" );
					}

					if ( name.empty() )
					{
						name = attribute( L"Name" );
					}

					if ( name.empty() )
					{
						return;
					}

					const auto provisioning_id = database.object( L"appx_provisioning", name, path.native() );
					const auto package_id = database.object( L"appx_reference", name );
					database.edge( provisioning_id, L"provisions", package_id, L"AppxProvisioning.xml" );
				} );
		}
		catch ( const std::exception& )
		{
			database.note( L"appx", L"warning", L"AppxProvisioning.xml parse failed" );
		}
	}
}

namespace nylon::appx
{
	void scan( const std::filesystem::path& image_root, database::database_t& database )
	{
		database.note( L"appx", L"info", L"scan started" );

		scan_package_root( image_root / L"Windows" / L"SystemApps", L"SystemApps", database );
		scan_package_root( image_root / L"Windows" / L"InboxApps", L"InboxApps", database );
		scan_package_root( image_root / L"Program Files" / L"WindowsApps", L"WindowsApps", database );
		scan_registry_state( database );
		scan_provisioning( image_root, database );

		database.note( L"appx", L"info", L"scan completed" );
	}
}
