#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

std::string path_utf8(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("Unable to encode path as UTF-8");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), static_cast<int>(wide.size()), result.data(), required, nullptr, nullptr) <= 0)
        throw std::runtime_error("Unable to encode path as UTF-8");
    return result;
}

class database_t
{
public:
    database_t() noexcept = default;
    ~database_t() { if (handle_ != nullptr) sqlite3_close_v2(handle_); }
    database_t(const database_t&) = delete;
    database_t& operator=(const database_t&) = delete;
    database_t(database_t&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    static database_t open(const std::filesystem::path& path, int flags)
    {
        sqlite3* handle = nullptr;
        const std::string utf8 = path_utf8(path);
        if (sqlite3_open_v2(utf8.c_str(), &handle, flags, nullptr) != SQLITE_OK)
        {
            const std::string error = handle != nullptr ? sqlite3_errmsg(handle) : "sqlite3_open_v2 failed";
            if (handle != nullptr) sqlite3_close_v2(handle);
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(handle, 5000);
        database_t result;
        result.handle_ = handle;
        return result;
    }

    [[nodiscard]] sqlite3* get() const noexcept { return handle_; }

private:
    sqlite3* handle_ = nullptr;
};

class statement_t
{
public:
    statement_t(sqlite3* database, std::string_view sql)
    {
        const std::string owned(sql);
        if (sqlite3_prepare_v2(database, owned.c_str(), -1, &handle_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database));
    }
    ~statement_t() { if (handle_ != nullptr) sqlite3_finalize(handle_); }
    statement_t(const statement_t&) = delete;
    statement_t& operator=(const statement_t&) = delete;
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return handle_; }
private:
    sqlite3_stmt* handle_ = nullptr;
};

void exec(sqlite3* database, std::string_view sql)
{
    char* error = nullptr;
    if (sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr, &error) != SQLITE_OK)
    {
        const std::string message = error != nullptr ? error : sqlite3_errmsg(database);
        if (error != nullptr) sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

std::string text(sqlite3_stmt* statement, int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    if (value == nullptr) return {};
    const int bytes = sqlite3_column_bytes(statement, column);
    return bytes > 0 ? std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes)) : std::string{};
}

std::filesystem::path map_path_for(const std::filesystem::path& source)
{
    std::filesystem::path result = source;
    result.replace_extension(L".map.db");
    return result;
}

std::string upper_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::vector<std::int64_t> parse_roots(std::string_view roots)
{
    std::vector<std::int64_t> result;
    std::size_t cursor = 0;
    while (cursor < roots.size())
    {
        const std::size_t comma = roots.find(',', cursor);
        const std::string_view part = roots.substr(cursor, comma == std::string_view::npos ? roots.size() - cursor : comma - cursor);
        std::int64_t id = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || id <= 0)
            throw std::runtime_error("Invalid semantic job root list");
        result.push_back(id);
        if (comma == std::string_view::npos) break;
        cursor = comma + 1U;
    }
    return result;
}

bool immediate_child_after(std::string_view upper, std::string_view marker, std::string_view* remainder_out = nullptr)
{
    const std::size_t found = upper.find(marker);
    if (found == std::string_view::npos) return false;
    const std::string_view remainder = upper.substr(found + marker.size());
    if (remainder.empty() || remainder.find('\\') != std::string_view::npos) return false;
    if (remainder_out != nullptr) *remainder_out = remainder;
    return true;
}

bool guid_like(std::string_view value)
{
    return value.size() >= 3U && value.front() == '{' && value.back() == '}';
}

