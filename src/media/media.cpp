#include "media.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../platform/hash.hpp"
#include "../platform/process.hpp"
#include "../platform/win32.hpp"

namespace nylon::media
{

namespace
{

constexpr std::size_t max_catalog_bytes = 64U * 1024U * 1024U;

struct catalog_image_t
{
	std::uint32_t index = 0;
	std::wstring name;
};

struct install_image_t
{
	std::filesystem::path primary;
	bool split = false;
	std::filesystem::path reference_glob;
};

class find_handle_t
{
public:
	find_handle_t() noexcept = default;
	explicit find_handle_t(HANDLE handle) noexcept
		: handle_(handle)
	{
	}

	~find_handle_t()
	{
		if (handle_ != INVALID_HANDLE_VALUE)
		{
			FindClose(handle_);
		}
	}

	find_handle_t(const find_handle_t&) = delete;
	find_handle_t& operator=(const find_handle_t&) = delete;

	[[nodiscard]] HANDLE get() const noexcept
	{
		return handle_;
	}

private:
	HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool equal_ordinal_ignore_case(std::wstring_view left, std::wstring_view right)
{
	if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
		right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return false;
	}

	return CompareStringOrdinal(
		left.data(),
		static_cast<int>(left.size()),
		right.data(),
		static_cast<int>(right.size()),
		TRUE) == CSTR_EQUAL;
}

bool has_iso_extension(const std::filesystem::path& path)
{
	return equal_ordinal_ignore_case(path.extension().native(), L".iso");
}

result_t<std::filesystem::path> absolute_path(const std::filesystem::path& path)
{
	std::error_code error;
	auto absolute = std::filesystem::absolute(path, error);
	if (error)
	{
		return result_t<std::filesystem::path>::failure(
			"Unable to resolve input path",
			static_cast<std::uint32_t>(error.value()));
	}
	return result_t<std::filesystem::path>::success(absolute.lexically_normal());
}

bool regular_file_exists(const std::filesystem::path& path)
{
	std::error_code error;
	const bool result = std::filesystem::is_regular_file(path, error);
	return !error && result;
}

result_t<std::vector<std::byte>> read_file_bytes(
	const std::filesystem::path& path,
	std::size_t maximum)
{
	std::error_code size_error;
	const auto size = std::filesystem::file_size(path, size_error);
	if (size_error)
	{
		return result_t<std::vector<std::byte>>::failure(
			"Unable to read catalog size",
			static_cast<std::uint32_t>(size_error.value()));
	}

	if (size > maximum || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
	{
		return result_t<std::vector<std::byte>>::failure("WIM XML catalog is unexpectedly large");
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream)
	{
		return result_t<std::vector<std::byte>>::failure("Unable to open WIM XML catalog");
	}

	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty())
	{
		stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size()))
		{
			return result_t<std::vector<std::byte>>::failure("Unable to read complete WIM XML catalog");
		}
	}

	return result_t<std::vector<std::byte>>::success(std::move(bytes));
}

result_t<std::wstring> decode_catalog(const std::filesystem::path& path)
{
	auto bytes_result = read_file_bytes(path, max_catalog_bytes);
	if (!bytes_result.ok())
	{
		return result_t<std::wstring>::failure(
			bytes_result.error().message,
			bytes_result.error().code);
	}

	const auto& bytes = bytes_result.value();
	if (bytes.empty())
	{
		return result_t<std::wstring>::failure("WIM XML catalog is empty");
	}

	auto byte_at = [&bytes](std::size_t index) -> unsigned char
	{
		return std::to_integer<unsigned char>(bytes[index]);
	};

	if (bytes.size() >= 2U &&
		((byte_at(0) == 0xFFU && byte_at(1) == 0xFEU) ||
		 (byte_at(0) == 0xFEU && byte_at(1) == 0xFFU)))
	{
		const bool little_endian = byte_at(0) == 0xFFU;
		if (((bytes.size() - 2U) & 1U) != 0)
		{
			return result_t<std::wstring>::failure("WIM XML UTF-16 catalog has an odd byte length");
		}

		std::wstring text;
		text.reserve((bytes.size() - 2U) / 2U);
		for (std::size_t index = 2U; index + 1U < bytes.size(); index += 2U)
		{
			const std::uint16_t first = byte_at(index);
			const std::uint16_t second = byte_at(index + 1U);
			const std::uint16_t code_unit = little_endian
				? static_cast<std::uint16_t>(first | (second << 8U))
				: static_cast<std::uint16_t>((first << 8U) | second);
			text.push_back(static_cast<wchar_t>(code_unit));
		}
		return result_t<std::wstring>::success(std::move(text));
	}

	std::size_t offset = 0;
	if (bytes.size() >= 3U && byte_at(0) == 0xEFU && byte_at(1) == 0xBBU && byte_at(2) == 0xBFU)
	{
		offset = 3U;
	}

	std::string utf8;
	utf8.resize(bytes.size() - offset);
	for (std::size_t index = offset; index < bytes.size(); ++index)
	{
		utf8[index - offset] = static_cast<char>(byte_at(index));
	}

	auto wide = platform::wide(utf8);
	if (!wide.ok())
	{
		return result_t<std::wstring>::failure(wide.error().message, wide.error().code);
	}
	return wide;
}

