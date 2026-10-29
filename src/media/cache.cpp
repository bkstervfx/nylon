#include "media.hpp"

#include <Windows.h>

#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "../platform/win32.hpp"

namespace nylon::media
{

namespace
{

constexpr std::size_t max_manifest_bytes = 1024U * 1024U;

result_t<std::string> read_manifest(const std::filesystem::path& path)
{
	std::error_code size_error;
	const auto size = std::filesystem::file_size(path, size_error);
	if (size_error || size > max_manifest_bytes ||
		size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
	{
		return result_t<std::string>::failure("Unable to read immutable cache manifest");
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream)
	{
		return result_t<std::string>::failure("Unable to open immutable cache manifest");
	}

	std::string text(static_cast<std::size_t>(size), '\0');
	if (!text.empty())
	{
		stream.read(text.data(), static_cast<std::streamsize>(text.size()));
		if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size()))
		{
			return result_t<std::string>::failure("Unable to read complete immutable cache manifest");
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

status_t require_manifest_value(
	std::string_view manifest,
	std::string_view key,
	std::string_view expected)
{
	const auto value = manifest_value(manifest, key);
	if (!value.has_value() || *value != expected)
	{
		return status_t::failure(
			"Immutable ISO cache manifest is incompatible or has been modified: " + std::string(key));
	}
	return status_t::success();
}

status_t require_read_only(const std::filesystem::path& path)
{
	auto attributes = platform::file_attributes(path);
	if (!attributes.ok())
	{
		return status_t::failure(attributes.error().message, attributes.error().code);
	}
	if ((attributes.value() & FILE_ATTRIBUTE_READONLY) == 0)
	{
		return status_t::failure("Immutable ISO cache contains a writable path");
	}
	return status_t::success();
}

status_t verify_read_only_tree(const std::filesystem::path& root)
{
	auto root_status = require_read_only(root);
	if (!root_status.ok()) return root_status;

	std::error_code error;
	std::filesystem::recursive_directory_iterator iterator(root, error);
	const std::filesystem::recursive_directory_iterator end;
	if (error)
	{
		return status_t::failure("Unable to enumerate immutable ISO cache", static_cast<std::uint32_t>(error.value()));
	}

	while (iterator != end)
	{
		auto attributes = platform::file_attributes(iterator->path());
		if (!attributes.ok())
		{
			return status_t::failure(attributes.error().message, attributes.error().code);
		}
		if ((attributes.value() & FILE_ATTRIBUTE_READONLY) == 0)
		{
			return status_t::failure("Immutable ISO cache contains a writable path");
		}
		if ((attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			iterator.disable_recursion_pending();
		}

		iterator.increment(error);
		if (error)
		{
			return status_t::failure(
				"Unable to enumerate immutable ISO cache",
				static_cast<std::uint32_t>(error.value()));
		}
	}

	return status_t::success();
}

}

status_t verify_prepared_cache(const prepared_input_t& prepared)
{
	if (prepared.source_kind != "iso")
	{
		return status_t::success();
	}
	if (prepared.iso_sha256.size() != 64U || prepared.image_index == 0U)
	{
		return status_t::failure("Prepared ISO cache identity is invalid");
	}

	const auto entry = prepared.image_root.parent_path();
	const auto manifest_path = entry / L"manifest.txt";
	auto manifest_result = read_manifest(manifest_path);
	if (!manifest_result.ok())
	{
		return status_t::failure(manifest_result.error().message, manifest_result.error().code);
	}
	const auto& manifest = manifest_result.value();

	for (const auto& [key, expected] : {
		std::pair<std::string_view, std::string>{ "format", "1" },
		{ "complete", "1" },
		{ "iso_sha256", prepared.iso_sha256 },
		{ "image_index", std::to_string(prepared.image_index) },
		{ "iso_extractor", "7zip-26.05" },
		{ "wimlib", "1.14.5" },
		{ "wim_apply_flags", "--no-acls --no-attributes --rpfix" },
		{ "immutable", "1" } })
	{
		auto status = require_manifest_value(manifest, key, expected);
		if (!status.ok()) return status;
	}

	return verify_read_only_tree(entry);
}

}
