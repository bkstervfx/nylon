#include <iostream>
#include <string_view>

#include "services/control_sets.hpp"

int wmain(int argc, wchar_t** argv)
{
	if (argc != 3) return 2;
	auto opened = nylon::database::database_t::open(argv[2]);
	if (!opened.ok()) { std::cerr << opened.error().message; return 1; }
	auto database = std::move(opened).value();
	auto status = nylon::status_t::success();
	if (std::wstring_view(argv[1]) == L"--collect")
	{
		status = database.begin();
		if (status.ok()) status = nylon::services::collect_control_sets(database);
		if (status.ok()) status = database.commit();
		if (!status.ok()) database.rollback();
	}
	if (status.ok()) status = nylon::services::verify_control_sets(database);
	if (!status.ok()) { std::cerr << status.error().message; return 1; }
	return 0;
}
