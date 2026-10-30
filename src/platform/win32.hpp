#ifndef NYLON_PLATFORM_WIN32_HPP
#define NYLON_PLATFORM_WIN32_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "../result.hpp"

namespace nylon::platform
{

class unique_handle_t
{
public:
	unique_handle_t() noexcept = default;
	explicit unique_handle_t(HANDLE handle) noexcept;
	~unique_handle_t();

	unique_handle_t(const unique_handle_t&) = delete;
	unique_handle_t& operator=(const unique_handle_t&) = delete;
	unique_handle_t(unique_handle_t&& other) noexcept;
	unique_handle_t& operator=(unique_handle_t&& other) noexcept;

	[[nodiscard]] HANDLE get() const noexcept;
	[[nodiscard]] explicit operator bool() const noexcept;
	[[nodiscard]] HANDLE release() noexcept;
	void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept;

private:
	HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class mapped_file_t
{
public:
	mapped_file_t() noexcept = default;
	~mapped_file_t();

	mapped_file_t(const mapped_file_t&) = delete;
	mapped_file_t& operator=(const mapped_file_t&) = delete;
	mapped_file_t(mapped_file_t&& other) noexcept;
	mapped_file_t& operator=(mapped_file_t&& other) noexcept;

	static result_t<mapped_file_t> open_readonly(const std::filesystem::path& path);

	[[nodiscard]] std::span<const std::byte> bytes() const noexcept;
	[[nodiscard]] bool empty() const noexcept;

private:
	mapped_file_t(
		unique_handle_t file,
		unique_handle_t mapping,
		const std::byte* view,
		std::size_t size) noexcept;

	void reset() noexcept;

	unique_handle_t file_;
	unique_handle_t mapping_;
	const std::byte* view_ = nullptr;
	std::size_t size_ = 0;
};

[[nodiscard]] error_t win32_error(std::string context, std::uint32_t code = GetLastError());
[[nodiscard]] result_t<std::string> utf8(std::wstring_view value);
[[nodiscard]] result_t<std::string> path_comparison_key(std::wstring_view value);
[[nodiscard]] result_t<std::wstring> wide(std::string_view value);
[[nodiscard]] result_t<std::filesystem::path> executable_path();
[[nodiscard]] result_t<std::wstring> native_path(const std::filesystem::path& path);
[[nodiscard]] result_t<DWORD> file_attributes(const std::filesystem::path& path);
[[nodiscard]] result_t<unique_handle_t> open_read_lock(const std::filesystem::path& path);
[[nodiscard]] result_t<unique_handle_t> open_directory_read_lock(const std::filesystem::path& path, bool allow_child_writes = false);

}

#endif
