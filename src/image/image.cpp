#include "image.hpp"

#include <system_error>
#include <utility>

#include "../platform/win32.hpp"

namespace nylon::image
{

namespace
{

bool escapes_root(const std::filesystem::path& relative)
{
	if (relative.empty())
	{
		return false;
	}

	for (const auto& component : relative)
	{
		if (component == L"..")
		{
			return true;
		}
	}

	return false;
}

status_t reject_reparse_components(
	const std::filesystem::path& root,
	const std::filesystem::path& relative)
{
	std::filesystem::path current = root;
	for (const auto& component : relative)
	{
		if (component.empty() || component == L".")
		{
			continue;
		}

		current /= component;
		auto attributes = platform::file_attributes(current);
		if (!attributes.ok())
		{
			if (attributes.error().code == ERROR_FILE_NOT_FOUND ||
				attributes.error().code == ERROR_PATH_NOT_FOUND)
			{
				break;
			}
			return status_t::failure(attributes.error().message, attributes.error().code);
		}

		if ((attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			return status_t::failure(
				"Image-relative path traverses a reparse point",
				ERROR_ACCESS_DENIED);
		}
	}

	return status_t::success();
}

}

image_t::image_t(std::filesystem::path root)
	: root_(std::move(root))
{
}

result_t<image_t> image_t::open(std::filesystem::path root)
{
	std::error_code error;
	root = std::filesystem::absolute(root, error);
	if (error)
	{
		return result_t<image_t>::failure("Unable to resolve image root", error.value());
	}
	root = root.lexically_normal();

	if (!std::filesystem::is_directory(root, error) || error)
	{
		return result_t<image_t>::failure("Image root is not a readable directory", error.value());
	}

	auto root_attributes = platform::file_attributes(root);
	if (!root_attributes.ok())
	{
		return result_t<image_t>::failure(root_attributes.error().message, root_attributes.error().code);
	}
	if ((root_attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		return result_t<image_t>::failure("Image root must not be a reparse point", ERROR_ACCESS_DENIED);
	}

	image_t image(root);
	auto kernel_result = image.resolve(L"Windows\\System32\\ntoskrnl.exe");
	if (!kernel_result.ok())
	{
		return result_t<image_t>::failure(kernel_result.error().message, kernel_result.error().code);
	}
	error.clear();
	if (!std::filesystem::is_regular_file(kernel_result.value(), error) || error)
	{
		return result_t<image_t>::failure("Image root does not contain Windows\\System32\\ntoskrnl.exe");
	}


	return result_t<image_t>::success(std::move(image));
}

const std::filesystem::path& image_t::root() const noexcept
{
	return root_;
}

result_t<guarded_file_t> image_t::open_file(const std::filesystem::path& relative) const
{
	if (relative.empty() || relative.has_root_path() || escapes_root(relative))
		return result_t<guarded_file_t>::failure("Invalid image-relative file path", ERROR_INVALID_PARAMETER);
	guarded_file_t guarded;
	auto directory = root_;
	std::vector<std::filesystem::path> chain{ directory };
	for (const auto& component : relative.parent_path())
	{
		directory /= component;
		chain.push_back(directory);
	}
	for (const auto& candidate : chain)
	{
		auto handle = platform::open_directory_read_lock(candidate);
		if (!handle.ok()) return result_t<guarded_file_t>::failure(handle.error().message, handle.error().code);
		guarded.directories.push_back(std::move(handle).value());
	}
	auto lock = platform::open_read_lock(root_ / relative);
	if (!lock.ok()) return result_t<guarded_file_t>::failure(lock.error().message, lock.error().code);
	guarded.file = std::move(lock).value();
	return result_t<guarded_file_t>::success(std::move(guarded));
}

result_t<std::filesystem::path> image_t::resolve(const std::filesystem::path& relative) const
{
	if (relative.empty())
	{
		return result_t<std::filesystem::path>::success(root_);
	}
	if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
	{
		return result_t<std::filesystem::path>::failure("Image-relative path must not be absolute");
	}

	const auto normalized = relative.lexically_normal();
	if (escapes_root(normalized))
	{
		return result_t<std::filesystem::path>::failure("Image-relative path escapes the image root");
	}

	auto reparse_status = reject_reparse_components(root_, normalized);
	if (!reparse_status.ok())
	{
		return result_t<std::filesystem::path>::failure(
			reparse_status.error().message,
			reparse_status.error().code);
	}

	return result_t<std::filesystem::path>::success((root_ / normalized).lexically_normal());
}

result_t<std::string> image_t::relative_utf8(const std::filesystem::path& absolute) const
{
	const auto normalized = absolute.lexically_normal();
	const auto relative = normalized.lexically_relative(root_);
	if (relative.empty() && normalized != root_)
	{
		return result_t<std::string>::failure("Path is not below the image root");
	}
	if (escapes_root(relative))
	{
		return result_t<std::string>::failure("Path is outside the image root");
	}
	return platform::utf8(relative.generic_wstring());
}

result_t<std::string> image_t::utf8_path(const std::filesystem::path& path) const
{
	return platform::utf8(path.generic_wstring());
}

}
