#include <sqlite3.h>

#include <algorithm>
#include <charconv>
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

void write_metadata(sqlite3* map, std::string_view key, std::string_view value)
{
    statement_t statement(map, "INSERT OR REPLACE INTO metadata(key,value) VALUES(?1,?2);");
    sqlite3_bind_text(statement.get(), 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

struct key_info_t
{
    std::int64_t id = 0;
    std::int64_t hive_id = 0;
    std::optional<std::int64_t> parent_id;
    std::vector<std::int64_t> children;
};

struct job_info_t
{
    std::int64_t id = 0;
    std::string mode;
    std::optional<std::int64_t> parent_context;
    std::vector<std::int64_t> roots;
    std::size_t value_offset = 0;
    std::size_t value_limit = 0;
    std::int64_t group_id = 0;
};

struct value_range_t
{
    std::size_t offset = 0;
    std::size_t limit = 0;
    std::int64_t group_id = 0;
};

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

        // Load the registry tree once. The old linker recursively queried this
        // tree once per work unit; this representation lets us assign ownership
        // with one linear walk over keys and one linear stream over values.
        std::unordered_map<std::int64_t, key_info_t> keys;
        keys.reserve(static_cast<std::size_t>(scalar_i64(source.get(), "SELECT COUNT(*) FROM registry_keys;")) * 2U + 1U);
        {
            statement_t rows(source.get(), "SELECT id,hive_id,parent_id FROM registry_keys ORDER BY id;");
            while (sqlite3_step(rows.get()) == SQLITE_ROW)
            {
                key_info_t key;
                key.id = sqlite3_column_int64(rows.get(), 0);
                key.hive_id = sqlite3_column_int64(rows.get(), 1);
                if (sqlite3_column_type(rows.get(), 2) != SQLITE_NULL) key.parent_id = sqlite3_column_int64(rows.get(), 2);
                keys.emplace(key.id, std::move(key));
            }
        }
        for (auto& [id, key] : keys)
        {
            (void)id;
            if (!key.parent_id.has_value()) continue;
            const auto parent = keys.find(*key.parent_id);
            if (parent != keys.end()) parent->second.children.push_back(key.id);
        }

        std::vector<job_info_t> jobs;
        {
            const char* sql = R"SQL(
SELECT j.id,j.mode,j.parent_context_id,j.roots,j.value_offset,j.value_limit,n.id
FROM lift_jobs j
JOIN semantic_nodes n ON n.job_id=j.id AND n.local_id='group'
WHERE j.status IN (2,3)
ORDER BY j.id;
)SQL";
            statement_t rows(map.get(), sql);
            while (sqlite3_step(rows.get()) == SQLITE_ROW)
            {
                job_info_t job;
                job.id = sqlite3_column_int64(rows.get(), 0);
                job.mode = text(rows.get(), 1);
                if (sqlite3_column_type(rows.get(), 2) != SQLITE_NULL) job.parent_context = sqlite3_column_int64(rows.get(), 2);
                job.roots = parse_roots(text(rows.get(), 3));
                job.value_offset = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(rows.get(), 4)));
                job.value_limit = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(rows.get(), 5)));
                job.group_id = sqlite3_column_int64(rows.get(), 6);
                jobs.push_back(std::move(job));
            }
        }

        std::unordered_map<std::int64_t, std::int64_t> key_owner;
        key_owner.reserve(keys.size() * 2U + 1U);
        std::unordered_map<std::int64_t, std::vector<value_range_t>> own_value_ranges;

        for (const auto& job : jobs)
        {
            if (job.mode == "own")
            {
                if (job.roots.size() != 1U) throw std::runtime_error("Invalid own semantic job root count");
                const std::int64_t key_id = job.roots.front();
                if (job.value_offset == 0U)
                {
                    if (!key_owner.emplace(key_id, job.group_id).second)
                        throw std::runtime_error("Semantic key ownership overlapped");
                }
                if (job.value_limit != 0U)
                    own_value_ranges[key_id].push_back(value_range_t{job.value_offset, job.value_limit, job.group_id});
                continue;
            }
            if (job.mode != "subtree") throw std::runtime_error("Unknown semantic job mode while linking");

            std::vector<std::int64_t> stack = job.roots;
            while (!stack.empty())
            {
                const std::int64_t key_id = stack.back();
                stack.pop_back();
                const auto found = keys.find(key_id);
                if (found == keys.end()) throw std::runtime_error("Semantic subtree referenced a missing registry key");
                if (!key_owner.emplace(key_id, job.group_id).second)
                    throw std::runtime_error("Semantic key ownership overlapped");
                stack.insert(stack.end(), found->second.children.begin(), found->second.children.end());
            }
        }

        if (key_owner.size() != keys.size())
            throw std::runtime_error("Semantic key ownership coverage invariant failed");
        for (auto& [key_id, ranges] : own_value_ranges)
        {
            (void)key_id;
            std::sort(ranges.begin(), ranges.end(), [](const value_range_t& left, const value_range_t& right) { return left.offset < right.offset; });
        }

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
                const std::int64_t node = insert_node(
                    map.get(), "system:hive:" + std::to_string(hive_id), root, "hive:" + std::to_string(hive_id),
                    "configuration_group", name, "Offline Windows registry hive.");
                hive_nodes.emplace(hive_id, node);
            }

            statement_t source_insert(map.get(), "INSERT OR IGNORE INTO semantic_sources(semantic_id,source_ref) VALUES(?1,?2);");
            auto add_source = [&](std::int64_t semantic_id, std::string_view ref)
            {
                sqlite3_reset(source_insert.get());
                sqlite3_clear_bindings(source_insert.get());
                sqlite3_bind_int64(source_insert.get(), 1, semantic_id);
                sqlite3_bind_text(source_insert.get(), 2, ref.data(), static_cast<int>(ref.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(source_insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map.get()));
            };

            for (const auto& [key_id, group_id] : key_owner)
                add_source(group_id, "rk:" + std::to_string(key_id));

            // Stream every registry value exactly once and route it to the job
            // that owns its key/subrange. No recursive per-job SQL is required.
            statement_t values(source.get(), "SELECT id,key_id FROM registry_values ORDER BY key_id,id;");
            std::int64_t current_key = -1;
            std::size_t ordinal = 0;
            std::size_t range_index = 0;
            const std::vector<value_range_t>* ranges = nullptr;
            while (sqlite3_step(values.get()) == SQLITE_ROW)
            {
                const std::int64_t value_id = sqlite3_column_int64(values.get(), 0);
                const std::int64_t key_id = sqlite3_column_int64(values.get(), 1);
                if (key_id != current_key)
                {
                    current_key = key_id;
                    ordinal = 0;
                    range_index = 0;
                    const auto found_ranges = own_value_ranges.find(key_id);
                    ranges = found_ranges == own_value_ranges.end() ? nullptr : &found_ranges->second;
                }

                std::int64_t group_id = 0;
                if (ranges != nullptr)
                {
                    while (range_index < ranges->size() &&
                           ordinal >= (*ranges)[range_index].offset + (*ranges)[range_index].limit)
                        ++range_index;
                    if (range_index >= ranges->size() || ordinal < (*ranges)[range_index].offset)
                        throw std::runtime_error("Semantic value range coverage invariant failed");
                    group_id = (*ranges)[range_index].group_id;
                }
                else
                {
                    const auto owner = key_owner.find(key_id);
                    if (owner == key_owner.end()) throw std::runtime_error("Semantic value owner was missing");
                    group_id = owner->second;
                }
                add_source(group_id, "rv:" + std::to_string(value_id));
                ++ordinal;
            }

            const std::int64_t expected_sources = static_cast<std::int64_t>(keys.size()) + scalar_i64(source.get(), "SELECT COUNT(*) FROM registry_values;");
            const std::int64_t linked_sources = scalar_i64(
                map.get(),
                "SELECT COUNT(DISTINCT source_ref) FROM semantic_sources WHERE source_ref LIKE 'rk:%' OR source_ref LIKE 'rv:%';");
            if (linked_sources != expected_sources)
                throw std::runtime_error("Semantic raw-source coverage invariant failed");

            statement_t edge_insert(map.get(), "INSERT INTO semantic_edges(job_id,source_id,relation,target_id) VALUES(0,?1,'contains',?2);");
            auto add_edge = [&](std::int64_t source_id, std::int64_t target_id)
            {
                if (source_id == target_id) return;
                sqlite3_reset(edge_insert.get());
                sqlite3_clear_bindings(edge_insert.get());
                sqlite3_bind_int64(edge_insert.get(), 1, source_id);
                sqlite3_bind_int64(edge_insert.get(), 2, target_id);
                if (sqlite3_step(edge_insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map.get()));
            };

            for (const auto& [hive_id, hive_node] : hive_nodes)
            {
                (void)hive_id;
                add_edge(root, hive_node);
            }

            statement_t parent_update(map.get(), "UPDATE semantic_nodes SET parent_id=?2 WHERE id=?1;");
            std::size_t linked = 0;
            std::size_t fallback = 0;
            for (const auto& job : jobs)
            {
                std::optional<std::int64_t> parent_semantic;
                if (job.parent_context.has_value())
                {
                    const auto owner = key_owner.find(*job.parent_context);
                    if (owner != key_owner.end() && owner->second != job.group_id) parent_semantic = owner->second;
                }
                if (!parent_semantic.has_value())
                {
                    const auto key = keys.find(job.roots.front());
                    if (key != keys.end())
                    {
                        const auto hive = hive_nodes.find(key->second.hive_id);
                        if (hive != hive_nodes.end()) parent_semantic = hive->second;
                    }
                    ++fallback;
                }
                if (!parent_semantic.has_value()) parent_semantic = root;

                add_edge(*parent_semantic, job.group_id);
                sqlite3_reset(parent_update.get());
                sqlite3_clear_bindings(parent_update.get());
                sqlite3_bind_int64(parent_update.get(), 1, job.group_id);
                sqlite3_bind_int64(parent_update.get(), 2, *parent_semantic);
                if (sqlite3_step(parent_update.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map.get()));
                ++linked;
            }

            write_metadata(map.get(), "source_coverage_complete", "1");
            write_metadata(map.get(), "source_ref_count", std::to_string(linked_sources));
            write_metadata(map.get(), "hierarchy_complete", "1");
            write_metadata(map.get(), "hierarchy_version", "2");
            write_metadata(map.get(), "hierarchy_link_count", std::to_string(linked));
            write_metadata(map.get(), "hierarchy_fallback_count", std::to_string(fallback));
            write_metadata(map.get(), "linker_runtime", "linear-v2");
            exec(map.get(), "COMMIT;");

            std::cout << "semantic hierarchy: " << linked << " work groups linked"
                      << " | " << fallback << " hive fallbacks"
                      << " | " << linked_sources << '/' << expected_sources << " raw sources represented"
                      << " | linear ownership pass\n";
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