std::wstring xml_unescape(std::wstring_view value)
{
	std::wstring result;
	result.reserve(value.size());

	for (std::size_t index = 0; index < value.size();)
	{
		struct entity_t
		{
			std::wstring_view encoded;
			wchar_t decoded;
		};
		static constexpr entity_t entities[] = {
			{ L"&amp;", L'&' },
			{ L"&lt;", L'<' },
			{ L"&gt;", L'>' },
			{ L"&quot;", L'\"' },
			{ L"&apos;", L'\'' }
		};

		bool matched = false;
		for (const auto& entity : entities)
		{
			if (value.substr(index).starts_with(entity.encoded))
			{
				result.push_back(entity.decoded);
				index += entity.encoded.size();
				matched = true;
				break;
			}
		}

		if (!matched)
		{
			result.push_back(value[index]);
			++index;
		}
	}

	return result;
}

std::optional<std::wstring> element_text(
	std::wstring_view block,
	std::wstring_view element)
{
	std::wstring start = L"<";
	start.append(element);
	start.push_back(L'>');
	std::wstring end = L"</";
	end.append(element);
	end.push_back(L'>');

	const std::size_t begin = block.find(start);
	if (begin == std::wstring_view::npos)
	{
		return std::nullopt;
	}

	const std::size_t content_begin = begin + start.size();
	const std::size_t content_end = block.find(end, content_begin);
	if (content_end == std::wstring_view::npos)
	{
		return std::nullopt;
	}

	return xml_unescape(block.substr(content_begin, content_end - content_begin));
}

std::optional<std::uint32_t> parse_image_index(std::wstring_view opening_tag)
{
	std::size_t marker = opening_tag.find(L"INDEX=\"");
	wchar_t quote = L'\"';
	if (marker == std::wstring_view::npos)
	{
		marker = opening_tag.find(L"INDEX='");
		quote = L'\'';
	}
	if (marker == std::wstring_view::npos)
	{
		return std::nullopt;
	}

	marker += 7U;
	std::uint64_t value = 0;
	std::size_t digits = 0;
	while (marker < opening_tag.size() && opening_tag[marker] != quote)
	{
		const wchar_t character = opening_tag[marker];
		if (character < L'0' || character > L'9')
		{
			return std::nullopt;
		}
		value = value * 10U + static_cast<std::uint64_t>(character - L'0');
		if (value > std::numeric_limits<std::uint32_t>::max())
		{
			return std::nullopt;
		}
		++digits;
		++marker;
	}

	if (digits == 0U || marker >= opening_tag.size() || value == 0U)
	{
		return std::nullopt;
	}

	return static_cast<std::uint32_t>(value);
}

