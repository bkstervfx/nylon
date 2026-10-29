#include "win32.hpp"

#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace nylon::platform
{

unique_handle_t::unique_handle_t(HANDLE handle) noexcept
	: handle_(handle)
{
}

unique_handle_t::~unique_handle_t()
{
	reset();
}

unique_handle_t::unique_handle_t(unique_handle_t&& other) noexcept
	: handle_(other.release())
{
}

unique_handle_t& unique_handle_t::operator=(unique_handle_t&& other) noexcept
{
	if (this != &other)
	{
		reset(other.release());
	}
	return *this;
}

HANDLE unique_handle_t::get() const noexcept
{
	return handle_;
}

unique_handle_t::operator bool() const noexcept
{
	return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

HANDLE unique_handle_t::release() noexcept
{
	const HANDLE handle = handle_;
	handle_ = INVALID_HANDLE_VALUE;
	return handle;
}

void unique_handle_t::reset(HANDLE handle) noexcept
{
	if (*this)
	{
		CloseHandle(handle_);
	}
	handle_ = handle;
}

mapped_file_t::mapped_file_t(
	unique_handle_t file,
	unique_handle_t mapping,
	const std::byte* view,
	std::size_t size) noexcept
	: file_(std::move(file)),
	  mapping_(std::move(mapping)),
	  view_(view),
	  size_(size)
{
}

mapped_file_t::~mapped_file_t()
{
	reset();
}

mapped_file_t::mapped_file_t(mapped_file_t&& other) noexcept
	: file_(std::move(other.file_)),
	  mapping_(std::move(other.mapping_)),
	  view_(std::exchange(other.view_, nullptr)),
	  size_(std::exchange(other.size_, 0))
{
}

mapped_file_t& mapped_file_t::operator=(mapped_file_t&& other) noexcept
{
	if (this != &other)
	{
		reset();
		file_ = std::move(other.file_);
		mapping_ = std::move(other.mapping_);
		view_ = std::exchange(other.view_, nullptr);
		size_ = std::exchange(other.size_, 0);
	}
	return *this;
}

result_t<mapped_file_t> mapped_file_t::open_readonly(const std::filesystem::path& path)
{
	auto native = native_path(path);
	if (!native.ok())
	{
		return result_t<mapped_file_t>::failure(native.error().message, native.error().code);
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
		const auto error = win32_error("CreateFileW failed while opening a read-only mapping");
		return result_t<mapped_file_t>::failure(error.message, error.code);
	}

	FILE_ATTRIBUTE_TAG_INFO tag_info{};
	if (!GetFileInformationByHandleEx(
		file.get(),
		FileAttributeTagInfo,
		&tag_info,
		static_cast<DWORD>(sizeof(tag_info))))
	{
		const auto error = win32_error("GetFileInformationByHandleEx failed for read-only mapping");
		return result_t<mapped_file_t>::failure(error.message, error.code);
	}
	if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		return result_t<mapped_file_t>::failure(
			"Refusing to follow a reparse point while reading an offline image",
			ERROR_ACCESS_DENIED);
	}

	LARGE_INTEGER size{};
	if (!GetFileSizeEx(file.get(), &size))
	{
		const auto error = win32_error("GetFileSizeEx failed");
		return result_t<mapped_file_t>::failure(error.message, error.code);
	}

	if (size.QuadPart <= 0)
	{
		return result_t<mapped_file_t>::failure("Cannot map an empty file", ERROR_BAD_LENGTH);
	}

	if (static_cast<unsigned long long>(size.QuadPart) >
		static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
	{
		return result_t<mapped_file_t>::failure("File is too large for this process", ERROR_FILE_TOO_LARGE);
	}

	unique_handle_t mapping(CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
	if (!mapping)
	{
		const auto error = win32_error("CreateFileMappingW failed");
		return result_t<mapped_file_t>::failure(error.message, error.code);
	}

	const void* view = MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0);
	if (view == nullptr)
	{
		const auto error = win32_error("MapViewOfFile failed");
		return result_t<mapped_file_t>::failure(error.message, error.code);
	}

	return result_t<mapped_file_t>::success(mapped_file_t(
		std::move(file),
		std::move(mapping),
		static_cast<const std::byte*>(view),
		static_cast<std::size_t>(size.QuadPart)));
}

std::span<const std::byte> mapped_file_t::bytes() const noexcept
{
	return { view_, size_ };
}

bool mapped_file_t::empty() const noexcept
{
	return size_ == 0;
}

void mapped_file_t::reset() noexcept
{
	if (view_ != nullptr)
	{
		UnmapViewOfFile(view_);
		view_ = nullptr;
	}
	size_ = 0;
	mapping_.reset();
	file_.reset();
}

error_t win32_error(std::string context, std::uint32_t code)
{
	return error_t{ std::move(context), code };
}

result_t<std::string> utf8(std::wstring_view value)
{
	if (value.empty())
	{
		return result_t<std::string>::success({});
	}
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return result_t<std::string>::failure("UTF-16 input is too large");
	}

	const int input_size = static_cast<int>(value.size());
	const int count = WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0, nullptr, nullptr);
	if (count <= 0)
	{
		const auto error = win32_error("WideCharToMultiByte failed");
		return result_t<std::string>::failure(error.message, error.code);
	}

	std::string output(static_cast<std::size_t>(count), '\0');
	const int written = WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, output.data(), count, nullptr, nullptr);
	if (written != count)
	{
		const auto error = win32_error("WideCharToMultiByte returned an incomplete result");
		return result_t<std::string>::failure(error.message, error.code);
	}
	return result_t<std::string>::success(std::move(output));
}

