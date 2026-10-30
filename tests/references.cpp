#include <iostream>
#include <memory>
#include <stdexcept>

#include "evidence/references.hpp"
#include "evidence/sql.hpp"
#include "platform/hash.hpp"
#include "platform/win32.hpp"

namespace
{

void require(bool condition, const std::string& message)
{
	if (!condition) throw std::runtime_error(message);
}

nylon::evidence::reference_t expect(sqlite3* database, const std::wstring& raw,
	const nylon::evidence::reference_context_t& context, std::string_view state,
	std::optional<sqlite3_int64> identity = std::nullopt)
{
	auto reference = nylon::evidence::resolve_reference(database, raw, context);
	require(reference.raw == raw, "Raw reference changed");
	auto display = nylon::platform::utf8(raw.size() < 256 ? raw : L"<long raw reference>");
	require(reference.state == state, "Expected " + std::string(state) + "; got " + reference.state + " for " + (display.ok() ? display.value() : "<invalid encoding>") + ": " + reference.detail);
	require(reference.entry_id.has_value() == (state == "PRESENT"), "Unexpected resolved entry identity");
	if (identity) require(reference.entry_id == identity, "Wrong canonical namespace entry");
	return reference;
}

void fixtures()
{
	sqlite3* handle = nullptr;
	nylon::evidence::check_sql(handle, sqlite3_open(":memory:", &handle));
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(handle, sqlite3_close);
	nylon::evidence::execute(handle, "CREATE TABLE fs_entries(id INTEGER PRIMARY KEY,parent_id INTEGER,path TEXT,comparison_key TEXT,kind TEXT,children_state TEXT,encoding_state TEXT); CREATE INDEX parent_index ON fs_entries(parent_id);");
	auto add = [&](sqlite3_int64 identity, std::optional<sqlite3_int64> parent, const std::wstring& path,
		std::string_view kind, std::string_view children = "NOT_APPLICABLE")
	{
		nylon::evidence::query_t insert(handle, "INSERT INTO fs_entries VALUES(?1,?2,?3,?4,?5,?6,'PRESENT');");
		insert.number(1, identity);
		if (parent) insert.number(2, *parent);
		auto name = nylon::platform::utf8(path);
		auto key = nylon::platform::path_comparison_key(path);
		require(name.ok() && key.ok(), "Invalid fixture path");
		insert.text(3, name.value());
		insert.text(4, key.value());
		insert.text(5, kind);
		insert.text(6, children);
		insert.done();
	};
	add(1, {}, L"", "DIRECTORY", "COMPLETE");
	add(2, 1, L"Windows", "DIRECTORY", "COMPLETE");
	add(3, 2, L"Windows\\System32", "DIRECTORY", "COMPLETE");
	add(4, 3, L"Windows\\System32\\Tool.exe", "FILE");
	add(5, 2, L"Windows\\Ångström", "DIRECTORY", "COMPLETE");
	add(6, 5, L"Windows\\Ångström\\模块.dll", "FILE");
	add(7, 2, L"Windows\\link", "REPARSE");
	add(8, 2, L"Windows\\partial", "DIRECTORY", "QUERY_FAILED");
	add(9, 8, L"Windows\\partial\\known.dll", "FILE");
	const nylon::evidence::reference_context_t root{ L"Windows", L"", false };
	const nylon::evidence::reference_context_t windows{ L"Windows", std::nullopt, false };
	const nylon::evidence::reference_context_t system32{ L"Windows", L"Windows\\System32", false };
	const nylon::evidence::reference_context_t command{ L"Windows", std::nullopt, true };
	expect(handle, L"Windows\\System32\\tool.EXE", root, "PRESENT", 4);
	expect(handle, L"%SystemRoot%\\System32\\Tool.exe", windows, "PRESENT", 4);
	expect(handle, L"%wInDiR%/System32/Tool.exe", windows, "PRESENT", 4);
	expect(handle, L"\\SystemRoot\\System32\\Tool.exe", windows, "PRESENT", 4);
	expect(handle, L"Windows\\ångström\\模块.dll", root, "PRESENT", 6);
	expect(handle, L"Tool.exe", system32, "PRESENT", 4);
	expect(handle, L"Tool.exe", { L"Windows", L"Windows/System32", false }, "PRESENT", 4);
	auto quoted = expect(handle, L"  \"%SystemRoot%\\System32\\Tool.exe\" -k Group", command, "PRESENT", 4);
	require(quoted.target == L"%SystemRoot%\\System32\\Tool.exe" && quoted.arguments == L" -k Group", "Quoted command source fields lost");
	expect(handle, L"%SystemRoot%\\System32\\Tool.exe -k Group", command, "AMBIGUOUS");
	expect(handle, L"C:\\Program Files\\Tool.exe", command, "AMBIGUOUS");
	expect(handle, L"C:\\Windows\\System32\\Tool.exe", root, "UNRESOLVED");
	expect(handle, L"%USERPROFILE%\\Tool.exe", root, "UNRESOLVED");
	expect(handle, L"%SystemRoot%suffix\\System32\\Tool.exe", root, "UNRESOLVED");
	expect(handle, L"Tool.exe", windows, "UNRESOLVED");
	expect(handle, L"\\\\host\\share\\Tool.exe", root, "UNRESOLVED");
	expect(handle, L"\\??\\C:\\Tool.exe", root, "UNRESOLVED");
	expect(handle, L"..\\outside.exe", root, "UNRESOLVED");
	expect(handle, L"Windows\\System32\\Tool.exe:stream", root, "UNRESOLVED");
	expect(handle, L"Windows\\System32\\Tool.exe.", root, "UNRESOLVED");
	expect(handle, L"\"%SystemRoot%\\System32\\Tool.exe", command, "PARSE_FAILED");
	expect(handle, L"\"%SystemRoot%\\System32\\Tool.exe\"suffix", command, "PARSE_FAILED");
	expect(handle, std::wstring(L"Windows\\System32\\Tool.exe\0hidden", 32), root, "PARSE_FAILED");
	expect(handle, std::wstring(1, static_cast<wchar_t>(0xD800)), root, "UNRESOLVED");
	expect(handle, std::wstring(nylon::evidence::max_reference_code_units + 1, L'a'), root, "UNSUPPORTED_FORMAT");
	expect(handle, L"Windows\\System32\\absent.exe", root, "NOT_PRESENT");
	expect(handle, L"Windows\\absent\\child.exe", root, "NOT_PRESENT");
	expect(handle, L"Windows\\System32\\Tool.exe\\child.exe", root, "NOT_PRESENT");
	expect(handle, L"Windows\\partial\\absent.exe", root, "UNRESOLVED");
	expect(handle, L"Windows\\partial\\known.dll", root, "UNRESOLVED");
	expect(handle, L"Windows\\link", root, "UNRESOLVED");
	expect(handle, L"Windows\\link\\outside.exe", root, "UNRESOLVED");
	expect(handle, L"Windows\\link\\..\\System32\\Tool.exe", root, "UNRESOLVED");
	add(10, 3, L"Windows\\System32\\tool.EXE", "FILE");
	auto ambiguous = expect(handle, L"Windows\\System32\\Tool.exe", root, "AMBIGUOUS");
	require(ambiguous.matched_entries.size() == 2, "Case-collision evidence truncated");
	nylon::evidence::execute(handle, "DELETE FROM fs_entries WHERE id=10; UPDATE fs_entries SET encoding_state='UNRESOLVED' WHERE id=6;");
	expect(handle, L"Windows\\Ångström\\absent.dll", root, "UNRESOLVED");
	nylon::evidence::execute(handle, "UPDATE fs_entries SET encoding_state='PRESENT' WHERE id=6;");
	std::wstring long_directory(4000, L'a');
	add(11, 2, L"Windows\\" + long_directory, "DIRECTORY", "COMPLETE");
	add(12, 11, L"Windows\\" + long_directory + L"\\file.dll", "FILE");
	expect(handle, L"Windows\\" + long_directory + L"\\file.dll", root, "PRESENT", 12);
	expect(nullptr, L"Windows\\System32\\Tool.exe", root, "QUERY_FAILED");
	nylon::evidence::execute(handle, "DROP TABLE fs_entries;");
	expect(handle, L"Windows\\System32\\Tool.exe", root, "QUERY_FAILED");
}

void real_audit(const std::filesystem::path& source)
{
	auto lock = nylon::platform::open_read_lock(source);
	require(lock.ok(), "Cannot lock real audit");
	auto before = nylon::platform::sha256_file(source);
	require(before.ok(), "Cannot hash real audit");
	auto path = nylon::platform::utf8(source.native());
	require(path.ok(), "Invalid real audit path");
	sqlite3* handle = nullptr;
	nylon::evidence::check_sql(handle, sqlite3_open_v2(path.value().c_str(), &handle, SQLITE_OPEN_READONLY, nullptr));
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(handle, sqlite3_close);
	const nylon::evidence::reference_context_t context{ L"Windows", L"", false };
	expect(handle, L"%SystemRoot%\\System32\\ntoskrnl.exe", context, "PRESENT");
	nylon::evidence::query_t reparses(handle, "SELECT path FROM fs_entries WHERE kind='REPARSE' AND path IS NOT NULL;");
	std::size_t count = 0;
	int code = SQLITE_OK;
	while ((code = sqlite3_step(reparses.get())) == SQLITE_ROW)
	{
		const auto* raw = sqlite3_column_text(reparses.get(), 0);
		auto name = nylon::platform::wide(reinterpret_cast<const char*>(raw));
		require(name.ok(), "Invalid real reparse path");
		expect(handle, name.value(), context, "UNRESOLVED");
		++count;
	}
	nylon::evidence::check_sql(handle, code);
	require(count > 0, "Real fixture has no reparse coverage");
	auto after = nylon::platform::sha256_file(source);
	require(after.ok() && before.value() == after.value(), "Reference queries modified real audit");
	std::cout << "PASS: real namespace reference and " << count << " reparse barriers; exact DB hash unchanged\n";
}

}

int wmain(int argc, wchar_t** argv)
{
	try
	{
		if (argc == 3 && std::wstring_view(argv[1]) == L"--real") real_audit(argv[2]);
		else if (argc == 1) { fixtures(); std::cout << "PASS: offline reference parsing and conservative inventory-only resolution fixtures\n"; }
		else return 2;
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what(); return 1; }
}