result_t<std::vector<catalog_image_t>> parse_catalog(const std::filesystem::path& path)
{
	auto text_result = decode_catalog(path);
	if (!text_result.ok())
	{
		return result_t<std::vector<catalog_image_t>>::failure(
			text_result.error().message,
			text_result.error().code);
	}

	const std::wstring& text = text_result.value();
	std::vector<catalog_image_t> images;
	std::size_t cursor = 0;

	while (cursor < text.size())
	{
		const std::size_t image_begin = text.find(L"<IMAGE", cursor);
		if (image_begin == std::wstring::npos)
		{
			break;
		}

		const std::size_t tag_end = text.find(L'>', image_begin);
		const std::size_t image_end = text.find(L"</IMAGE>", tag_end == std::wstring::npos ? image_begin : tag_end);
		if (tag_end == std::wstring::npos || image_end == std::wstring::npos)
		{
			return result_t<std::vector<catalog_image_t>>::failure("WIM XML contains a truncated IMAGE element");
		}

		const auto index = parse_image_index(std::wstring_view(text).substr(
			image_begin,
			tag_end - image_begin + 1U));
		if (!index.has_value())
		{
			return result_t<std::vector<catalog_image_t>>::failure("WIM XML IMAGE element has an invalid index");
		}

		std::wstring name = L"(unnamed)";
		const auto image_block = std::wstring_view(text).substr(
			tag_end + 1U,
			image_end - tag_end - 1U);
		if (auto parsed_name = element_text(image_block, L"NAME"); parsed_name.has_value() && !parsed_name->empty())
		{
			name = std::move(*parsed_name);
		}

		if (std::any_of(images.begin(), images.end(), [&](const catalog_image_t& existing)
			{ return existing.index == *index; }))
		{
			return result_t<std::vector<catalog_image_t>>::failure("WIM XML contains duplicate image indexes");
		}

		images.push_back(catalog_image_t{ *index, std::move(name) });
		cursor = image_end + 8U;
	}

	if (images.empty())
	{
		return result_t<std::vector<catalog_image_t>>::failure("No Windows images were found in install media");
	}

	std::sort(images.begin(), images.end(), [](const catalog_image_t& left, const catalog_image_t& right)
	{
		return left.index < right.index;
	});

	return result_t<std::vector<catalog_image_t>>::success(std::move(images));
}

std::string catalog_description(const std::vector<catalog_image_t>& images)
{
	std::ostringstream output;
	for (const auto& image : images)
	{
		auto name = platform::utf8(image.name);
		output << "\n  " << image.index << ": "
			<< (name.ok() ? name.value() : std::string("(name encoding error)"));
	}
	return output.str();
}

result_t<install_image_t> find_install_image(const std::filesystem::path& media_root)
{
	const auto sources = media_root / L"sources";
	const std::filesystem::path preferred[] = {
		sources / L"install.wim",
		sources / L"install.esd",
		sources / L"install.swm"
	};

	for (std::size_t index = 0; index < std::size(preferred); ++index)
	{
		if (regular_file_exists(preferred[index]))
		{
			return result_t<install_image_t>::success(install_image_t{
				preferred[index],
				index == 2U,
				index == 2U ? sources / L"install*.swm" : std::filesystem::path{}
			});
		}
	}

	std::error_code iterator_error;
	std::filesystem::directory_iterator iterator(sources, iterator_error);
	if (iterator_error)
	{
		return result_t<install_image_t>::failure(
			"ISO does not contain an accessible sources directory",
			static_cast<std::uint32_t>(iterator_error.value()));
	}

	std::vector<std::filesystem::path> wim_files;
	std::vector<std::filesystem::path> esd_files;
	std::vector<std::filesystem::path> swm_files;

	for (const auto& entry : iterator)
	{
		std::error_code type_error;
		if (!entry.is_regular_file(type_error) || type_error)
		{
			continue;
		}

		const auto filename = entry.path().filename().native();
		if (filename.size() < 7U || !equal_ordinal_ignore_case(filename.substr(0U, 7U), L"install"))
		{
			continue;
		}

		const auto extension = entry.path().extension().native();
		if (equal_ordinal_ignore_case(extension, L".wim"))
		{
			wim_files.push_back(entry.path());
		}
		else if (equal_ordinal_ignore_case(extension, L".esd"))
		{
			esd_files.push_back(entry.path());
		}
		else if (equal_ordinal_ignore_case(extension, L".swm"))
		{
			swm_files.push_back(entry.path());
		}
	}

	auto choose_first = [](std::vector<std::filesystem::path>& paths) -> std::optional<std::filesystem::path>
	{
		if (paths.empty()) return std::nullopt;
		std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right)
		{
			return left.filename().native() < right.filename().native();
		});
		return paths.front();
	};

	if (auto path = choose_first(wim_files); path.has_value())
	{
		return result_t<install_image_t>::success(install_image_t{ *path, false, {} });
	}
	if (auto path = choose_first(esd_files); path.has_value())
	{
		return result_t<install_image_t>::success(install_image_t{ *path, false, {} });
	}
	if (auto path = choose_first(swm_files); path.has_value())
	{
		return result_t<install_image_t>::success(install_image_t{
			*path,
			true,
			sources / L"install*.swm"
		});
	}

	return result_t<install_image_t>::failure("ISO does not contain install.wim, install.esd, or install*.swm");
}

