#ifndef NYLON_IMAGE_IMAGE_HPP
#define NYLON_IMAGE_IMAGE_HPP

#include <filesystem>
#include <string>
#include <vector>

#include "../platform/win32.hpp"

#include "../result.hpp"

namespace nylon::image
{

struct guarded_file_t
{
	std::vector<platform::unique_handle_t> directories;
	platform::unique_handle_t file;
};

class image_t
{
public:
	static result_t<image_t> open(std::filesystem::path root);

	[[nodiscard]] const std::filesystem::path& root() const noexcept;
	[[nodiscard]] result_t<std::filesystem::path> resolve(const std::filesystem::path& relative) const;
	[[nodiscard]] result_t<std::string> relative_utf8(const std::filesystem::path& absolute) const;
	[[nodiscard]] result_t<std::string> utf8_path(const std::filesystem::path& path) const;
	[[nodiscard]] result_t<guarded_file_t> open_file(const std::filesystem::path& relative) const;

private:
	explicit image_t(std::filesystem::path root);

	std::filesystem::path root_;
};

}

#endif
