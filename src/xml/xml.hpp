#ifndef NYLON_XML_XML_HPP
#define NYLON_XML_XML_HPP

#include "../win32/resource.hpp"

#include <xmllite.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace nylon::xml
{
	using attribute_fn_t = std::function<std::wstring( std::wstring_view )>;
	using start_element_fn_t = std::function<void( std::wstring_view, const attribute_fn_t& )>;
	using text_fn_t = std::function<void( std::wstring_view, std::wstring_view )>;
	using end_element_fn_t = std::function<void( std::wstring_view )>;

	void read(
		const std::filesystem::path& path,
		const start_element_fn_t& on_start_element,
		const text_fn_t& on_text = {},
		const end_element_fn_t& on_end_element = {} );

	void read(
		std::span<const std::byte> data,
		const start_element_fn_t& on_start_element,
		const text_fn_t& on_text = {},
		const end_element_fn_t& on_end_element = {} );
}

#endif