status_t run_checked(
	const std::filesystem::path& application,
	std::vector<std::wstring> arguments,
	std::string_view tool_name)
{
	auto result = platform::run_process(application, arguments, application.parent_path());
	if (!result.ok())
	{
		return status_t::failure(result.error().message, result.error().code);
	}
	if (result.value() != 0U)
	{
		return status_t::failure(
			std::string(tool_name) + " exited with code " + std::to_string(result.value()),
			result.value());
	}
	return status_t::success();
}

std::string sanitize_manifest_value(std::string value)
{
	for (char& character : value)
	{
		if (character == '\r' || character == '\n')
		{
			character = ' ';
		}
	}
	return value;
}

status_t write_manifest(
	const std::filesystem::path& path,
	std::string_view iso_sha256,
	std::uint32_t image_index,
	std::size_t image_count,
	std::string image_name)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
	{
		return status_t::failure("Unable to create immutable cache manifest");
	}

	stream
		<< "format=1\n"
		<< "complete=1\n"
		<< "iso_sha256=" << iso_sha256 << '\n'
		<< "image_index=" << image_index << '\n'
		<< "image_count=" << image_count << '\n'
		<< "image_name=" << sanitize_manifest_value(std::move(image_name)) << '\n'
		<< "iso_extractor=7zip-26.05\n"
		<< "wimlib=1.14.5\n"
		<< "wim_apply_flags=--no-acls --no-attributes --rpfix\n"
		<< "immutable=1\n";
	stream.flush();

	if (!stream)
	{
		return status_t::failure("Unable to write complete immutable cache manifest");
	}
	return status_t::success();
}

result_t<std::string> read_small_text(const std::filesystem::path& path, std::size_t maximum = 1024U * 1024U)
{
	std::error_code size_error;
	const auto size = std::filesystem::file_size(path, size_error);
	if (size_error || size > maximum || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
	{
		return result_t<std::string>::failure("Unable to read cache manifest");
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream)
	{
		return result_t<std::string>::failure("Unable to open cache manifest");
	}

	std::string text(static_cast<std::size_t>(size), '\0');
	if (!text.empty())
	{
		stream.read(text.data(), static_cast<std::streamsize>(text.size()));
		if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size()))
		{
			return result_t<std::string>::failure("Unable to read complete cache manifest");
		}
	}
	return result_t<std::string>::success(std::move(text));
}

std::optional<std::string> manifest_value(std::string_view manifest, std::string_view key)
{
	std::string prefix(key);
	prefix.push_back('=');
	std::size_t cursor = 0;

	while (cursor <= manifest.size())
	{
		const std::size_t end = manifest.find('\n', cursor);
		const std::string_view line = manifest.substr(
			cursor,
			end == std::string_view::npos ? manifest.size() - cursor : end - cursor);
		if (line.starts_with(prefix))
		{
			std::string value(line.substr(prefix.size()));
			if (!value.empty() && value.back() == '\r') value.pop_back();
			return value;
		}
		if (end == std::string_view::npos) break;
		cursor = end + 1U;
	}
	return std::nullopt;
}

