#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "database/database.hpp"
#include "image/image.hpp"
#include "media/media.hpp"
#include "platform/hash.hpp"
#include "platform/win32.hpp"
#include "registry/registry.hpp"

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

bool looks_like_project_root(const std::filesystem::path& candidate)
{
	std::error_code error;
	if (std::filesystem::is_regular_file(candidate / L"build.cmd", error) && !error)
	{
		return true;
	}

	error.clear();
	return std::filesystem::is_directory(candidate / L"tools" / L"media", error) && !error;
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

	std::cout << "input...\n";
	auto prepared_result = nylon::media::prepare(
		cli.input,
		nylon::media::prepare_options_t{
			nylon_root,
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
		if (!prepared.cache_hit)
		{
			auto stable_hash = nylon::platform::sha256_file(cli.input);
			if (!stable_hash.ok())
			{
				print_error(stable_hash.error());
				return 1;
			}
			if (stable_hash.value() != prepared.iso_sha256)
			{
				print_error(nylon::error_t{
					"ISO changed while it was being prepared; retry with a stable input file",
					ERROR_FILE_INVALID });
				return 1;
			}
		}

		std::cout << "iso sha256: " << prepared.iso_sha256 << '\n';
		std::cout << "image index: " << prepared.image_index;
		if (!prepared.image_name.empty()) std::cout << " (" << prepared.image_name << ")";
		std::cout << '\n';
		std::cout << "cache: " << (prepared.cache_hit ? "hit" : "created") << '\n';
	}

	auto image_result = nylon::image::image_t::open(prepared.image_root);
	if (!image_result.ok())
	{
		print_error(image_result.error());
		return 1;
	}
	auto image = std::move(image_result).value();

	status = replace_database(database_path);
	if (!status.ok())
	{
		print_error(status.error());
		return 1;
	}

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
	auto database_display = nylon::platform::utf8(database_path.native());
	std::cout << "database: "
		<< (database_display.ok() ? database_display.value() : std::string("<path encoding error>"))
		<< '\n';
	return 0;
}
