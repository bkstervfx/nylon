#include <iostream>

#include "evidence/evidence.hpp"
#include "database/staging.hpp"

int wmain(int argc, wchar_t** argv)
{
	if (argc != 3) return 2;
	auto opened = nylon::database::database_t::open(argv[2]);
	if (!opened.ok()) { std::cerr << opened.error().message; return 1; }
	auto database = std::move(opened).value();
	const bool verify_only = std::wstring_view(argv[1]) == L"--verify";
	auto status = verify_only ? nylon::status_t::success() : database.initialize();
	nylon::media::prepared_input_t input;
	input.image_root = argv[1];
	input.source_kind = "expanded";
	if (!verify_only && status.ok()) status = nylon::evidence::initialize(database, input);
	if (!verify_only && status.ok()) status = nylon::evidence::inventory(database, argv[1]);
	if (status.ok()) status = nylon::evidence::verify(database);
	if (status.ok()) status = nylon::database::quick_check(database);
	if (!status.ok()) { std::cerr << status.error().message; return 1; }
	return 0;
}
