#include <iostream>
#include <string_view>

#include "evidence/evidence.hpp"
#include "database/staging.hpp"

int wmain(int argc, wchar_t** argv)
{
	if (argc == 3 && std::wstring_view(argv[1]) == L"--parse")
	{
		auto parsed = nylon::media::read_catalog(argv[2]);
		if (!parsed.ok()) { std::cerr << parsed.error().message; return 1; }
		std::cout << parsed.value().images.size();
		return 0;
	}
	const bool verify = argc == 3 && std::wstring_view(argv[1]) == L"--verify";
	if (!verify && argc != 5 && argc != 6) return 2;
	auto opened = nylon::database::database_t::open(verify ? argv[2] : argv[4]);
	if (!opened.ok()) { std::cerr << opened.error().message; return 1; }
	auto database = std::move(opened).value();
	auto status = nylon::status_t::success();
	if (!verify)
	{
		nylon::media::prepare_options_t options{ std::filesystem::current_path(), argv[3], std::filesystem::path(argv[3]) / L"staging", 2 };
		if (argc == 6) options.nylon_root = argv[5];
		if (std::wstring_view(argv[1]) == L"--prepare-index1") options.image_index = 1;
		if (std::wstring_view(argv[1]) == L"--prepare-auto") options.image_index.reset();
		auto prepared = nylon::media::prepare(argv[2], options);
		if (!prepared.ok()) { std::cerr << prepared.error().message; return 1; }
		status = database.initialize();
		if (status.ok() && argc == 6) status = nylon::media::verify_prepared_cache(prepared.value());
		if (status.ok()) status = nylon::evidence::initialize(database, prepared.value());
	}
	if (status.ok()) status = nylon::evidence::verify_source_media(database);
	if (status.ok()) status = nylon::database::quick_check(database);
	if (!status.ok()) { std::cerr << status.error().message; return 1; }
	return 0;
}