result_t<std::optional<std::string>> validate_cache_entry(
	const std::filesystem::path& entry,
	std::string_view iso_sha256,
	std::uint32_t image_index)
{
	std::error_code exists_error;
	const bool exists = std::filesystem::exists(entry, exists_error);
	if (exists_error)
	{
		return result_t<std::optional<std::string>>::failure(
			"Unable to inspect ISO cache entry",
			static_cast<std::uint32_t>(exists_error.value()));
	}
	if (!exists)
	{
		return result_t<std::optional<std::string>>::success(std::nullopt);
	}

	const auto manifest_path = entry / L"manifest.txt";
	const auto system_hive = entry / L"image" / L"Windows" / L"System32" / L"config" / L"SYSTEM";
	const auto software_hive = entry / L"image" / L"Windows" / L"System32" / L"config" / L"SOFTWARE";

	if (!regular_file_exists(manifest_path) ||
		!regular_file_exists(system_hive) ||
		!regular_file_exists(software_hive))
	{
		return result_t<std::optional<std::string>>::failure(
			"Existing immutable ISO cache entry is incomplete. Remove that cache entry before rebuilding it.");
	}

	auto manifest_result = read_small_text(manifest_path);
	if (!manifest_result.ok())
	{
		return result_t<std::optional<std::string>>::failure(
			manifest_result.error().message,
			manifest_result.error().code);
	}

	const auto& manifest = manifest_result.value();
	const auto complete = manifest_value(manifest, "complete");
	const auto hash = manifest_value(manifest, "iso_sha256");
	const auto index = manifest_value(manifest, "image_index");
	const auto immutable = manifest_value(manifest, "immutable");

	if (!complete.has_value() || *complete != "1" ||
		!hash.has_value() || *hash != iso_sha256 ||
		!index.has_value() || *index != std::to_string(image_index) ||
		!immutable.has_value() || *immutable != "1")
	{
		return result_t<std::optional<std::string>>::failure(
			"Existing immutable ISO cache entry failed manifest validation. Remove that cache entry before rebuilding it.");
	}

	return result_t<std::optional<std::string>>::success(
		manifest_value(manifest, "image_name").value_or(std::string{}));
}

status_t set_read_only_native(const std::wstring& path, bool enabled)
{
	const DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		const auto error = platform::win32_error("GetFileAttributesW failed while finalizing ISO cache");
		return status_t::failure(error.message, error.code);
	}

	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
	{
		std::wstring pattern = path;
		if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
		pattern.push_back(L'*');

		WIN32_FIND_DATAW data{};
		find_handle_t find(FindFirstFileExW(
			pattern.c_str(),
			FindExInfoBasic,
			&data,
			FindExSearchNameMatch,
			nullptr,
			FIND_FIRST_EX_LARGE_FETCH));

		if (find.get() == INVALID_HANDLE_VALUE)
		{
			const DWORD code = GetLastError();
			if (code != ERROR_FILE_NOT_FOUND)
			{
				return status_t::failure("FindFirstFileExW failed while finalizing ISO cache", code);
			}
		}
		else
		{
			for (;;)
			{
				const std::wstring_view name(data.cFileName);
				if (name != L"." && name != L"..")
				{
					std::wstring child = path;
					if (!child.empty() && child.back() != L'\\') child.push_back(L'\\');
					child.append(name);

					auto status = set_read_only_native(child, enabled);
					if (!status.ok()) return status;
				}

				if (!FindNextFileW(find.get(), &data))
				{
					const DWORD code = GetLastError();
					if (code != ERROR_NO_MORE_FILES)
					{
						return status_t::failure("FindNextFileW failed while finalizing ISO cache", code);
					}
					break;
				}
			}
		}
	}

	const DWORD requested = enabled ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
	if (!SetFileAttributesW(path.c_str(), requested))
	{
		const auto error = platform::win32_error("SetFileAttributesW failed while finalizing ISO cache");
		return status_t::failure(error.message, error.code);
	}

	return status_t::success();
}

status_t set_tree_read_only(const std::filesystem::path& root, bool enabled)
{
	auto native = platform::native_path(root);
	if (!native.ok())
	{
		return status_t::failure(native.error().message, native.error().code);
	}
	return set_read_only_native(native.value(), enabled);
}

void remove_tree_best_effort(const std::filesystem::path& path) noexcept
{
	if (path.empty()) return;

	std::error_code error;
	std::filesystem::remove_all(path, error);
	if (!error) return;

	(void)set_tree_read_only(path, false);
	error.clear();
	std::filesystem::remove_all(path, error);
}

