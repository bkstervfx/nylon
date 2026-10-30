#include "evidence.hpp"
#include "sql.hpp"

#include <Windows.h>
#include <winioctl.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../platform/win32.hpp"

namespace nylon::evidence
{
namespace
{

struct find_closer_t
{
	void operator()(void* handle) const { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
};
using find_guard_t = std::unique_ptr<void, find_closer_t>;

struct frame_t
{
	std::wstring relative;
	sqlite3_int64 id;
	platform::unique_handle_t directory;
	find_guard_t search;
	WIN32_FIND_DATAW entry;
	bool available;
};

class inventory_t
{
public:
	explicit inventory_t(sqlite3* database) : database_(database),
		insert_(database, "INSERT INTO fs_entries(parent_id,path,path_utf16,comparison_key,kind,encoding_state,metadata_state,children_state,size,creation_time,access_time,write_time,attributes,reparse_tag,reparse_state) VALUES(?1,?2,?3,?4,?5,?6,'QUERY_FAILED',?7,?8,?9,?10,?11,?12,?13,?14);"),
		gap_(database, "INSERT INTO evidence_gaps(collector,source_entry_id,operation,state,error_code,detail) VALUES('filesystem',?1,?2,?3,?4,?5);"),
		children_(database, "UPDATE fs_entries SET children_state=?2 WHERE id=?1;"),
		metadata_(database, "UPDATE fs_entries SET metadata_state=?2,volume_serial=?3,file_index=?4,link_count=?5,error_code=?6 WHERE id=?1;"),
		reparse_(database, "UPDATE fs_entries SET reparse_data=?2,reparse_state=?3 WHERE id=?1;") {}

