#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
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
    database_t& operator=(database_t&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr) sqlite3_close_v2(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

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

std::int64_t scalar_i64(sqlite3* database, std::string_view sql)
{
    statement_t statement(database, sql);
    return sqlite3_step(statement.get()) == SQLITE_ROW ? sqlite3_column_int64(statement.get(), 0) : 0;
}

std::filesystem::path map_path_for(const std::filesystem::path& source)
{
    std::filesystem::path result = source;
    result.replace_extension(L".map.db");
    return result;
}

std::int64_t insert_node(
    sqlite3* map,
    std::string_view stable_key,
    std::optional<std::int64_t> parent,
    std::string_view local_id,
    std::string_view kind,
    std::string_view label,
    std::string_view summary)
{
    statement_t insert(map, "INSERT INTO semantic_nodes(stable_key,job_id,parent_id,local_id,kind,label,summary,confidence) VALUES(?1,0,?2,?3,?4,?5,?6,1.0);");
    sqlite3_bind_text(insert.get(), 1, stable_key.data(), static_cast<int>(stable_key.size()), SQLITE_TRANSIENT);
    if (parent.has_value()) sqlite3_bind_int64(insert.get(), 2, *parent); else sqlite3_bind_null(insert.get(), 2);
    sqlite3_bind_text(insert.get(), 3, local_id.data(), static_cast<int>(local_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 4, kind.data(), static_cast<int>(kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 5, label.data(), static_cast<int>(label.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 6, summary.data(), static_cast<int>(summary.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
    return sqlite3_last_insert_rowid(map);
}

void insert_edge(sqlite3* map, std::int64_t source, std::string_view relation, std::int64_t target)
{
    if (source == target) return;
    statement_t insert(map, "INSERT INTO semantic_edges(job_id,source_id,relation,target_id) VALUES(0,?1,?2,?3);");
    sqlite3_bind_int64(insert.get(), 1, source);
    sqlite3_bind_text(insert.get(), 2, relation.data(), static_cast<int>(relation.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 3, target);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

void insert_source(sqlite3* map, std::int64_t semantic_id, std::string_view source_ref)
{
    statement_t insert(map, "INSERT OR IGNORE INTO semantic_sources(semantic_id,source_ref) VALUES(?1,?2);");
    sqlite3_bind_int64(insert.get(), 1, semantic_id);
    sqlite3_bind_text(insert.get(), 2, source_ref.data(), static_cast<int>(source_ref.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
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
    if (result.empty()) throw std::runtime_error("Semantic job has no roots");
    return result;
}

std::optional<std::int64_t> first_root_id(std::string_view roots)
{
    const auto parsed = parse_roots(roots);
    if (parsed.empty()) return std::nullopt;
    return parsed.front();
}

std::optional<std::int64_t> hive_for_key(sqlite3* source, std::int64_t key_id)
{
    statement_t statement(source, "SELECT hive_id FROM registry_keys WHERE id=?1;");
    sqlite3_bind_int64(statement.get(), 1, key_id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int64(statement.get(), 0);
}

std::string key_path(sqlite3* source, std::int64_t key_id)
{
    statement_t statement(source, "SELECT path FROM registry_keys WHERE id=?1;");
    sqlite3_bind_int64(statement.get(), 1, key_id);
    return sqlite3_step(statement.get()) == SQLITE_ROW ? text(statement.get(), 0) : std::string{};
}

std::optional<std::int64_t> semantic_owner_for_ref(sqlite3* map, std::string_view source_ref, std::string_view preferred_path, std::int64_t exclude_job)
{
    const char* sql = R"SQL(
SELECT n.id
FROM semantic_sources s
JOIN semantic_nodes n ON n.id=s.semantic_id
LEFT JOIN lift_jobs j ON j.id=n.job_id
WHERE s.source_ref=?1
  AND n.job_id<>?2
ORDER BY
    CASE WHEN j.path=?3 THEN 0 ELSE 1 END,
    CASE WHEN n.local_id='group' THEN 0 ELSE 1 END,
    n.confidence DESC,
    n.id
LIMIT 1;
)SQL";
    statement_t statement(map, sql);
    sqlite3_bind_text(statement.get(), 1, source_ref.data(), static_cast<int>(source_ref.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 2, exclude_job);
    sqlite3_bind_text(statement.get(), 3, preferred_path.data(), static_cast<int>(preferred_path.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int64(statement.get(), 0);
}

void write_metadata(sqlite3* map, std::string_view key, std::string_view value)
{
    statement_t statement(map, "INSERT OR REPLACE INTO metadata(key,value) VALUES(?1,?2);");
    sqlite3_bind_text(statement.get(), 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

void attach_owned_sources(
    sqlite3* source,
    sqlite3* map,
    std::string_view mode,
    std::string_view roots_text,
    std::size_t value_offset,
    std::size_t value_limit,
    std::int64_t semantic_group)
{
    const auto roots = parse_roots(roots_text);
    if (mode == "own")
    {
        if (roots.size() != 1U) throw std::runtime_error("Invalid own semantic job root count");
        const std::int64_t key_id = roots.front();
        if (value_offset == 0U) insert_source(map, semantic_group, "rk:" + std::to_string(key_id));

        const std::string sql = value_limit == 0U
            ? "SELECT id FROM registry_values WHERE key_id=?1 ORDER BY id;"
            : "SELECT id FROM registry_values WHERE key_id=?1 ORDER BY id LIMIT ?2 OFFSET ?3;";
        statement_t values(source, sql);
        sqlite3_bind_int64(values.get(), 1, key_id);
        if (value_limit != 0U)
        {
            sqlite3_bind_int64(values.get(), 2, static_cast<sqlite3_int64>(value_limit));
            sqlite3_bind_int64(values.get(), 3, static_cast<sqlite3_int64>(value_offset));
        }
        while (sqlite3_step(values.get()) == SQLITE_ROW)
            insert_source(map, semantic_group, "rv:" + std::to_string(sqlite3_column_int64(values.get(), 0)));
        return;
    }

    if (mode != "subtree") throw std::runtime_error("Unknown semantic job mode while attaching sources");
    std::ostringstream seeds;
    for (std::size_t index = 0; index < roots.size(); ++index)
    {
        if (index != 0U) seeds << " UNION ALL ";
        seeds << "SELECT ?" << (index + 1U);
    }
    const std::string sql = "WITH RECURSIVE tree(id) AS (" + seeds.str() +
        " UNION ALL SELECT k.id FROM registry_keys k JOIN tree t ON k.parent_id=t.id) "
        "SELECT t.id,v.id FROM tree t LEFT JOIN registry_values v ON v.key_id=t.id ORDER BY t.id,v.id;";
    statement_t rows(source, sql);
    for (std::size_t index = 0; index < roots.size(); ++index)
        sqlite3_bind_int64(rows.get(), static_cast<int>(index + 1U), roots[index]);

    std::int64_t previous_key = -1;
    while (sqlite3_step(rows.get()) == SQLITE_ROW)
    {
        const std::int64_t key_id = sqlite3_column_int64(rows.get(), 0);
        if (key_id != previous_key)
        {
            previous_key = key_id;
            insert_source(map, semantic_group, "rk:" + std::to_string(key_id));
        }
        if (sqlite3_column_type(rows.get(), 1) != SQLITE_NULL)
            insert_source(map, semantic_group, "rv:" + std::to_string(sqlite3_column_int64(rows.get(), 1)));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc < 2 || argc > 3)
            throw std::runtime_error("usage: nylon-semantic-link.exe <image.db> [image.map.db]");
        const std::filesystem::path source_path(argv[1]);
        const std::filesystem::path map_path = argc == 3 ? std::filesystem::path(argv[2]) : map_path_for(source_path);

        auto source = database_t::open(source_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        auto map = database_t::open(map_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX);
        exec(source.get(), "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY;");
        exec(map.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");

        if (scalar_i64(map.get(), "SELECT COUNT(*) FROM lift_jobs WHERE status NOT IN (2,3);") != 0)
            throw std::runtime_error("Semantic map is not complete; refusing to build final hierarchy");

        exec(map.get(), "BEGIN IMMEDIATE;");
        try
        {
            exec(map.get(), "DELETE FROM semantic_edges WHERE job_id=0;");
            exec(map.get(), "DELETE FROM semantic_sources WHERE semantic_id IN (SELECT id FROM semantic_nodes WHERE job_id=0);");
            exec(map.get(), "UPDATE semantic_nodes SET parent_id=NULL WHERE job_id<>0 AND local_id='group';");
            exec(map.get(), "DELETE FROM semantic_nodes WHERE job_id=0;");

            const std::int64_t root = insert_node(map.get(), "system:registry", std::nullopt, "registry", "component", "Windows Registry", "Semantic map reconstructed from the offline Windows registry.");

            std::unordered_map<std::int64_t, std::int64_t> hive_nodes;
            statement_t hives(source.get(), "SELECT id,name FROM registry_hives ORDER BY id;");
            while (sqlite3_step(hives.get()) == SQLITE_ROW)
            {
                const std::int64_t hive_id = sqlite3_column_int64(hives.get(), 0);
                const std::string name = text(hives.get(), 1);
                const std::string stable = "system:hive:" + std::to_string(hive_id);
                const std::string local = "hive:" + std::to_string(hive_id);
                const std::int64_t node = insert_node(map.get(), stable, root, local, "configuration_group", name, "Offline Windows registry hive.");
                hive_nodes.emplace(hive_id, node);
                insert_edge(map.get(), root, "contains", node);
            }

            // First, attach every raw source object owned by every completed job to
            // that job's semantic group. This is independent of what the model chose
            // to cite, so source coverage is exact even for deterministic lifts or
            // deliberately unclassified evidence.
            const char* coverage_sql = R"SQL(
SELECT j.id,j.mode,j.roots,j.value_offset,j.value_limit,n.id
FROM lift_jobs j
JOIN semantic_nodes n ON n.job_id=j.id AND n.local_id='group'
WHERE j.status IN (2,3)
ORDER BY j.id;
)SQL";
            statement_t coverage_jobs(map.get(), coverage_sql);
            while (sqlite3_step(coverage_jobs.get()) == SQLITE_ROW)
            {
                const std::string mode = text(coverage_jobs.get(), 1);
                const std::string roots = text(coverage_jobs.get(), 2);
                const std::size_t value_offset = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(coverage_jobs.get(), 3)));
                const std::size_t value_limit = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(coverage_jobs.get(), 4)));
                const std::int64_t group_id = sqlite3_column_int64(coverage_jobs.get(), 5);
                attach_owned_sources(source.get(), map.get(), mode, roots, value_offset, value_limit, group_id);
            }

            const std::int64_t expected_sources =
                scalar_i64(source.get(), "SELECT COUNT(*) FROM registry_keys;") +
                scalar_i64(source.get(), "SELECT COUNT(*) FROM registry_values;");
            const std::int64_t linked_sources = scalar_i64(
                map.get(),
                "SELECT COUNT(DISTINCT source_ref) FROM semantic_sources WHERE source_ref LIKE 'rk:%' OR source_ref LIKE 'rv:%';");
            if (linked_sources != expected_sources)
                throw std::runtime_error("Semantic raw-source coverage invariant failed");

            // With complete source links in place, reconstruct cross-work-unit
            // containment deterministically from exact registry parentage.
            const char* jobs_sql = R"SQL(
SELECT j.id,j.parent_context_id,j.roots,j.path,n.id
FROM lift_jobs j
JOIN semantic_nodes n ON n.job_id=j.id AND n.local_id='group'
WHERE j.status IN (2,3)
ORDER BY j.id;
)SQL";
            statement_t jobs(map.get(), jobs_sql);
            std::size_t linked = 0;
            std::size_t fallback = 0;
            while (sqlite3_step(jobs.get()) == SQLITE_ROW)
            {
                const std::int64_t job_id = sqlite3_column_int64(jobs.get(), 0);
                const bool has_parent = sqlite3_column_type(jobs.get(), 1) != SQLITE_NULL;
                const std::int64_t parent_key = has_parent ? sqlite3_column_int64(jobs.get(), 1) : 0;
                const std::string roots = text(jobs.get(), 2);
                const std::int64_t group_id = sqlite3_column_int64(jobs.get(), 4);
                std::optional<std::int64_t> parent_semantic;

                if (has_parent)
                {
                    const std::string ref = "rk:" + std::to_string(parent_key);
                    parent_semantic = semantic_owner_for_ref(map.get(), ref, key_path(source.get(), parent_key), job_id);
                }

                if (!parent_semantic.has_value())
                {
                    const auto first = first_root_id(roots);
                    const auto hive = first.has_value() ? hive_for_key(source.get(), *first) : std::nullopt;
                    if (hive.has_value())
                    {
                        const auto found = hive_nodes.find(*hive);
                        if (found != hive_nodes.end()) parent_semantic = found->second;
                    }
                    ++fallback;
                }

                if (!parent_semantic.has_value()) parent_semantic = root;
                insert_edge(map.get(), *parent_semantic, "contains", group_id);
                statement_t parent_update(map.get(), "UPDATE semantic_nodes SET parent_id=?2 WHERE id=?1;");
                sqlite3_bind_int64(parent_update.get(), 1, group_id);
                sqlite3_bind_int64(parent_update.get(), 2, *parent_semantic);
                if (sqlite3_step(parent_update.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map.get()));
                ++linked;
            }

            write_metadata(map.get(), "source_coverage_complete", "1");
            write_metadata(map.get(), "source_ref_count", std::to_string(linked_sources));
            write_metadata(map.get(), "hierarchy_complete", "1");
            write_metadata(map.get(), "hierarchy_version", "1");
            write_metadata(map.get(), "hierarchy_link_count", std::to_string(linked));
            write_metadata(map.get(), "hierarchy_fallback_count", std::to_string(fallback));
            exec(map.get(), "COMMIT;");

            std::cout << "semantic hierarchy: " << linked << " work groups linked"
                      << " | " << fallback << " hive fallbacks"
                      << " | " << linked_sources << '/' << expected_sources << " raw sources represented\n";
        }
        catch (...)
        {
            try { exec(map.get(), "ROLLBACK;"); } catch (...) {}
            throw;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
