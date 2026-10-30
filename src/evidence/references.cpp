#include "references.hpp"
#include "sql.hpp"

#include <algorithm>
#include <utility>

#include "../platform/win32.hpp"

namespace nylon::evidence
{
namespace
{

bool space(wchar_t character) { return character == L' ' || character == L'\t'; }

bool prefix(std::wstring_view value, std::wstring_view expected)
{
	if (value.size() < expected.size()) return false;
	for (std::size_t position = 0; position < expected.size(); ++position)
	{
		auto character = value[position];
		if (character >= L'a' && character <= L'z') character -= L'a' - L'A';
		if (character != expected[position]) return false;
	}
	return value.size() == expected.size() || value[expected.size()] == L'\\';
}

result_t<std::wstring> relative_path(std::wstring_view path)
{
	std::wstring normalized_input(path);
	std::replace(normalized_input.begin(), normalized_input.end(), L'/', L'\\');
	path = normalized_input;
	if (std::any_of(path.begin(), path.end(), [](wchar_t character) { return character < 32; }))
		return result_t<std::wstring>::failure("Control character in offline path context");
	if (!path.empty() && path.front() == L'\\') return result_t<std::wstring>::failure("Absolute/UNC/device path has no authoritative offline mapping");
	if (path.find_first_of(L":%\"<>|*?") != std::wstring_view::npos)
		return result_t<std::wstring>::failure("Nonliteral path, unknown environment variable, drive or stream mapping");
	std::vector<std::wstring_view> components;
	std::size_t begin = 0;
	while (begin <= path.size())
	{
		const auto end = path.find(L'\\', begin);
		const auto component = path.substr(begin, end == std::wstring_view::npos ? path.size() - begin : end - begin);
		if (component == L"..")
		{
			return result_t<std::wstring>::failure("Parent traversal needs reparse-aware source semantics; not lexically collapsed");
		}
		else if (!component.empty() && component != L".")
		{
			if (component.back() == L'.' || component.back() == L' ')
				return result_t<std::wstring>::failure("Trailing dot/space path semantics require additional context");
			components.push_back(component);
		}
		if (end == std::wstring_view::npos) break;
		begin = end + 1;
	}
	std::wstring normalized;
	for (const auto component : components)
	{
		if (!normalized.empty()) normalized += L'\\';
		normalized += component;
	}
	return result_t<std::wstring>::success(std::move(normalized));
}

std::string column_text(sqlite3_stmt* statement, int column)
{
	const auto* text = sqlite3_column_text(statement, column);
	return text ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(statement, column))) : std::string{};
}

}

reference_t parse_reference(std::wstring raw, const reference_context_t& context)
{
	reference_t result;
	result.raw = std::move(raw);
	auto fail = [&](std::string state, std::string detail)
	{
		result.state = std::move(state);
		result.detail = std::move(detail);
		return result;
	};
	if (result.raw.size() > max_reference_code_units) return fail("UNSUPPORTED_FORMAT", "Reference exceeds 1 Mi UTF-16-code-unit parser limit; raw input retained");
	if (!platform::utf8(result.raw).ok()) return fail("UNRESOLVED", "Reference encoding failure; original UTF-16 retained");
	if (result.raw.find(L'\0') != std::wstring::npos) return fail("PARSE_FAILED", "Embedded NUL in reference");
	std::wstring_view input(result.raw);
	if (context.command_line) while (!input.empty() && space(input.front())) input.remove_prefix(1);
	if (input.empty()) return fail("PARSE_FAILED", "Empty reference");
	if (input.front() == L'\"')
	{
		const auto closing = input.find(L'\"', 1);
		if (closing == std::wstring_view::npos) return fail("PARSE_FAILED", "Unterminated quoted target");
		result.target = input.substr(1, closing - 1);
		result.arguments = input.substr(closing + 1);
		if ((!context.command_line && !result.arguments.empty()) ||
			(!result.arguments.empty() && !space(result.arguments.front())))
			return fail("PARSE_FAILED", "Unexpected text after quoted target");
	}
	else if (context.command_line)
	{
		const auto split = input.find_first_of(L" \t");
		result.target = input.substr(0, split);
		if (split != std::wstring_view::npos)
		{
			result.arguments = input.substr(split);
			return fail("AMBIGUOUS", "Unquoted command with whitespace; executable boundary is not asserted");
		}
	}
	else result.target = input;
	if (result.target.empty()) return fail("PARSE_FAILED", "Empty target");
	if (std::any_of(result.target.begin(), result.target.end(), [](wchar_t character) { return character < 32; }))
		return fail("PARSE_FAILED", "Control character in target path");
	std::wstring path = result.target;
	std::replace(path.begin(), path.end(), L'/', L'\\');
	bool rooted = false;
	for (const auto marker : { L"%SYSTEMROOT%", L"%WINDIR%", L"\\SYSTEMROOT" })
	{
		const std::wstring_view token(marker);
		if (!prefix(path, token)) continue;
		if (context.windows_directory.empty()) return fail("UNRESOLVED", "Offline Windows directory was not supplied");
		auto base = relative_path(context.windows_directory);
		if (!base.ok()) return fail("UNRESOLVED", base.error().message);
		path = base.value() + path.substr(token.size());
		rooted = true;
		break;
	}
	if (!rooted)
	{
		if (!context.relative_base.has_value()) return fail("UNRESOLVED", "Relative path requires an explicit offline base; no host search path used");
		auto base = relative_path(*context.relative_base);
		if (!base.ok()) return fail("UNRESOLVED", base.error().message);
		if (!path.empty() && path.front() == L'\\') return fail("UNRESOLVED", "Absolute/UNC/device path has no authoritative offline mapping");
		path = base.value().empty() ? path : base.value() + L"\\" + path;
	}
	auto normalized = relative_path(path);
	if (!normalized.ok()) return fail("UNRESOLVED", normalized.error().message);
	auto converted = platform::utf8(normalized.value());
	if (!converted.ok()) return fail("UNRESOLVED", converted.error().message);
	result.image_path = std::move(converted).value();
	result.state = "CANDIDATE";
	result.detail = "Literal offline candidate only; namespace presence not yet established";
	return result;
}