	void run(const std::filesystem::path& root)
	{
		execute(database_, "UPDATE collector_runs SET state='RUNNING',started_utc=CURRENT_TIMESTAMP WHERE collector='filesystem';");
		auto native = platform::native_path(root);
		if (!native.ok()) throw std::runtime_error(native.error().message);
		root_ = std::move(native).value();
		std::vector<platform::unique_handle_t> ancestors;
		const auto absolute = std::filesystem::absolute(root).lexically_normal();
		auto current = absolute.root_path();
		bool guarded = true;
		DWORD guard_error = ERROR_SUCCESS;
		for (const auto& component : absolute.relative_path())
		{
			current /= component;
			auto path = platform::native_path(current);
			if (!path.ok()) { guarded = false; guard_error = path.error().code; break; }
			auto guard = directory_guard(path.value(), guard_error);
			if (!guard) { guarded = false; break; }
			ancestors.push_back(std::move(guard));
		}
		WIN32_FIND_DATAW root_data{};
		root_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
		if (guarded)
		{
			WIN32_FILE_ATTRIBUTE_DATA attributes{};
			if (GetFileAttributesExW(root_.c_str(), GetFileExInfoStandard, &attributes))
			{
				root_data.dwFileAttributes = attributes.dwFileAttributes;
				root_data.ftCreationTime = attributes.ftCreationTime;
				root_data.ftLastAccessTime = attributes.ftLastAccessTime;
				root_data.ftLastWriteTime = attributes.ftLastWriteTime;
			}
			else { guarded = false; guard_error = GetLastError(); }
		}
		const auto root_id = add({}, std::nullopt, root_data, !guarded);
		if (!guarded)
		{
			gap(root_id, "enumerate", guard_error, "Root or ancestor could not be held as a non-reparse directory");
			set_children(root_id, "QUERY_FAILED");
		}
		else
		{
			DWORD error = 0;
			auto guard = directory_guard(root_, error);
			if (!guard)
			{
				gap(root_id, "enumerate", error, "Cannot guard image root");
				set_children(root_id, "QUERY_FAILED");
			}
			else push({}, root_id, std::move(guard));
			while (!frames_.empty())
			{
				auto& frame = frames_.back();
				if (!frame.available) { frames_.pop_back(); continue; }
				const auto data = frame.entry;
				const std::wstring name(data.cFileName);
				const auto parent = frame.id;
				const auto relative = frame.relative.empty() ? name : frame.relative + L"\\" + name;
				if (!FindNextFileW(frame.search.get(), &frame.entry))
				{
					const DWORD error_code = GetLastError();
					frame.available = false;
					if (error_code != ERROR_NO_MORE_FILES)
					{
						gap(frame.id, "enumerate", error_code, "FindNextFileW failed");
						set_children(frame.id, "QUERY_FAILED");
					}
					else set_children(frame.id, "COMPLETE");
				}
				if (name == L"." || name == L"..") continue;
				const auto id = add(relative, parent, data, false);
				if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
					(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
				if (frames_.size() >= 512U)
				{
					gap(id, "enumerate", ERROR_BUFFER_OVERFLOW, "Namespace depth exceeds bounded traversal policy (512)");
					set_children(id, "UNSUPPORTED_FORMAT");
					continue;
				}
				auto child_guard = directory_guard(full_path(relative), error);
				if (!child_guard)
				{
					gap(id, "enumerate", error, "Directory changed, inaccessible or reparse; not traversed");
					set_children(id, "QUERY_FAILED");
					continue;
				}
				push(relative, id, std::move(child_guard));
			}
		}
		query_t finish(database_, R"sql(
UPDATE collector_runs SET
 discovered=?1,represented=(SELECT count(*) FROM fs_entries),
 parsed=(SELECT count(*) FROM fs_entries WHERE metadata_state='PRESENT'),
 failed=(SELECT count(DISTINCT source_entry_id) FROM evidence_gaps WHERE collector='filesystem'),
 unresolved=(SELECT count(*) FROM evidence_gaps WHERE collector='filesystem'),
 state=CASE WHEN EXISTS(SELECT 1 FROM evidence_gaps WHERE collector='filesystem') THEN 'COMPLETE_WITH_GAPS' ELSE 'COMPLETE' END,
 discovery_complete=NOT EXISTS(SELECT 1 FROM evidence_gaps WHERE collector='filesystem' AND operation='enumerate'),
 representation_complete=(?1=(SELECT count(*) FROM fs_entries)),
 ended_utc=CURRENT_TIMESTAMP,checkpoint='namespace-walk-finished'
WHERE collector='filesystem';
)sql");
		finish.number(1, discovered_);
		finish.done();
	}

private:
	std::wstring full_path(const std::wstring& relative) const
	{
		return relative.empty() ? root_ : root_ + L"\\" + relative;
	}

	platform::unique_handle_t directory_guard(const std::wstring& path, DWORD& error)
	{
		platform::unique_handle_t handle(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		if (!handle) { error = GetLastError(); return {}; }
		BY_HANDLE_FILE_INFORMATION information{};
		if (!GetFileInformationByHandle(handle.get(), &information)) { error = GetLastError(); return {}; }
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
			(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			error = ERROR_REPARSE_TAG_INVALID;
			return {};
		}
		return handle;
	}

	void gap(sqlite3_int64 id, std::string_view operation, DWORD error, std::string_view detail)
	{
		gap_.number(1, id);
		gap_.text(2, operation);
		gap_.text(3, error == ERROR_ACCESS_DENIED ? "ACCESS_DENIED" : "QUERY_FAILED");
		gap_.number(4, error);
		gap_.text(5, detail);
		gap_.done();
	}

	void set_children(sqlite3_int64 id, std::string_view state)
	{
		children_.number(1, id);
		children_.text(2, state);
		children_.done();
	}

	void push(std::wstring relative, sqlite3_int64 id, platform::unique_handle_t guard)
	{
		WIN32_FIND_DATAW entry{};
		find_guard_t search(FindFirstFileExW((full_path(relative) + L"\\*").c_str(),
			FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
		if (search.get() == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND) set_children(id, "COMPLETE");
			else
			{
				gap(id, "enumerate", error, "FindFirstFileExW failed");
				set_children(id, "QUERY_FAILED");
			}
			return;
		}
		frames_.push_back(frame_t{ std::move(relative), id, std::move(guard), std::move(search), entry, true });
	}

	sqlite3_int64 add(const std::wstring& relative, std::optional<sqlite3_int64> parent,
		const WIN32_FIND_DATAW& data, bool skip_metadata)
	{
		++discovered_;
		const bool reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (parent) insert_.number(1, *parent);
		auto text = platform::utf8(relative);
		if (text.ok()) insert_.text(2, text.value());
		insert_.blob(3, relative.data(), relative.size() * sizeof(wchar_t));
		bool normalized = false;
		if (relative.empty()) { insert_.text(4, ""); normalized = true; }
		else
		{
			const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
				relative.data(), static_cast<int>(relative.size()), nullptr, 0, nullptr, nullptr, 0);
			if (size > 0)
			{
				std::wstring key(static_cast<std::size_t>(size), L'\0');
				if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, relative.data(),
					static_cast<int>(relative.size()), key.data(), size, nullptr, nullptr, 0) > 0)
				{
					auto converted = platform::utf8(key);
					if (converted.ok()) { insert_.text(4, converted.value()); normalized = true; }
				}
			}
		}
		insert_.text(5, reparse ? "REPARSE" : directory ? "DIRECTORY" : "FILE");
		insert_.text(6, text.ok() && normalized ? "PRESENT" : "UNRESOLVED");
		insert_.text(7, directory && !reparse ? "PENDING" : "NOT_APPLICABLE");
		if (!directory) insert_.number(8, (static_cast<sqlite3_int64>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow);
		if (!skip_metadata)
		{
			insert_.blob(9, &data.ftCreationTime, sizeof(FILETIME));
			insert_.blob(10, &data.ftLastAccessTime, sizeof(FILETIME));
			insert_.blob(11, &data.ftLastWriteTime, sizeof(FILETIME));
		}
		insert_.number(12, data.dwFileAttributes);
		if (reparse) insert_.number(13, data.dwReserved0);
		insert_.text(14, reparse ? "QUERY_FAILED" : "NOT_APPLICABLE");
		insert_.done();
		const auto id = sqlite3_last_insert_rowid(database_);
		if (!text.ok() || !normalized) gap(id, "encode_path", ERROR_NO_UNICODE_TRANSLATION, "Original UTF-16 identity retained; text/comparison conversion failed");
		if (skip_metadata)
		{
			gap(id, "metadata", ERROR_ACCESS_DENIED, "Unsafe root chain; no path-based metadata query attempted");
			return id;
		}
		platform::unique_handle_t handle(CreateFileW(full_path(relative).c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		BY_HANDLE_FILE_INFORMATION information{};
		const bool readable = handle && GetFileInformationByHandle(handle.get(), &information);
		const DWORD error = readable ? ERROR_SUCCESS : GetLastError();
		metadata_.number(1, id);
		metadata_.text(2, readable ? "PRESENT" : error == ERROR_ACCESS_DENIED ? "ACCESS_DENIED" : "QUERY_FAILED");
		if (readable)
		{
			metadata_.number(3, information.dwVolumeSerialNumber);
			const std::uint64_t index = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) | information.nFileIndexLow;
			metadata_.blob(4, &index, sizeof(index));
			metadata_.number(5, information.nNumberOfLinks);
			if (information.dwFileAttributes != data.dwFileAttributes && parent)
				gap(id, "metadata", ERROR_FILE_INVALID, "Attributes changed between discovery and metadata query");
		}
		else gap(id, "metadata", error, "Cannot read working-view file identity");
		metadata_.number(6, error);
		metadata_.done();
		if (reparse && handle)
		{
			std::vector<std::byte> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
			DWORD written = 0;
			const bool obtained = DeviceIoControl(handle.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0,
				buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) != FALSE;
			const DWORD reparse_error = obtained ? ERROR_SUCCESS : GetLastError();
			reparse_.number(1, id);
			if (obtained) reparse_.blob(2, buffer.data(), written);
			reparse_.text(3, obtained ? "PRESENT" : "QUERY_FAILED");
			reparse_.done();
			if (!obtained) gap(id, "reparse_metadata", reparse_error, "Working-view reparse data unavailable; target never followed");
		}
		return id;
	}

	sqlite3* database_;
	query_t insert_, gap_, children_, metadata_, reparse_;
	std::wstring root_;
	std::vector<frame_t> frames_;
	sqlite3_int64 discovered_ = 0;
};

}

status_t inventory(database::database_t& database, const std::filesystem::path& root)
{
	auto status = database.begin();
	if (!status.ok()) return status;
	try
	{
		inventory_t collector(database.native_handle());
		collector.run(root);
		status = verify(database);
		if (!status.ok()) { database.rollback(); return status; }
		status = database.commit();
		if (!status.ok()) database.rollback();
		return status;
	}
	catch (const std::exception& error)
	{
		database.rollback();
		return status_t::failure(error.what());
	}
}

}