result_t<std::wstring> wide(std::string_view value)
{
	if (value.empty())
	{
		return result_t<std::wstring>::success({});
	}
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return result_t<std::wstring>::failure("UTF-8 input is too large");
	}

	const int input_size = static_cast<int>(value.size());
	const int count = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
	if (count <= 0)
	{
		const auto error = win32_error("MultiByteToWideChar failed");
		return result_t<std::wstring>::failure(error.message, error.code);
	}

	std::wstring output(static_cast<std::size_t>(count), L'\0');
	const int written = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, output.data(), count);
	if (written != count)
	{
		const auto error = win32_error("MultiByteToWideChar returned an incomplete result");
		return result_t<std::wstring>::failure(error.message, error.code);
	}
	return result_t<std::wstring>::success(std::move(output));
}

result_t<std::filesystem::path> executable_path()
{
	std::vector<wchar_t> buffer(512);
	for (;;)
	{
		if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max()))
		{
			return result_t<std::filesystem::path>::failure("Executable path buffer exceeds Win32 limits");
		}

		SetLastError(ERROR_SUCCESS);
		const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (written == 0)
		{
			const auto error = win32_error("GetModuleFileNameW failed");
			return result_t<std::filesystem::path>::failure(error.message, error.code);
		}
		if (written < buffer.size())
		{
			return result_t<std::filesystem::path>::success(
				std::filesystem::path(std::wstring(buffer.data(), written)));
		}
		if (buffer.size() >= 32768U)
		{
			return result_t<std::filesystem::path>::failure("Executable path exceeds Windows path limits");
		}
		buffer.resize(buffer.size() * 2U);
	}
}

result_t<std::wstring> native_path(const std::filesystem::path& path)
{
	std::error_code error;
	auto absolute = std::filesystem::absolute(path, error);
	if (error)
	{
		return result_t<std::wstring>::failure(
			"Unable to resolve Win32 path", static_cast<std::uint32_t>(error.value()));
	}

	std::wstring value = absolute.lexically_normal().native();
	if (value.starts_with(L"\\\\?\\"))
	{
		return result_t<std::wstring>::success(std::move(value));
	}
	if (value.starts_with(L"\\\\"))
	{
		value = L"\\\\?\\UNC\\" + value.substr(2);
	}
	else
	{
		value = L"\\\\?\\" + value;
	}
	return result_t<std::wstring>::success(std::move(value));
}

result_t<DWORD> file_attributes(const std::filesystem::path& path)
{
	auto native = native_path(path);
	if (!native.ok())
	{
		return result_t<DWORD>::failure(native.error().message, native.error().code);
	}
	const DWORD attributes = GetFileAttributesW(native.value().c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		const auto error = win32_error("GetFileAttributesW failed");
		return result_t<DWORD>::failure(error.message, error.code);
	}
	return result_t<DWORD>::success(attributes);
}

}