reference_t resolve_reference(sqlite3* database, std::wstring raw, const reference_context_t& context)
{
	auto result = parse_reference(std::move(raw), context);
	if (result.state != "CANDIDATE") return result;
	try
	{
		if (!database) throw std::runtime_error("No raw namespace database supplied");
		query_t root(database, "SELECT id,kind,children_state FROM fs_entries WHERE parent_id IS NULL;");
		if (sqlite3_step(root.get()) != SQLITE_ROW) throw std::runtime_error("Cannot establish selected-image root");
		auto parent_id = sqlite3_column_int64(root.get(), 0);
		auto parent_kind = column_text(root.get(), 1);
		auto children_state = column_text(root.get(), 2);
		if (sqlite3_step(root.get()) != SQLITE_DONE) throw std::runtime_error("Ambiguous selected-image root");
		auto wide = platform::wide(*result.image_path);
		if (!wide.ok()) throw std::runtime_error(wide.error().message);
		query_t children(database, "SELECT id,kind,children_state FROM fs_entries WHERE parent_id=?1 AND comparison_key=?2 ORDER BY id;");
		query_t encoding(database, "SELECT EXISTS(SELECT 1 FROM fs_entries WHERE parent_id=?1 AND encoding_state<>'PRESENT');");
		std::wstring accumulated;
		std::size_t begin = 0;
		while (begin < wide.value().size())
		{
			if (parent_kind == "REPARSE") { result.state = "UNRESOLVED"; result.detail = "Reparse ancestor not followed"; return result; }
			if (parent_kind != "DIRECTORY") { result.state = "NOT_PRESENT"; result.detail = "Observed non-directory cannot contain requested child"; return result; }
			if (children_state != "COMPLETE") { result.state = "UNRESOLVED"; result.detail = "Parent enumeration state: " + children_state; return result; }
			encoding.number(1, parent_id);
			const int encoding_code = sqlite3_step(encoding.get());
			check_sql(database, encoding_code);
			if (encoding_code != SQLITE_ROW) throw std::runtime_error("Missing namespace encoding accounting");
			if (sqlite3_column_int(encoding.get(), 0)) { result.state = "UNRESOLVED"; result.detail = "Parent has unresolved child encoding/comparison identities"; return result; }
			check_sql(database, sqlite3_reset(encoding.get()));
			const auto end = wide.value().find(L'\\', begin);
			if (!accumulated.empty()) accumulated += L'\\';
			accumulated += wide.value().substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
			auto key = platform::path_comparison_key(accumulated);
			if (!key.ok()) { result.state = "UNRESOLVED"; result.detail = key.error().message; return result; }
			children.number(1, parent_id);
			children.text(2, key.value());
			result.matched_entries.clear();
			int code = SQLITE_OK;
			while ((code = sqlite3_step(children.get())) == SQLITE_ROW)
			{
				result.matched_entries.push_back(sqlite3_column_int64(children.get(), 0));
				parent_id = sqlite3_column_int64(children.get(), 0);
				parent_kind = column_text(children.get(), 1);
				children_state = column_text(children.get(), 2);
			}
			check_sql(database, code);
			check_sql(database, sqlite3_reset(children.get()));
			if (result.matched_entries.empty()) { result.state = "NOT_PRESENT"; result.detail = "Requested child absent from fully enumerated/encoded parent"; return result; }
			if (result.matched_entries.size() != 1) { result.state = "AMBIGUOUS"; result.detail = "Multiple namespace identities have the same comparison key"; return result; }
			if (end == std::wstring::npos) break;
			begin = end + 1;
		}
		if (parent_kind != "FILE")
		{
			result.state = "UNRESOLVED";
			result.detail = parent_kind == "REPARSE" ? "Reparse entry represented but target not followed" : "Reference names a directory, not a payload file";
			return result;
		}
		result.entry_id = parent_id;
		result.state = "PRESENT";
		result.detail = "Unique working-namespace file reference; not content interpretation, ownership or execution proof";
		return result;
	}
	catch (const std::exception& error)
	{
		result.state = "QUERY_FAILED";
		result.detail = error.what();
		return result;
	}
}

}
