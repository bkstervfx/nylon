#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "database/database.hpp"
#include "image/image.hpp"
#include "platform/win32.hpp"
#include "registry/registry.hpp"

namespace
{

void print_error(const nylon::error_t& error)
{
	std::cerr << "error: " << error.message;
	if (error.code != 0)
	{
		std::cerr << " (" << error.code << ")";
	}
	std::cerr << '\n';
}

nylon::result_t<std::filesystem::path> project_root()
{
	auto executable = nylon::platform::executable_path();
	if (!executable.ok())
	{
		return nylon::result_t<std::filesystem::path>::failure(
			executable.error().message,
			executable.error().code);
	}

	const auto parent = executable.value().parent_path();
	if (CompareStringOrdinal(
		parent.filename().c_str(),
		-1,
		L"bin",
		-1,
		TRUE) == CSTR_EQUAL)
	{
		return nylon::result_t<std::filesystem::path>::success(
			parent.parent_path().lexically_normal());
	}

	return nylon::result_t<std::filesystem::path>::success(L"C:\\nylon");
}

nylon::status_t create_runtime_directories(
	const std::filesystem::path& output,
	const std::filesystem::path& temp,
	const std::filesystem::path& cache)
{
	for (const auto& directory : { output, temp, cache })
	{
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error)
		{
			return nylon::status_t::failure(
				"Unable to create Nylon runtime directory",
				static_cast<std::uint32_t>(error.value()));
		}
	}
	return nylon::status_t::success();
}

nylon::status_t replace_database(const std::filesystem::path& path)
{
	for (const auto& candidate : {
		path,
		std::filesystem::path(path.native() + L"-wal"),
		std::filesystem::path(path.native() + L"-shm") })
	{
		std::error_code error;
		std::filesystem::remove(candidate, error);
		if (error)
		{
			return nylon::status_t::failure(
				"Unable to replace the previous audit database",
				static_cast<std::uint32_t>(error.value()));
		}
	}
	return nylon::status_t::success();
}

}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 3 || std::wstring_view(argv[1]) != L"scan")
	{
		std::cout
			<< "Nylon Windows image auditor\n\n"
			<< "usage:\n"
			<< "  nylon.exe scan <expanded-windows-image>\n";
		return 2;
	}

	auto root_result = project_root();
	if (!root_result.ok())
	{
		print_error(root_result.error());
		return 1;
	}

	const auto nylon_root = root_result.value();
	const auto output_root = nylon_root / L"output";
	const auto temp_root = nylon_root / L"temp";
	const auto cache_root = nylon_root / L"cache";
	const auto database_path = output_root / L"image.db";

	auto status = create_runtime_directories(output_root, temp_root, cache_root);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = replace_database(database_path);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	auto image_result = nylon::image::image_t::open(argv[2]);
	if (!image_result.ok())
	{
		print_error(image_result.error());
		return 1;
	}
	auto image = std::move(image_result).value();

	auto database_result = nylon::database::database_t::open(database_path);
	if (!database_result.ok())
	{
		print_error(database_result.error());
		return 1;
	}
	auto database = std::move(database_result).value();

	status = database.initialize();
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	auto image_path = image.utf8_path(image.root());
	if (!image_path.ok())
	{
		print_error(image_path.error());
		return 1;
	}

	for (const auto& [key, value] : {
		std::pair<std::string_view, std::string_view>{ "schema_version", "1" },
		{ "mode", "read_only_static_audit" } })
	{
		status = database.meta(key, value);
		if (!status.ok())
		{
			print_error(status.error());
			return 1;
		}
	}
	status = database.meta("image_root", image_path.value());
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = database.collector_status("registry", "running");
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = database.begin();
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	std::cout << "registry...\n";
	status = nylon::registry::scan_all(image, database);
	if (!status.ok())
	{
		database.rollback();
		database.collector_status("registry", "failed", status.error().message);
		print_error(status.error());
		return 1;
	}

	status = database.commit();
	if (!status.ok())
	{
		database.rollback();
		database.collector_status("registry", "failed", status.error().message);
		print_error(status.error());
		return 1;
	}

	status = database.collector_status("registry", "complete");
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = database.finalize_graph();
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	std::cout << "complete\n";
	std::cout << "database: " << database_path.string() << '\n';
	return 0;
}