std::string classify_path(std::string_view path)
{
    const std::string upper_owned = upper_ascii(std::string(path));
    const std::string_view upper(upper_owned);
    std::string_view remainder;

    constexpr std::string_view interface_markers[] = {
        "\\CLASSES\\INTERFACE\\",
        "\\CLASSES\\WOW6432NODE\\INTERFACE\\"
    };
    for (const auto marker : interface_markers)
        if (immediate_child_after(upper, marker, &remainder) && guid_like(remainder)) return "component";

    constexpr std::string_view typelib_markers[] = {
        "\\CLASSES\\TYPELIB\\",
        "\\CLASSES\\WOW6432NODE\\TYPELIB\\"
    };
    for (const auto marker : typelib_markers)
        if (immediate_child_after(upper, marker, &remainder) && guid_like(remainder)) return "component";

    constexpr std::string_view appid_markers[] = {
        "\\CLASSES\\APPID\\",
        "\\CLASSES\\WOW6432NODE\\APPID\\"
    };
    for (const auto marker : appid_markers)
        if (immediate_child_after(upper, marker, &remainder)) return "component";

    if (immediate_child_after(upper, "\\CURRENTVERSION\\UNINSTALL\\", &remainder)) return "application";
    if (immediate_child_after(upper, "\\TCPIP\\PARAMETERS\\INTERFACES\\", &remainder) && guid_like(remainder)) return "network_configuration";

    if (upper.ends_with("\\ENVIRONMENT") || upper.find("\\ENVIRONMENT\\") != std::string_view::npos)
        return "environment_setting";

    if (upper.find("\\POLICIES\\") != std::string_view::npos || upper.ends_with("\\POLICIES"))
        return "security_policy";

    if (immediate_child_after(upper, "\\CLASSES\\", &remainder) && remainder.size() > 1U && remainder.front() == '.')
        return "file_association";

    return {};
}

std::string root_path(sqlite3* source, std::int64_t id)
{
    statement_t statement(source, "SELECT path FROM registry_keys WHERE id=?1;");
    sqlite3_bind_int64(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return {};
    return text(statement.get(), 0);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc < 2 || argc > 3)
            throw std::runtime_error("usage: nylon-semantic-classify.exe <image.db> [image.map.db]");
        const std::filesystem::path source_path(argv[1]);
        const std::filesystem::path map_path = argc == 3 ? std::filesystem::path(argv[2]) : map_path_for(source_path);

        auto source = database_t::open(source_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        auto map = database_t::open(map_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX);
        exec(source.get(), "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY;");
        exec(map.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");

        statement_t jobs(map.get(), "SELECT id,roots FROM lift_jobs WHERE status=0 AND deterministic_kind='' ORDER BY id;");
        statement_t update(map.get(), "UPDATE lift_jobs SET deterministic_kind=?2 WHERE id=?1 AND status=0 AND deterministic_kind='';");
        std::unordered_map<std::string, std::size_t> counts;
        std::size_t classified = 0;

        exec(map.get(), "BEGIN IMMEDIATE;");
        try
        {
            while (sqlite3_step(jobs.get()) == SQLITE_ROW)
            {
                const std::int64_t job_id = sqlite3_column_int64(jobs.get(), 0);
                const auto roots = parse_roots(text(jobs.get(), 1));
                std::string common_kind;
                bool valid = !roots.empty();
                for (const std::int64_t root : roots)
                {
                    const std::string kind = classify_path(root_path(source.get(), root));
                    if (kind.empty())
                    {
                        valid = false;
                        break;
                    }
                    if (common_kind.empty()) common_kind = kind;
                    else if (common_kind != kind)
                    {
                        valid = false;
                        break;
                    }
                }
                if (!valid || common_kind.empty()) continue;

                sqlite3_reset(update.get());
                sqlite3_clear_bindings(update.get());
                sqlite3_bind_int64(update.get(), 1, job_id);
                sqlite3_bind_text(update.get(), 2, common_kind.c_str(), static_cast<int>(common_kind.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(update.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map.get()));
                ++classified;
                ++counts[common_kind];
            }
            exec(map.get(), "COMMIT;");
        }
        catch (...)
        {
            try { exec(map.get(), "ROLLBACK;"); } catch (...) {}
            throw;
        }

        std::cout << "semantic preclassification: " << classified << " additional work units require no model";
        if (!counts.empty())
        {
            std::cout << " (";
            bool first = true;
            for (const auto& [kind, count] : counts)
            {
                if (!first) std::cout << ", ";
                first = false;
                std::cout << kind << '=' << count;
            }
            std::cout << ')';
        }
        std::cout << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
