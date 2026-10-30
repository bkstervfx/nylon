#include <filesystem>
#include <fstream>
#include <string_view>

int wmain(int argc, wchar_t** argv)
{
	const auto executable = std::filesystem::path(argv[0]);
	if (executable.filename() == L"7z.exe")
	{
		if (argc != 5 || std::wstring_view(argv[1]) != L"x" || std::wstring_view(argv[2]) != L"-y" || !std::wstring_view(argv[3]).starts_with(L"-o")) return 2;
		const auto media = std::filesystem::path(std::wstring_view(argv[3]).substr(2));
		std::filesystem::create_directories(media / L"sources");
		std::ofstream(media / L"sources" / L"install.wim") << "synthetic container; not a WIM";
		return 0;
	}
	if (argc < 2) return 2;
	std::ofstream(executable.parent_path() / L"activity.log", std::ios::app) << (std::wstring_view(argv[1]) == L"info" ? "info\n" : "apply\n");
	if (std::wstring_view(argv[1]) == L"info")
	{
		if (argc != 4 || !std::wstring_view(argv[3]).starts_with(L"--extract-xml=")) return 2;
		std::filesystem::copy_file(executable.parent_path() / L"catalog-fixture.xml", std::filesystem::path(std::wstring_view(argv[3]).substr(14)));
		return 0;
	}
	if (std::wstring_view(argv[1]) != L"apply" || argc != 8 || std::wstring_view(argv[3]) != L"2"
		|| std::wstring_view(argv[5]) != L"--no-acls" || std::wstring_view(argv[6]) != L"--no-attributes" || std::wstring_view(argv[7]) != L"--rpfix") return 2;
	const auto config = std::filesystem::path(argv[4]) / L"Windows" / L"System32" / L"config";
	std::filesystem::create_directories(config);
	std::ofstream(config / L"SYSTEM") << "synthetic hive; not parsed";
	std::ofstream(config / L"SOFTWARE") << "synthetic hive; not parsed";
	return 0;
}
