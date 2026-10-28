#include "process.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "win32.hpp"

namespace nylon::platform
{

namespace
{

std::wstring quote_argument(std::wstring_view argument)
{
	const bool needs_quotes = argument.empty() ||
		argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;

	if (!needs_quotes)
	{
		return std::wstring(argument);
	}

	std::wstring output;
	output.reserve(argument.size() + 2U);
	output.push_back(L'\"');

	std::size_t backslashes = 0;
	for (const wchar_t character : argument)
	{
		if (character == L'\\')
		{
			++backslashes;
			continue;
		}

		if (character == L'\"')
		{
			output.append(backslashes * 2U + 1U, L'\\');
			output.push_back(L'\"');
			backslashes = 0;
			continue;
		}

		output.append(backslashes, L'\\');
		backslashes = 0;
		output.push_back(character);
	}

	output.append(backslashes * 2U, L'\\');
	output.push_back(L'\"');
	return output;
}

result_t<std::wstring> absolute_process_path(const std::filesystem::path& path)
{
	std::error_code error;
	auto absolute = std::filesystem::absolute(path, error);
	if (error)
	{
		return result_t<std::wstring>::failure(
			"Unable to resolve subprocess path",
			static_cast<std::uint32_t>(error.value()));
	}

	return result_t<std::wstring>::success(absolute.lexically_normal().native());
}

}

result_t<std::uint32_t> run_process(
	const std::filesystem::path& application,
	std::span<const std::wstring> arguments,
	const std::filesystem::path& working_directory)
{
	auto application_native = native_path(application);
	if (!application_native.ok())
	{
		return result_t<std::uint32_t>::failure(
			application_native.error().message,
			application_native.error().code);
	}

	auto display_application = absolute_process_path(application);
	if (!display_application.ok())
	{
		return result_t<std::uint32_t>::failure(
			display_application.error().message,
			display_application.error().code);
	}

	std::wstring command_line = quote_argument(display_application.value());
	for (const auto& argument : arguments)
	{
		command_line.push_back(L' ');
		command_line.append(quote_argument(argument));
	}

	std::vector<wchar_t> mutable_command_line(
		command_line.begin(),
		command_line.end());
	mutable_command_line.push_back(L'\0');

	std::wstring working_directory_storage;
	const wchar_t* working_directory_pointer = nullptr;
	if (!working_directory.empty())
	{
		auto absolute_working = absolute_process_path(working_directory);
		if (!absolute_working.ok())
		{
			return result_t<std::uint32_t>::failure(
				absolute_working.error().message,
				absolute_working.error().code);
		}
		working_directory_storage = std::move(absolute_working).value();
		working_directory_pointer = working_directory_storage.c_str();
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process_info{};

	if (!CreateProcessW(
		application_native.value().c_str(),
		mutable_command_line.data(),
		nullptr,
		nullptr,
		FALSE,
		0,
		nullptr,
		working_directory_pointer,
		&startup,
		&process_info))
	{
		const auto error = win32_error("CreateProcessW failed");
		return result_t<std::uint32_t>::failure(error.message, error.code);
	}

	unique_handle_t process(process_info.hProcess);
	unique_handle_t thread(process_info.hThread);

	const DWORD wait_result = WaitForSingleObject(process.get(), INFINITE);
	if (wait_result != WAIT_OBJECT_0)
	{
		const auto error = wait_result == WAIT_FAILED
			? win32_error("WaitForSingleObject failed for subprocess")
			: error_t{ "Unexpected subprocess wait result", wait_result };
		return result_t<std::uint32_t>::failure(error.message, error.code);
	}

	DWORD exit_code = 0;
	if (!GetExitCodeProcess(process.get(), &exit_code))
	{
		const auto error = win32_error("GetExitCodeProcess failed");
		return result_t<std::uint32_t>::failure(error.message, error.code);
	}

	return result_t<std::uint32_t>::success(exit_code);
}

}
