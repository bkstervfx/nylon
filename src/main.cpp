#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "database/database.hpp"
#include "database/staging.hpp"
#include "evidence/evidence.hpp"
#include "image/image.hpp"
#include "media/media.hpp"
#include "platform/win32.hpp"
#include "progress/progress.hpp"
#include "registry/registry.hpp"
#include "services/control_sets.hpp"

namespace
{

struct cli_options_t
{
	std::filesystem::path input;
	std::optional<std::uint32_t> image_index;
};

void print_error(const nylon::error_t& error)
{
	std::cerr << "error: " << error.message;
	if (error.code != 0)
	{
		std::cerr << " (" << error.code << ")";
	}
	std::cerr << '\n';
}

void print_usage()
{
	std::cout
		<< "Nylon Windows image auditor\n\n"
		<< "usage:\n"
		<< "  nylon.exe scan <expanded-windows-image>\n"
		<< "  nylon.exe scan <windows.iso> [--index <number>]\n";
}

nylon::result_t<std::uint32_t> parse_positive_u32(std::wstring_view value)
{
	if (value.empty())
	{
		return nylon::result_t<std::uint32_t>::failure("Image index cannot be empty");
	}

	std::uint64_t parsed = 0;
	for (const wchar_t character : value)
	{
		if (character < L'0' || character > L'9')
		{
			return nylon::result_t<std::uint32_t>::failure("Image index must be a positive integer");
		}
		parsed = parsed * 10U + static_cast<std::uint64_t>(character - L'0');
		if (parsed > std::numeric_limits<std::uint32_t>::max())
		{
			return nylon::result_t<std::uint32_t>::failure("Image index is too large");
		}
	}

	if (parsed == 0U)
	{
		return nylon::result_t<std::uint32_t>::failure("Image index must be greater than zero");
	}

	return nylon::result_t<std::uint32_t>::success(static_cast<std::uint32_t>(parsed));
}

nylon::result_t<cli_options_t> parse_cli(int argc, wchar_t** argv)
{
	if (argc != 3 && argc != 5)
	{
		return nylon::result_t<cli_options_t>::failure("Invalid command line");
	}
	if (std::wstring_view(argv[1]) != L"scan")
	{
		return nylon::result_t<cli_options_t>::failure("Unknown command");
	}

	cli_options_t options;
	options.input = argv[2];

	if (argc == 5)
	{
		if (std::wstring_view(argv[3]) != L"--index")
		{
			return nylon::result_t<cli_options_t>::failure("Expected --index before image number");
		}

		auto index = parse_positive_u32(argv[4]);
		if (!index.ok())
		{
			return nylon::result_t<cli_options_t>::failure(index.error().message, index.error().code);
		}
		options.image_index = index.value();
	}

	return nylon::result_t<cli_options_t>::success(std::move(options));
}

bool has_iso_extension(const std::filesystem::path& path)
{
	const auto extension = path.extension().native();
	return CompareStringOrdinal(
		extension.c_str(),
		static_cast<int>(extension.size()),
		L".iso",
		4,
		TRUE) == CSTR_EQUAL;
}

bool looks_like_project_root(const std::filesystem::path& candidate)
{
	std::error_code error;
	return std::filesystem::is_regular_file(candidate / L"build.cmd", error) && !error;
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

	const auto parent = executable.value().parent_path().lexically_normal();
	if (CompareStringOrdinal(
		parent.filename().c_str(),
		-1,
		L"bin",
		-1,
		TRUE) == CSTR_EQUAL)
	{
		const auto candidate = parent.parent_path().lexically_normal();
		if (looks_like_project_root(candidate))
		{
			return nylon::result_t<std::filesystem::path>::success(candidate);
		}
	}

	return nylon::result_t<std::filesystem::path>::success(parent);
}

nylon::result_t<std::filesystem::path> persistent_runtime_root()
{
	const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
	if (required == 0)
	{
		return nylon::result_t<std::filesystem::path>::failure(
			"LOCALAPPDATA is not available",
			GetLastError());
	}

	std::wstring local_appdata(required, L'\0');
	const DWORD written = GetEnvironmentVariableW(
		L"LOCALAPPDATA",
		local_appdata.data(),
		required);
	if (written == 0 || written >= required)
	{
		return nylon::result_t<std::filesystem::path>::failure(
			"Unable to read LOCALAPPDATA",
			GetLastError());
	}

	local_appdata.resize(written);
	return nylon::result_t<std::filesystem::path>::success(
		std::filesystem::path(std::move(local_appdata)) / L"Temp" / L"nylon");
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

nylon::status_t remove_database_artifacts(const std::filesystem::path& path)
{
	for (const auto& candidate : {
		path,
		std::filesystem::path(path.native() + L"-wal"),
		std::filesystem::path(path.native() + L"-journal"),
		std::filesystem::path(path.native() + L"-shm") })
	{
		std::error_code error;
		std::filesystem::remove(candidate, error);
		if (error)
		{
			return nylon::status_t::failure(
				"Unable to remove stale audit database staging files",
				static_cast<std::uint32_t>(error.value()));
		}
	}
	return nylon::status_t::success();
}

nylon::status_t publish_database(
	const std::filesystem::path& staging,
	const std::filesystem::path& destination)
{
	for (const auto& sidecar : {
		std::filesystem::path(destination.native() + L"-wal"),
		std::filesystem::path(destination.native() + L"-shm") })
	{
		std::error_code error;
		std::filesystem::remove(sidecar, error);
		if (error)
		{
			return nylon::status_t::failure(
				"Unable to remove stale audit database sidecar",
				static_cast<std::uint32_t>(error.value()));
		}
	}

	if (!MoveFileExW(
		staging.c_str(),
		destination.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		return nylon::status_t::failure("Unable to publish audit database", GetLastError());
	}
	return nylon::status_t::success();
}

}

int wmain(int argc, wchar_t** argv)
{
	auto cli_result = parse_cli(argc, argv);
	if (!cli_result.ok())
	{
		print_usage();
		print_error(cli_result.error());
		return 2;
	}
	const auto cli = std::move(cli_result).value();

	auto root_result = project_root();
	if (!root_result.ok())
	{
		print_error(root_result.error());
		return 1;
	}

	auto runtime_root_result = persistent_runtime_root();
	if (!runtime_root_result.ok())
	{
		print_error(runtime_root_result.error());
		return 1;
	}

	const auto nylon_root = root_result.value();
	const auto runtime_root = runtime_root_result.value();
	const auto output_root = nylon_root / L"output";
	const auto temp_root = runtime_root / L"temp";
	const auto cache_root = runtime_root / L"cache";
	const auto database_path = output_root / L"image.db";
	const auto staging_database_path = output_root / L"image.db.staging";

	auto status = create_runtime_directories(output_root, temp_root, cache_root);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	nylon::platform::unique_handle_t iso_read_lock;
	std::error_code input_type_error;
	if (has_iso_extension(cli.input) &&
		std::filesystem::is_regular_file(cli.input, input_type_error) &&
		!input_type_error)
	{
		auto lock_result = nylon::platform::open_read_lock(cli.input);
		if (!lock_result.ok())
		{
			print_error(lock_result.error());
			return 1;
		}
		iso_read_lock = std::move(lock_result).value();
	}

	std::cout << "input...\n";
	auto prepared_result = nylon::media::prepare(
		cli.input,
		nylon::media::prepare_options_t{
			runtime_root,
			cache_root,
			temp_root,
			cli.image_index
		});
	if (!prepared_result.ok())
	{
		print_error(prepared_result.error());
		return 1;
	}
	const auto prepared = std::move(prepared_result).value();

	if (prepared.source_kind == "iso")
	{
		std::cout << "iso sha256: " << prepared.iso_sha256 << '\n';
		std::cout << "image index: " << prepared.image_index;
		if (!prepared.image_name.empty()) std::cout << " (" << prepared.image_name << ")";
		std::cout << '\n';
		std::cout << "cache: " << (prepared.cache_hit ? "hit" : "created") << '\n';
	}

	status = nylon::media::verify_prepared_cache(prepared);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	auto image_result = nylon::image::image_t::open(prepared.image_root);
	if (!image_result.ok())
	{
		print_error(image_result.error());
		return 1;
	}
	auto image = std::move(image_result).value();

	status = remove_database_artifacts(staging_database_path);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	auto database_result = nylon::database::database_t::open(staging_database_path);
	if (!database_result.ok())
	{
		print_error(database_result.error());
		return 1;
	}
	auto database = std::move(database_result).value();

	status = nylon::database::prepare_staging_database(database);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = database.initialize();
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = nylon::database::activate_staging_database(database);
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
		std::pair<std::string_view, std::string_view>{ "schema_version", "6" },
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
	status = database.meta("source_kind", prepared.source_kind);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	if (prepared.source_kind == "iso")
	{
		status = database.meta("source_iso_sha256", prepared.iso_sha256);
		if (!status.ok())
		{
			print_error(status.error());
			return 1;
		}
		status = database.meta("source_image_index", std::to_string(prepared.image_index));
		if (!status.ok())
		{
			print_error(status.error());
			return 1;
		}
		status = database.meta("source_image_name", prepared.image_name);
		if (!status.ok())
		{
			print_error(status.error());
			return 1;
		}
		status = database.meta("source_cache", prepared.cache_hit ? "hit" : "created");
		if (!status.ok())
		{
			print_error(status.error());
			return 1;
		}
	}

	status = nylon::evidence::initialize(database, prepared);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}
	std::cout << "filesystem namespace...\n";
	status = nylon::evidence::inventory(database, image.root());
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

	nylon::progress::registry_state_t registry_progress;
	nylon::progress::registry_view_t registry_view(registry_progress);
	registry_view.start();
	status = nylon::registry::scan_all(image, database, &registry_progress);
	registry_view.finish(status.ok());
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

	sqlite3_stmt* registry_state = nullptr;
	if (sqlite3_prepare_v2(database.native_handle(), "SELECT state FROM collector_runs WHERE collector='registry';", -1, &registry_state, nullptr) != SQLITE_OK)
	{
		std::cerr << "Unable to query registry accounting state\n";
		return 1;
	}
	const bool registry_has_gaps = sqlite3_step(registry_state) != SQLITE_ROW ||
		std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(registry_state, 0))) != "COMPLETE";
	sqlite3_finalize(registry_state);
	status = database.collector_status("registry", registry_has_gaps ? "complete_with_gaps" : "complete");
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	std::cout << "offline control sets...\n";
	status = database.begin();
	if (status.ok()) status = nylon::services::collect_control_sets(database);
	if (status.ok()) status = database.commit();
	if (!status.ok())
	{
		database.rollback();
		print_error(status.error());
		return 1;
	}
	status = database.finalize_graph();
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	status = nylon::evidence::finalize(database);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}
	status = nylon::database::quick_check(database);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	database.close();
	status = publish_database(staging_database_path, database_path);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

	std::cout << "current collector pass complete; whole-image collector migration pending\n";
	auto database_display = nylon::platform::utf8(database_path.native());
	std::cout << "database: "
		<< (database_display.ok() ? database_display.value() : std::string("<path encoding error>"))
		<< '\n';
	return 0;
}
