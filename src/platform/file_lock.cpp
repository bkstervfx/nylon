#include "win32.hpp"

namespace nylon::platform
{

result_t<unique_handle_t> open_read_lock(const std::filesystem::path& path)
{
	auto native = native_path(path);
	if (!native.ok())
	{
		return result_t<unique_handle_t>::failure(native.error().message, native.error().code);
	}

	unique_handle_t file(CreateFileW(
		native.value().c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
	if (!file)
	{
		const auto error = win32_error("Unable to acquire stable read lock for input file");
		return result_t<unique_handle_t>::failure(error.message, error.code);
	}

	FILE_ATTRIBUTE_TAG_INFO tag_info{};
	if (!GetFileInformationByHandleEx(
		file.get(),
		FileAttributeTagInfo,
		&tag_info,
		static_cast<DWORD>(sizeof(tag_info))))
	{
		const auto error = win32_error("Unable to inspect stable read lock input");
		return result_t<unique_handle_t>::failure(error.message, error.code);
	}
	if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		return result_t<unique_handle_t>::failure(
			"ISO input must not be a reparse point",
			ERROR_ACCESS_DENIED);
	}
	if ((tag_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		return result_t<unique_handle_t>::failure(
			"ISO input must be a regular file",
			ERROR_INVALID_PARAMETER);
	}

	return result_t<unique_handle_t>::success(std::move(file));
}

}