class cleanup_directory_t
{
public:
	explicit cleanup_directory_t(std::filesystem::path path)
		: path_(std::move(path))
	{
	}

	~cleanup_directory_t()
	{
		if (active_) remove_tree_best_effort(path_);
	}

	cleanup_directory_t(const cleanup_directory_t&) = delete;
	cleanup_directory_t& operator=(const cleanup_directory_t&) = delete;

	void release() noexcept
	{
		active_ = false;
	}

private:
	std::filesystem::path path_;
	bool active_ = true;
};

result_t<std::filesystem::path> create_stage_directory(
	const std::filesystem::path& temp_root,
	std::string_view iso_sha256)
{
	auto hash_wide = platform::wide(iso_sha256.substr(0U, std::min<std::size_t>(16U, iso_sha256.size())));
	if (!hash_wide.ok())
	{
		return result_t<std::filesystem::path>::failure(hash_wide.error().message, hash_wide.error().code);
	}

	for (std::uint32_t attempt = 0; attempt < 64U; ++attempt)
	{
		std::wstring name = L"iso-";
		name.append(hash_wide.value());
		name.push_back(L'-');
		name.append(std::to_wstring(GetCurrentProcessId()));
		name.push_back(L'-');
		name.append(std::to_wstring(GetTickCount64()));
		name.push_back(L'-');
		name.append(std::to_wstring(attempt));

		const auto candidate = temp_root / name;
		std::error_code error;
		if (std::filesystem::create_directories(candidate, error))
		{
			return result_t<std::filesystem::path>::success(candidate);
		}
		if (error && error != std::errc::file_exists)
		{
			return result_t<std::filesystem::path>::failure(
				"Unable to create ISO staging directory",
				static_cast<std::uint32_t>(error.value()));
		}
	}

	return result_t<std::filesystem::path>::failure("Unable to allocate a unique ISO staging directory");
}

result_t<std::string> selected_name_utf8(
	const std::vector<catalog_image_t>& images,
	std::uint32_t image_index)
{
	const auto found = std::find_if(images.begin(), images.end(), [&](const catalog_image_t& image)
	{
		return image.index == image_index;
	});
	if (found == images.end())
	{
		return result_t<std::string>::failure("Selected image index was not found in WIM catalog");
	}

	auto name = platform::utf8(found->name);
	if (!name.ok())
	{
		return result_t<std::string>::failure(name.error().message, name.error().code);
	}
	return name;
}

result_t<std::uint32_t> choose_image_index(
	const std::vector<catalog_image_t>& images,
	const std::optional<std::uint32_t>& requested)
{
	if (requested.has_value())
	{
		const bool exists = std::any_of(images.begin(), images.end(), [&](const catalog_image_t& image)
		{
			return image.index == *requested;
		});
		if (!exists)
		{
			return result_t<std::uint32_t>::failure(
				"Requested image index is not present. Available images:" + catalog_description(images),
				ERROR_INVALID_PARAMETER);
		}
		return result_t<std::uint32_t>::success(*requested);
	}

	if (images.size() == 1U)
	{
		return result_t<std::uint32_t>::success(images.front().index);
	}

	return result_t<std::uint32_t>::failure(
		"ISO contains multiple Windows images. Re-run with --index <number>. Available images:" +
			catalog_description(images),
		ERROR_INVALID_PARAMETER);
}

std::filesystem::path cache_entry_path(
	const std::filesystem::path& cache_root,
	std::string_view iso_sha256,
	std::uint32_t image_index)
{
	return cache_root /
		L"images" /
		std::filesystem::path(std::string(iso_sha256)) /
		std::filesystem::path(L"index-" + std::to_wstring(image_index));
}

result_t<prepared_input_t> cached_result(
	const std::filesystem::path& entry,
	std::string iso_sha256,
	std::uint32_t image_index,
	std::string image_name,
	bool cache_hit)
{
	return result_t<prepared_input_t>::success(prepared_input_t{
		entry / L"image",
		"iso",
		std::move(iso_sha256),
		std::move(image_name),
		image_index,
		cache_hit
	});
}

}

