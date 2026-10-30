#include "catalog.hpp"

#include <Windows.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

#include "../platform/win32.hpp"

namespace nylon::media
{
namespace
{

template<class Interface> struct release_t
{
	void operator()(Interface* pointer) const { if (pointer) pointer->Release(); }
};

void require_xml(HRESULT code)
{
	if (FAILED(code)) throw std::runtime_error("WIM catalog XML failure: " + std::to_string(static_cast<unsigned long>(code)));
}

std::string utf8(std::wstring_view value)
{
	auto converted = platform::utf8(value);
	if (!converted.ok()) throw std::runtime_error(converted.error().message);
	return std::move(converted).value();
}

std::wstring xml_value(IXmlReader* reader)
{
	const wchar_t* value = nullptr;
	UINT length = 0;
	require_xml(reader->GetValue(&value, &length));
	return std::wstring(value, length);
}

std::wstring xml_name(IXmlReader* reader)
{
	const wchar_t* value = nullptr;
	UINT length = 0;
	require_xml(reader->GetNamespaceUri(&value, &length));
	if (length != 0) throw std::runtime_error("Namespaced WIM catalog is unsupported");
	require_xml(reader->GetQualifiedName(&value, &length));
	return std::wstring(value, length);
}

struct element_t
{
	std::wstring name;
	std::wstring text;
	bool children = false;
};

}

result_t<catalog_t> parse_catalog_bytes(std::vector<std::byte> bytes)
{
	try
	{
		if (bytes.empty() || bytes.size() > max_catalog_bytes) throw std::runtime_error("WIM catalog empty or exceeds 64 MiB parser limit");
		std::unique_ptr<IStream, release_t<IStream>> stream(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
		if (!stream) throw std::runtime_error("Unable to allocate catalog stream");
		IXmlReader* raw = nullptr;
		require_xml(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&raw), nullptr));
		std::unique_ptr<IXmlReader, release_t<IXmlReader>> reader(raw);
		require_xml(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
		require_xml(reader->SetProperty(XmlReaderProperty_XmlResolver, 0));
		require_xml(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64));
		require_xml(reader->SetInput(stream.get()));
		catalog_t catalog;
		catalog.xml = std::move(bytes);
		std::vector<element_t> stack;
		catalog_image_t* image = nullptr;
		bool root_seen = false;
		auto field_path = [&]()
		{
			std::wstring path;
			for (std::size_t position = 2; position < stack.size(); ++position)
			{
				if (!path.empty()) path += L"/";
				path += stack[position].name;
			}
			return path;
		};
		auto finish = [&]()
		{
			if (image && !stack.back().children && stack.size() > 2)
			{
				image->fields.push_back({ utf8(field_path()), utf8(stack.back().text) });
				if (stack.size() == 3 && stack.back().name == L"NAME")
				{
					if (std::count_if(image->fields.begin(), image->fields.end(), [](const auto& field) { return field.path == "NAME"; }) > 1)
						throw std::runtime_error("Duplicate WIM image NAME");
					image->name = stack.back().text;
				}
			}
			if (stack.size() == 2) image = nullptr;
			stack.pop_back();
		};
		XmlNodeType type = XmlNodeType_None;
		HRESULT code = S_OK;
		while ((code = reader->Read(&type)) == S_OK)
		{
			if (type == XmlNodeType_Element)
			{
				auto name = xml_name(reader.get());
				if (stack.empty())
				{
					if (root_seen || name != L"WIM") throw std::runtime_error("Invalid WIM catalog root");
					root_seen = true;
				}
				else stack.back().children = true;
				stack.push_back({ std::move(name), {}, false });
				const bool new_image = stack.size() == 2 && stack.back().name == L"IMAGE";
				if (new_image)
				{
					catalog.images.emplace_back();
					image = &catalog.images.back();
				}
				if (stack.size() > 2 && stack.back().name == L"IMAGE") throw std::runtime_error("Nested WIM IMAGE element");
				HRESULT attribute = reader->MoveToFirstAttribute();
				while (attribute == S_OK)
				{
					const auto attribute_name = xml_name(reader.get());
					const auto value = xml_value(reader.get());
					if (new_image && attribute_name == L"INDEX")
					{
						const auto numeric = utf8(value);
						const auto parsed = std::from_chars(numeric.data(), numeric.data() + numeric.size(), image->index);
						if (parsed.ec != std::errc{} || parsed.ptr != numeric.data() + numeric.size() || image->index == 0)
							throw std::runtime_error("Invalid WIM image index");
					}
					else if (image) image->fields.push_back({ utf8(field_path() + L"/@" + attribute_name), utf8(value) });
					attribute = reader->MoveToNextAttribute();
				}
				require_xml(attribute);
				require_xml(reader->MoveToElement());
				if (new_image && image->index == 0) throw std::runtime_error("Missing WIM image index");
				if (reader->IsEmptyElement()) finish();
			}
			else if (type == XmlNodeType_EndElement) finish();
			else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)
			{
				if (!stack.empty()) stack.back().text += xml_value(reader.get());
			}
		}
		require_xml(code);
		if (!stack.empty() || !root_seen || catalog.images.empty()) throw std::runtime_error("Incomplete WIM image catalog");
		std::sort(catalog.images.begin(), catalog.images.end(), [](const auto& left, const auto& right) { return left.index < right.index; });
		for (std::size_t position = 1; position < catalog.images.size(); ++position)
			if (catalog.images[position - 1].index == catalog.images[position].index) throw std::runtime_error("Duplicate WIM image index");
		return result_t<catalog_t>::success(std::move(catalog));
	}
	catch (const std::exception& error) { return result_t<catalog_t>::failure(error.what()); }
}

result_t<std::vector<std::byte>> read_catalog_xml(const std::filesystem::path& path)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size == 0 || size > max_catalog_bytes) return result_t<std::vector<std::byte>>::failure("Unable to read bounded WIM catalog", static_cast<std::uint32_t>(error.value()));
	std::ifstream stream(path, std::ios::binary);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return result_t<std::vector<std::byte>>::failure("Unable to read complete WIM catalog");
	return result_t<std::vector<std::byte>>::success(std::move(bytes));
}

result_t<catalog_t> read_catalog(const std::filesystem::path& path)
{
	auto bytes = read_catalog_xml(path);
	if (!bytes.ok()) return result_t<catalog_t>::failure(bytes.error().message, bytes.error().code);
	return parse_catalog_bytes(std::move(bytes).value());
}

}
