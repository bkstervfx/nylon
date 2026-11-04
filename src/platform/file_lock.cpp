#include "win32.hpp"

namespace nylon::platform
{

result_t<unique_handle_t> open_directory_read_lock(const std::filesystem::path& path, bool allow_child_writes)
{
	auto native = native_path(path);
	if (!native.ok()) return result_t<unique_handle_t>::failure(native.error().message, native.error().code);
	unique_handle_t handle(CreateFileW(native.value().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | (allow_child_writes ? FILE_SHARE_WRITE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (!handle) return result_t<unique_handle_t>::failure("Cannot guard source directory", GetLastError());
	BY_HANDLE_FILE_INFORMATION information{};
	if (!GetFileInformationByHandle(handle.get(), &information))
		return result_t<unique_handle_t>::failure("Cannot inspect guarded directory", GetLastError());
	if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
		(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return result_t<unique_handle_t>::failure("Source path traverses a reparse/non-directory entry", ERROR_ACCESS_DENIED);
	return result_t<unique_handle_t>::success(std::move(handle));
}

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