result_t<prepared_input_t> prepare(
	std::filesystem::path input,
	const prepare_options_t& options)
{
	auto absolute_result = absolute_path(input);
	if (!absolute_result.ok())
	{
		return result_t<prepared_input_t>::failure(
			absolute_result.error().message,
			absolute_result.error().code);
	}
	input = std::move(absolute_result).value();

	std::error_code status_error;
	const auto input_status = std::filesystem::status(input, status_error);
	if (status_error)
	{
		return result_t<prepared_input_t>::failure(
			"Unable to inspect scan input",
			static_cast<std::uint32_t>(status_error.value()));
	}

	if (std::filesystem::is_directory(input_status))
	{
		return result_t<prepared_input_t>::success(prepared_input_t{
			std::move(input),
			"directory",
			{},
			{},
			0,
			false
		});
	}

	if (!std::filesystem::is_regular_file(input_status) || !has_iso_extension(input))
	{
		return result_t<prepared_input_t>::failure(
			"Scan input must be an expanded Windows image directory or a Windows .iso file",
			ERROR_INVALID_PARAMETER);
	}

	auto hash_result = platform::sha256_file(input);
	if (!hash_result.ok())
	{
		return result_t<prepared_input_t>::failure(hash_result.error().message, hash_result.error().code);
	}
	std::string iso_sha256 = std::move(hash_result).value();

	if (options.image_index.has_value())
	{
		const auto entry = cache_entry_path(options.cache_root, iso_sha256, *options.image_index);
		auto validation = validate_cache_entry(entry, iso_sha256, *options.image_index);
		if (!validation.ok())
		{
			return result_t<prepared_input_t>::failure(validation.error().message, validation.error().code);
		}
		if (validation.value().has_value())
		{
			return cached_result(
				entry,
				std::move(iso_sha256),
				*options.image_index,
				std::move(*validation.value()),
				true);
		}
	}

	const auto seven_zip = options.nylon_root / L"tools" / L"media" / L"7zip" / L"7z.exe";
	const auto wimlib = options.nylon_root / L"tools" / L"media" / L"wimlib" / L"wimlib-imagex.exe";
	if (!regular_file_exists(seven_zip))
	{
		return result_t<prepared_input_t>::failure(
			"Portable 7-Zip runtime is missing. Re-run build.cmd to bootstrap media tools.",
			ERROR_FILE_NOT_FOUND);
	}
	if (!regular_file_exists(wimlib))
	{
		return result_t<prepared_input_t>::failure(
			"Portable wimlib runtime is missing. Re-run build.cmd to bootstrap media tools.",
			ERROR_FILE_NOT_FOUND);
	}

	auto stage_result = create_stage_directory(options.temp_root, iso_sha256);
	if (!stage_result.ok())
	{
		return result_t<prepared_input_t>::failure(stage_result.error().message, stage_result.error().code);
	}
	const auto stage = stage_result.value();
	cleanup_directory_t cleanup(stage);

	const auto media_root = stage / L"media";
	std::error_code create_error;
	std::filesystem::create_directories(media_root, create_error);
	if (create_error)
	{
		return result_t<prepared_input_t>::failure(
			"Unable to create ISO extraction directory",
			static_cast<std::uint32_t>(create_error.value()));
	}

	auto extract_status = run_checked(
		seven_zip,
		{ L"x", L"-y", L"-o" + media_root.native(), input.native() },
		"7-Zip ISO extraction");
	if (!extract_status.ok())
	{
		return result_t<prepared_input_t>::failure(
			extract_status.error().message,
			extract_status.error().code);
	}

	auto install_result = find_install_image(media_root);
	if (!install_result.ok())
	{
		return result_t<prepared_input_t>::failure(
			install_result.error().message,
			install_result.error().code);
	}
	const auto install = install_result.value();

	const auto catalog_path = stage / L"catalog.xml";
	std::vector<std::wstring> info_arguments = {
		L"info",
		install.primary.native(),
		L"--extract-xml=" + catalog_path.native()
	};

	auto info_status = run_checked(wimlib, std::move(info_arguments), "wimlib image catalog");
	if (!info_status.ok())
	{
		return result_t<prepared_input_t>::failure(info_status.error().message, info_status.error().code);
	}

	auto catalog_result = parse_catalog(catalog_path);
	if (!catalog_result.ok())
	{
		return result_t<prepared_input_t>::failure(catalog_result.error().message, catalog_result.error().code);
	}
	const auto& images = catalog_result.value();

	auto index_result = choose_image_index(images, options.image_index);
	if (!index_result.ok())
	{
		return result_t<prepared_input_t>::failure(index_result.error().message, index_result.error().code);
	}
	const std::uint32_t image_index = index_result.value();

	auto name_result = selected_name_utf8(images, image_index);
	if (!name_result.ok())
	{
		return result_t<prepared_input_t>::failure(name_result.error().message, name_result.error().code);
	}
	std::string image_name = std::move(name_result).value();

	const auto final_entry = cache_entry_path(options.cache_root, iso_sha256, image_index);
	auto existing = validate_cache_entry(final_entry, iso_sha256, image_index);
	if (!existing.ok())
	{
		return result_t<prepared_input_t>::failure(existing.error().message, existing.error().code);
	}
	if (existing.value().has_value())
	{
		return cached_result(
			final_entry,
			std::move(iso_sha256),
			image_index,
			existing.value()->empty() ? std::move(image_name) : std::move(*existing.value()),
			true);
	}

	const auto entry_stage = stage / L"cache-entry";
	const auto image_root = entry_stage / L"image";
	std::filesystem::create_directories(image_root, create_error);
	if (create_error)
	{
		return result_t<prepared_input_t>::failure(
			"Unable to create image application directory",
			static_cast<std::uint32_t>(create_error.value()));
	}

	std::vector<std::wstring> apply_arguments = {
		L"apply",
		install.primary.native(),
		std::to_wstring(image_index),
		image_root.native(),
		L"--no-acls",
		L"--no-attributes",
		L"--rpfix"
	};
	if (install.split)
	{
		apply_arguments.push_back(L"--ref=" + install.reference_glob.native());
	}

	auto apply_status = run_checked(wimlib, std::move(apply_arguments), "wimlib image apply");
	if (!apply_status.ok())
	{
		return result_t<prepared_input_t>::failure(apply_status.error().message, apply_status.error().code);
	}

	const auto system_hive = image_root / L"Windows" / L"System32" / L"config" / L"SYSTEM";
	const auto software_hive = image_root / L"Windows" / L"System32" / L"config" / L"SOFTWARE";
	if (!regular_file_exists(system_hive) || !regular_file_exists(software_hive))
	{
		return result_t<prepared_input_t>::failure(
			"Applied WIM image failed validation because required offline registry hives are missing");
	}

	auto manifest_status = write_manifest(
		entry_stage / L"manifest.txt",
		iso_sha256,
		image_index,
		images.size(),
		image_name);
	if (!manifest_status.ok())
	{
		return result_t<prepared_input_t>::failure(
			manifest_status.error().message,
			manifest_status.error().code);
	}

	auto readonly_status = set_tree_read_only(entry_stage, true);
	if (!readonly_status.ok())
	{
		return result_t<prepared_input_t>::failure(
			readonly_status.error().message,
			readonly_status.error().code);
	}

	std::filesystem::create_directories(final_entry.parent_path(), create_error);
	if (create_error)
	{
		return result_t<prepared_input_t>::failure(
			"Unable to create ISO cache directory",
			static_cast<std::uint32_t>(create_error.value()));
	}

	std::error_code rename_error;
	std::filesystem::rename(entry_stage, final_entry, rename_error);
	if (rename_error)
	{
		auto raced = validate_cache_entry(final_entry, iso_sha256, image_index);
		if (raced.ok() && raced.value().has_value())
		{
			return cached_result(
				final_entry,
				std::move(iso_sha256),
				image_index,
				raced.value()->empty() ? std::move(image_name) : std::move(*raced.value()),
				true);
		}

		return result_t<prepared_input_t>::failure(
			"Unable to atomically publish immutable ISO cache entry",
			static_cast<std::uint32_t>(rename_error.value()));
	}

	return cached_result(
		final_entry,
		std::move(iso_sha256),
		image_index,
		std::move(image_name),
		false);
}

}
