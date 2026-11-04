#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <sqlite3.h>
#include "source_binding.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

constexpr std::string_view model_name = "Qwen3.5-4B-Q8_0";
constexpr std::string_view model_sha256 = "5b25856e42534b000b5e06d7cfc10c756931f4dc66a4ca6ef08d835bd233a833";
constexpr std::string_view ontology_version = "1";
constexpr std::string_view planner_version = "1";
constexpr std::string_view prompt_version = "1";
constexpr INTERNET_PORT llama_port = 51827;
constexpr std::size_t planner_target_bytes = 14000U;
constexpr std::size_t planner_hard_bytes = 18000U;
constexpr std::size_t planner_max_sources = 100U;
constexpr std::size_t own_value_batch = 24U;
constexpr std::size_t prompt_hard_bytes = 24000U;
constexpr unsigned worker_count = 4U;

const std::unordered_set<std::string> semantic_kinds = {
    "configuration_group",
    "registry_configuration",
    "service",
    "driver",
    "application",
    "component",
    "executable",
    "library",
    "user",
    "group",
    "security_policy",
    "network_configuration",
    "device",
    "com_class",
    "shell_extension",
    "startup_entry",
    "scheduled_task",
    "certificate",
    "file_association",
    "protocol_handler",
    "environment_setting",
    "unknown_configuration"
};

const std::unordered_set<std::string> semantic_relations = {
    "contains",
    "configures",
    "launches",
    "loads",
    "depends_on",
    "belongs_to",
    "references",
    "signed_by",
    "registered_as",
    "runs_as",
    "starts_with",
    "associated_with",
    "inherits_from"
};

std::string path_utf8(const std::filesystem::path& path)
{
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("Unable to encode a filesystem path as UTF-8");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, result.data(), required, nullptr, nullptr) <= 0)
        throw std::runtime_error("Unable to encode a filesystem path as UTF-8");
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

class database_t
{
public:
    database_t() noexcept = default;
    ~database_t() { reset(); }
    database_t(const database_t&) = delete;
    database_t& operator=(const database_t&) = delete;
    database_t(database_t&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    database_t& operator=(database_t&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    static database_t open_readonly(const std::filesystem::path& path)
    {
        return open(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
    }

    static database_t open_map(const std::filesystem::path& path)
    {
        return open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX);
    }

    [[nodiscard]] sqlite3* get() const noexcept { return handle_; }

private:
    static database_t open(const std::filesystem::path& path, int flags)
    {
        sqlite3* handle = nullptr;
        const std::string utf8 = path_utf8(path);
        const int rc = sqlite3_open_v2(utf8.c_str(), &handle, flags, nullptr);
        if (rc != SQLITE_OK)
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

    void reset() noexcept
    {
        if (handle_ != nullptr) sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }

    sqlite3* handle_ = nullptr;
};

class statement_t
{
public:
    statement_t(sqlite3* database, std::string_view sql)
    {
        if (sqlite3_prepare_v2(database, std::string(sql).c_str(), -1, &handle_, nullptr) != SQLITE_OK)
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
    const int rc = sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK)
    {
        const std::string message = error != nullptr ? error : sqlite3_errmsg(database);
        if (error != nullptr) sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

std::string column_text(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite3_column_text(statement, column);
    if (text == nullptr) return {};
    const int size = sqlite3_column_bytes(statement, column);
    return size > 0 ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)) : std::string{};
}

std::optional<std::string> scalar_text(sqlite3* database, std::string_view sql)
{
    statement_t statement(database, sql);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return column_text(statement.get(), 0);
}

std::int64_t scalar_i64(sqlite3* database, std::string_view sql)
{
    statement_t statement(database, sql);
    return sqlite3_step(statement.get()) == SQLITE_ROW ? sqlite3_column_int64(statement.get(), 0) : 0;
}

std::string json_escape(std::string_view input)
{
    std::string output;
    output.reserve(input.size() + 32U);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : input)
    {
        switch (c)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 0x20U)
            {
                output += "\\u00";
                output.push_back(hex[(c >> 4U) & 0x0fU]);
                output.push_back(hex[c & 0x0fU]);
            }
            else
            {
                output.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return output;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string upper_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string sha256_hex(std::string_view input)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptGetProperty failed");
    }
    std::vector<unsigned char> object(object_size);
    std::array<unsigned char, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    const NTSTATUS data_status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0);
    const NTSTATUS finish_status = data_status == 0 ? BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) : data_status;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finish_status != 0) throw std::runtime_error("SHA-256 hashing failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64U);
    for (const unsigned char byte : digest)
    {
        output.push_back(hex[(byte >> 4U) & 0x0fU]);
        output.push_back(hex[byte & 0x0fU]);
    }
    return output;
}

std::filesystem::path default_database_path()
{
    std::wstring buffer(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::filesystem::path(L"output") / L"image.db";
    buffer.resize(length);
    const std::filesystem::path executable(buffer);
    const auto parent = executable.parent_path();
    if (_wcsicmp(parent.filename().c_str(), L"bin") == 0)
        return parent.parent_path() / L"output" / L"image.db";
    return parent / L"output" / L"image.db";
}

std::filesystem::path map_path_for(const std::filesystem::path& source)
{
    std::filesystem::path result = source;
    result.replace_extension(L".map.db");
    return result;
}

std::string source_fingerprint(sqlite3*, const std::filesystem::path& path)
{
    return nylon::semantic_binding::fingerprint(path);
}

bool map_compatible(const std::filesystem::path& path, std::string_view fingerprint)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    try
    {
        auto database = database_t::open_readonly(path);
        const auto fp = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='source_fingerprint';");
        const auto model = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='model_sha256';");
        const auto ontology = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='ontology_version';");
        const auto planner = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='planner_version';");
        const auto prompt = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='prompt_version';");
        return fp == fingerprint && model == model_sha256 && ontology == ontology_version && planner == planner_version && prompt == prompt_version;
    }
    catch (...)
    {
        return false;
    }
}

void remove_map_files(const std::filesystem::path& path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.wstring() + L"-wal", ignored);
    std::filesystem::remove(path.wstring() + L"-shm", ignored);
}

void initialize_map(sqlite3* database, std::string_view fingerprint, const std::filesystem::path& source_path, std::string_view audit_id = {})
{
    exec(database, R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;
CREATE TABLE IF NOT EXISTS metadata(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS lift_jobs(
    id INTEGER PRIMARY KEY,
    job_key TEXT NOT NULL UNIQUE,
    mode TEXT NOT NULL,
    parent_context_id INTEGER,
    roots TEXT NOT NULL,
    value_offset INTEGER NOT NULL DEFAULT 0,
    value_limit INTEGER NOT NULL DEFAULT 0,
    source_count INTEGER NOT NULL,
    estimated_bytes INTEGER NOT NULL,
    path TEXT NOT NULL,
    deterministic_kind TEXT NOT NULL DEFAULT '',
    status INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    input_hash TEXT NOT NULL DEFAULT '',
    error TEXT NOT NULL DEFAULT '',
    started_ms INTEGER,
    completed_ms INTEGER
);
CREATE INDEX IF NOT EXISTS lift_jobs_status_index ON lift_jobs(status,id);
CREATE TABLE IF NOT EXISTS semantic_nodes(
    id INTEGER PRIMARY KEY,
    stable_key TEXT NOT NULL UNIQUE,
    job_id INTEGER NOT NULL,
    parent_id INTEGER,
    local_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    label TEXT NOT NULL,
    summary TEXT NOT NULL,
    confidence REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS semantic_nodes_job_index ON semantic_nodes(job_id);
CREATE INDEX IF NOT EXISTS semantic_nodes_parent_index ON semantic_nodes(parent_id);
CREATE TABLE IF NOT EXISTS semantic_edges(
    id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    source_id INTEGER NOT NULL,
    relation TEXT NOT NULL,
    target_id INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS semantic_edges_job_index ON semantic_edges(job_id);
CREATE TABLE IF NOT EXISTS semantic_sources(
    semantic_id INTEGER NOT NULL,
    source_ref TEXT NOT NULL,
    PRIMARY KEY(semantic_id,source_ref)
);
CREATE INDEX IF NOT EXISTS semantic_sources_ref_index ON semantic_sources(source_ref);
CREATE TABLE IF NOT EXISTS unresolved(
    id INTEGER PRIMARY KEY,
    job_id INTEGER NOT NULL,
    text TEXT NOT NULL
);
)SQL");

    statement_t upsert(database, "INSERT OR REPLACE INTO metadata(key,value) VALUES(?1,?2);");
    const std::vector<std::pair<std::string, std::string>> values = {
        {"format_version", "1"},
        {"source_fingerprint", std::string(fingerprint)},
        {"source_binding_format", "raw-db-sha256-v1"},
        {"source_audit_id", std::string(audit_id)},
        {"source_path", path_utf8(std::filesystem::absolute(source_path))},
        {"model_name", std::string(model_name)},
        {"model_sha256", std::string(model_sha256)},
        {"ontology_version", std::string(ontology_version)},
        {"planner_version", std::string(planner_version)},
        {"prompt_version", std::string(prompt_version)}
    };
    for (const auto& [key, value] : values)
    {
        sqlite3_reset(upsert.get());
        sqlite3_clear_bindings(upsert.get());
        sqlite3_bind_text(upsert.get(), 1, key.c_str(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(upsert.get(), 2, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(upsert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database));
    }
}

struct key_node_t
{
    std::int64_t id = 0;
    std::int64_t hive_id = 0;
    std::optional<std::int64_t> parent_id;
    std::string name;
    std::string path;
    std::size_t value_count = 0;
    std::size_t direct_bytes = 0;
    std::size_t subtree_sources = 0;
    std::size_t subtree_bytes = 0;
    std::vector<std::size_t> children;
};

std::size_t path_depth(std::string_view path)
{
    return static_cast<std::size_t>(std::count(path.begin(), path.end(), '\\'));
}

std::string known_kind_for_path(std::string_view path)
{
    const std::string upper = upper_ascii(std::string(path));
    const std::size_t services = upper.find("\\SERVICES\\");
    if (services != std::string::npos)
    {
        const std::string_view remainder(upper.data() + services + 10U, upper.size() - services - 10U);
        if (!remainder.empty() && remainder.find('\\') == std::string_view::npos) return "service";
    }
    const std::size_t clsid = upper.find("\\CLSID\\{");
    if (clsid != std::string::npos)
    {
        const std::string_view remainder(upper.data() + clsid + 7U, upper.size() - clsid - 7U);
        if (!remainder.empty() && remainder.find('\\') == std::string_view::npos) return "com_class";
    }
    if (upper.ends_with("\\CURRENTVERSION\\RUN") || upper.ends_with("\\CURRENTVERSION\\RUNONCE")) return "startup_entry";
    if (upper.find("\\CONTROL\\CLASS\\{") != std::string::npos && upper.back() == '}') return "device";
    return {};
}

std::vector<std::int64_t> parse_roots(std::string_view text)
{
    std::vector<std::int64_t> result;
    std::size_t cursor = 0;
    while (cursor < text.size())
    {
        const std::size_t comma = text.find(',', cursor);
        const std::string_view part = text.substr(cursor, comma == std::string_view::npos ? text.size() - cursor : comma - cursor);
        std::int64_t id = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || id <= 0)
            throw std::runtime_error("Invalid planned root list");
        result.push_back(id);
        if (comma == std::string_view::npos) break;
        cursor = comma + 1U;
    }
    return result;
}

std::string join_roots(const std::vector<std::int64_t>& roots)
{
    std::ostringstream output;
    for (std::size_t i = 0; i < roots.size(); ++i)
    {
        if (i != 0) output << ',';
        output << roots[i];
    }
    return output.str();
}

struct planned_job_t
{
    std::string key;
    std::string mode;
    std::optional<std::int64_t> parent_context;
    std::vector<std::int64_t> roots;
    std::size_t value_offset = 0;
    std::size_t value_limit = 0;
    std::size_t source_count = 0;
    std::size_t estimated_bytes = 0;
    std::string path;
    std::string deterministic_kind;
};

std::string deterministic_kind_for_roots(const std::vector<std::int64_t>& roots, const std::unordered_map<std::int64_t, std::size_t>& by_id, const std::vector<key_node_t>& nodes)
{
    std::string common;
    for (const std::int64_t root : roots)
    {
        const auto found = by_id.find(root);
        if (found == by_id.end()) return {};
        const std::string kind = known_kind_for_path(nodes[found->second].path);
        if (kind.empty()) return {};
        if (common.empty()) common = kind;
        else if (common != kind) return {};
    }
    return common;
}

std::vector<planned_job_t> build_plan(sqlite3* source)
{
    const char* sql = R"SQL(
WITH value_stats AS (
    SELECT key_id,
           COUNT(*) AS value_count,
           COALESCE(SUM(64 + MIN(LENGTH(name),96) + MIN(LENGTH(COALESCE(text_value,'')),256)),0) AS estimated_bytes
    FROM registry_values
    GROUP BY key_id
)
SELECT k.id,k.hive_id,k.parent_id,k.name,k.path,
       COALESCE(v.value_count,0),
       96 + LENGTH(k.path) + LENGTH(k.name) + COALESCE(v.estimated_bytes,0)
FROM registry_keys k
LEFT JOIN value_stats v ON v.key_id=k.id
ORDER BY k.id;
)SQL";
    statement_t statement(source, sql);
    std::vector<key_node_t> nodes;
    std::unordered_map<std::int64_t, std::size_t> by_id;
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        key_node_t node;
        node.id = sqlite3_column_int64(statement.get(), 0);
        node.hive_id = sqlite3_column_int64(statement.get(), 1);
        if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL) node.parent_id = sqlite3_column_int64(statement.get(), 2);
        node.name = column_text(statement.get(), 3);
        node.path = column_text(statement.get(), 4);
        node.value_count = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 5)));
        node.direct_bytes = static_cast<std::size_t>(std::max<std::int64_t>(96, sqlite3_column_int64(statement.get(), 6)));
        node.subtree_sources = 1U + node.value_count;
        node.subtree_bytes = node.direct_bytes;
        by_id.emplace(node.id, nodes.size());
        nodes.push_back(std::move(node));
    }
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        if (!nodes[i].parent_id.has_value()) continue;
        const auto parent = by_id.find(*nodes[i].parent_id);
        if (parent != by_id.end()) nodes[parent->second].children.push_back(i);
    }
    std::vector<std::size_t> order(nodes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right)
    {
        return path_depth(nodes[left].path) > path_depth(nodes[right].path);
    });
    for (const std::size_t index : order)
    {
        if (!nodes[index].parent_id.has_value()) continue;
        const auto parent = by_id.find(*nodes[index].parent_id);
        if (parent == by_id.end()) continue;
        auto& target = nodes[parent->second];
        target.subtree_sources += nodes[index].subtree_sources;
        target.subtree_bytes += nodes[index].subtree_bytes;
    }

    std::vector<planned_job_t> jobs;
    auto add_job = [&](planned_job_t job)
    {
        job.deterministic_kind = deterministic_kind_for_roots(job.roots, by_id, nodes);
        jobs.push_back(std::move(job));
    };

    std::function<void(std::size_t, std::optional<std::int64_t>)> partition;
    partition = [&](std::size_t index, std::optional<std::int64_t> context)
    {
        const auto& node = nodes[index];
        if (node.subtree_bytes <= planner_hard_bytes && node.subtree_sources <= planner_max_sources)
        {
            planned_job_t job;
            job.mode = "subtree";
            job.parent_context = context;
            job.roots = {node.id};
            job.source_count = node.subtree_sources;
            job.estimated_bytes = node.subtree_bytes;
            job.path = node.path;
            job.key = "subtree:" + std::to_string(context.value_or(0)) + ":" + std::to_string(node.id);
            add_job(std::move(job));
            return;
        }

        if (node.value_count == 0U)
        {
            planned_job_t own;
            own.mode = "own";
            own.parent_context = context;
            own.roots = {node.id};
            own.source_count = 1U;
            own.estimated_bytes = std::min(node.direct_bytes, planner_hard_bytes);
            own.path = node.path;
            own.key = "own:" + std::to_string(node.id) + ":0:0";
            add_job(std::move(own));
        }
        else
        {
            for (std::size_t offset = 0; offset < node.value_count; offset += own_value_batch)
            {
                const std::size_t count = std::min(own_value_batch, node.value_count - offset);
                planned_job_t own;
                own.mode = "own";
                own.parent_context = context;
                own.roots = {node.id};
                own.value_offset = offset;
                own.value_limit = count;
                own.source_count = count + (offset == 0U ? 1U : 0U);
                const std::size_t value_bytes = node.value_count == 0U ? 0U : (node.direct_bytes * count) / node.value_count;
                own.estimated_bytes = std::min(planner_hard_bytes, 256U + value_bytes);
                own.path = node.path;
                own.key = "own:" + std::to_string(node.id) + ":" + std::to_string(offset) + ":" + std::to_string(count);
                add_job(std::move(own));
            }
        }

        std::vector<std::int64_t> packed_roots;
        std::size_t packed_bytes = 0;
        std::size_t packed_sources = 0;
        auto flush = [&]()
        {
            if (packed_roots.empty()) return;
            planned_job_t packed;
            packed.mode = "subtree";
            packed.parent_context = node.id;
            packed.roots = packed_roots;
            packed.source_count = packed_sources;
            packed.estimated_bytes = packed_bytes;
            packed.path = node.path;
            packed.key = "subtrees:" + std::to_string(node.id) + ":" + join_roots(packed_roots);
            add_job(std::move(packed));
            packed_roots.clear();
            packed_bytes = 0;
            packed_sources = 0;
        };

        for (const std::size_t child_index : node.children)
        {
            const auto& child = nodes[child_index];
            if (child.subtree_bytes > planner_hard_bytes || child.subtree_sources > planner_max_sources)
            {
                flush();
                partition(child_index, node.id);
                continue;
            }
            if (!packed_roots.empty() && (packed_bytes + child.subtree_bytes > planner_target_bytes || packed_sources + child.subtree_sources > planner_max_sources))
                flush();
            packed_roots.push_back(child.id);
            packed_bytes += child.subtree_bytes;
            packed_sources += child.subtree_sources;
        }
        flush();
    };

    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (!nodes[i].parent_id.has_value()) partition(i, std::nullopt);

    const std::uint64_t planned_sources = std::accumulate(jobs.begin(), jobs.end(), std::uint64_t{0}, [](std::uint64_t total, const planned_job_t& job)
    {
        return total + static_cast<std::uint64_t>(job.source_count);
    });
    const std::uint64_t expected_sources = static_cast<std::uint64_t>(scalar_i64(source, "SELECT COUNT(*) FROM registry_keys;")) +
                                           static_cast<std::uint64_t>(scalar_i64(source, "SELECT COUNT(*) FROM registry_values;"));
    if (planned_sources != expected_sources)
        throw std::runtime_error("Semantic planner coverage invariant failed");
    return jobs;
}

void persist_plan(sqlite3* map, const std::vector<planned_job_t>& jobs)
{
    exec(map, "BEGIN IMMEDIATE;");
    try
    {
        statement_t insert(map, R"SQL(
INSERT OR IGNORE INTO lift_jobs(
    job_key,mode,parent_context_id,roots,value_offset,value_limit,source_count,estimated_bytes,path,deterministic_kind
) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);
)SQL");
        for (const auto& job : jobs)
        {
            sqlite3_reset(insert.get());
            sqlite3_clear_bindings(insert.get());
            const std::string roots = join_roots(job.roots);
            sqlite3_bind_text(insert.get(), 1, job.key.c_str(), static_cast<int>(job.key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert.get(), 2, job.mode.c_str(), static_cast<int>(job.mode.size()), SQLITE_TRANSIENT);
            if (job.parent_context.has_value()) sqlite3_bind_int64(insert.get(), 3, *job.parent_context); else sqlite3_bind_null(insert.get(), 3);
            sqlite3_bind_text(insert.get(), 4, roots.c_str(), static_cast<int>(roots.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert.get(), 5, static_cast<sqlite3_int64>(job.value_offset));
            sqlite3_bind_int64(insert.get(), 6, static_cast<sqlite3_int64>(job.value_limit));
            sqlite3_bind_int64(insert.get(), 7, static_cast<sqlite3_int64>(job.source_count));
            sqlite3_bind_int64(insert.get(), 8, static_cast<sqlite3_int64>(job.estimated_bytes));
            sqlite3_bind_text(insert.get(), 9, job.path.c_str(), static_cast<int>(job.path.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert.get(), 10, job.deterministic_kind.c_str(), static_cast<int>(job.deterministic_kind.size()), SQLITE_TRANSIENT);
            if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
        }
        exec(map, "COMMIT;");
    }
    catch (...)
    {
        try { exec(map, "ROLLBACK;"); } catch (...) {}
        throw;
    }
}

struct job_t
{
    std::int64_t id = 0;
    std::string key;
    std::string mode;
    std::optional<std::int64_t> parent_context;
    std::vector<std::int64_t> roots;
    std::size_t value_offset = 0;
    std::size_t value_limit = 0;
    std::size_t source_count = 0;
    std::string path;
    std::string deterministic_kind;
};

job_t load_job(sqlite3* map, std::int64_t id)
{
    statement_t statement(map, "SELECT id,job_key,mode,parent_context_id,roots,value_offset,value_limit,source_count,path,deterministic_kind FROM lift_jobs WHERE id=?1;");
    sqlite3_bind_int64(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error("Semantic job disappeared");
    job_t job;
    job.id = sqlite3_column_int64(statement.get(), 0);
    job.key = column_text(statement.get(), 1);
    job.mode = column_text(statement.get(), 2);
    if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL) job.parent_context = sqlite3_column_int64(statement.get(), 3);
    job.roots = parse_roots(column_text(statement.get(), 4));
    job.value_offset = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 5)));
    job.value_limit = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 6)));
    job.source_count = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(statement.get(), 7)));
    job.path = column_text(statement.get(), 8);
    job.deterministic_kind = column_text(statement.get(), 9);
    return job;
}

std::string registry_type_name(std::int64_t type)
{
    switch (type)
    {
    case 0: return "REG_NONE";
    case 1: return "REG_SZ";
    case 2: return "REG_EXPAND_SZ";
    case 3: return "REG_BINARY";
    case 4: return "REG_DWORD";
    case 5: return "REG_DWORD_BIG_ENDIAN";
    case 6: return "REG_LINK";
    case 7: return "REG_MULTI_SZ";
    case 8: return "REG_RESOURCE_LIST";
    case 9: return "REG_FULL_RESOURCE_DESCRIPTOR";
    case 10: return "REG_RESOURCE_REQUIREMENTS_LIST";
    case 11: return "REG_QWORD";
    default: return "REG_UNKNOWN";
    }
}

struct prompt_t
{
    std::string text;
    std::unordered_set<std::string> allowed_refs;
    std::vector<std::string> root_refs;
};

void append_key_values(sqlite3* source, std::ostringstream& evidence, std::unordered_set<std::string>& refs, std::int64_t key_id, std::size_t offset, std::size_t limit, bool include_key)
{
    statement_t key(source, "SELECT h.name,k.name,k.path FROM registry_keys k JOIN registry_hives h ON h.id=k.hive_id WHERE k.id=?1;");
    sqlite3_bind_int64(key.get(), 1, key_id);
    if (sqlite3_step(key.get()) != SQLITE_ROW) throw std::runtime_error("Registry key missing while building semantic prompt");
    if (include_key)
    {
        evidence << "KEY rk:" << key_id << " hive=" << column_text(key.get(), 0) << " path=" << column_text(key.get(), 2) << " name=" << column_text(key.get(), 1) << '\n';
        refs.insert("rk:" + std::to_string(key_id));
    }
    const std::string sql = limit == 0U
        ? "SELECT id,name,value_type,length(data),substr(COALESCE(text_value,''),1,256),hex(substr(data,1,16)) FROM registry_values WHERE key_id=?1 ORDER BY id;"
        : "SELECT id,name,value_type,length(data),substr(COALESCE(text_value,''),1,256),hex(substr(data,1,16)) FROM registry_values WHERE key_id=?1 ORDER BY id LIMIT ?2 OFFSET ?3;";
    statement_t values(source, sql);
    sqlite3_bind_int64(values.get(), 1, key_id);
    if (limit != 0U)
    {
        sqlite3_bind_int64(values.get(), 2, static_cast<sqlite3_int64>(limit));
        sqlite3_bind_int64(values.get(), 3, static_cast<sqlite3_int64>(offset));
    }
    while (sqlite3_step(values.get()) == SQLITE_ROW)
    {
        const std::int64_t value_id = sqlite3_column_int64(values.get(), 0);
        const std::string ref = "rv:" + std::to_string(value_id);
        refs.insert(ref);
        evidence << "  VALUE " << ref
                 << " name=" << (column_text(values.get(), 1).empty() ? "(Default)" : column_text(values.get(), 1))
                 << " type=" << registry_type_name(sqlite3_column_int64(values.get(), 2))
                 << " bytes=" << sqlite3_column_int64(values.get(), 3);
        const std::string text = column_text(values.get(), 4);
        if (!text.empty()) evidence << " text=" << text;
        else evidence << " hex16=" << column_text(values.get(), 5);
        evidence << '\n';
    }
}

prompt_t build_prompt(sqlite3* source, const job_t& job)
{
    prompt_t result;
    std::ostringstream evidence;
    if (job.parent_context.has_value())
    {
        statement_t parent(source, "SELECT h.name,k.path,k.name FROM registry_keys k JOIN registry_hives h ON h.id=k.hive_id WHERE k.id=?1;");
        sqlite3_bind_int64(parent.get(), 1, *job.parent_context);
        if (sqlite3_step(parent.get()) == SQLITE_ROW)
        {
            const std::string ref = "rk:" + std::to_string(*job.parent_context);
            result.allowed_refs.insert(ref);
            evidence << "PARENT_CONTEXT " << ref << " hive=" << column_text(parent.get(), 0) << " path=" << column_text(parent.get(), 1) << " name=" << column_text(parent.get(), 2) << '\n';
        }
    }

    if (job.mode == "own")
    {
        if (job.roots.size() != 1U) throw std::runtime_error("Invalid own semantic job");
        result.root_refs.push_back("rk:" + std::to_string(job.roots.front()));
        append_key_values(source, evidence, result.allowed_refs, job.roots.front(), job.value_offset, job.value_limit, job.value_offset == 0U);
    }
    else if (job.mode == "subtree")
    {
        if (job.roots.empty()) throw std::runtime_error("Semantic subtree job has no roots");
        std::ostringstream seeds;
        for (std::size_t i = 0; i < job.roots.size(); ++i)
        {
            if (i != 0) seeds << " UNION ALL ";
            seeds << "SELECT ?" << (i + 1U);
            result.root_refs.push_back("rk:" + std::to_string(job.roots[i]));
        }
        const std::string sql = "WITH RECURSIVE tree(id) AS (" + seeds.str() +
            " UNION ALL SELECT k.id FROM registry_keys k JOIN tree t ON k.parent_id=t.id) "
            "SELECT k.id,h.name,k.path,k.name,v.id,v.name,v.value_type,length(v.data),substr(COALESCE(v.text_value,''),1,256),hex(substr(v.data,1,16)) "
            "FROM tree t JOIN registry_keys k ON k.id=t.id JOIN registry_hives h ON h.id=k.hive_id "
            "LEFT JOIN registry_values v ON v.key_id=k.id ORDER BY k.path,v.id;";
        statement_t rows(source, sql);
        for (std::size_t i = 0; i < job.roots.size(); ++i) sqlite3_bind_int64(rows.get(), static_cast<int>(i + 1U), job.roots[i]);
        std::int64_t current_key = -1;
        while (sqlite3_step(rows.get()) == SQLITE_ROW)
        {
            const std::int64_t key_id = sqlite3_column_int64(rows.get(), 0);
            if (key_id != current_key)
            {
                current_key = key_id;
                const std::string key_ref = "rk:" + std::to_string(key_id);
                result.allowed_refs.insert(key_ref);
                evidence << "KEY " << key_ref << " hive=" << column_text(rows.get(), 1) << " path=" << column_text(rows.get(), 2) << " name=" << column_text(rows.get(), 3) << '\n';
            }
            if (sqlite3_column_type(rows.get(), 4) != SQLITE_NULL)
            {
                const std::int64_t value_id = sqlite3_column_int64(rows.get(), 4);
                const std::string value_ref = "rv:" + std::to_string(value_id);
                result.allowed_refs.insert(value_ref);
                const std::string value_name = column_text(rows.get(), 5);
                evidence << "  VALUE " << value_ref
                         << " name=" << (value_name.empty() ? "(Default)" : value_name)
                         << " type=" << registry_type_name(sqlite3_column_int64(rows.get(), 6))
                         << " bytes=" << sqlite3_column_int64(rows.get(), 7);
                const std::string text = column_text(rows.get(), 8);
                if (!text.empty()) evidence << " text=" << text;
                else evidence << " hex16=" << column_text(rows.get(), 9);
                evidence << '\n';
            }
        }
    }
    else
    {
        throw std::runtime_error("Unknown semantic job mode");
    }

    std::ostringstream prompt;
    prompt << "Lift this bounded Windows registry evidence into a neutral semantic representation.\n"
           << "This is structural normalization, not malware analysis. Do not assign risk or infer facts not present.\n"
           << "Choose semantic kinds only from the allowed ontology. Keep labels short and human-readable.\n"
           << "Every entity source_refs entry must exactly match a source reference shown below.\n"
           << "Use unknown_configuration when evidence is ambiguous.\n"
           << "Relationships may reference local entity IDs or the literal local ID group.\n\n"
           << "EVIDENCE\n" << evidence.str();
    result.text = prompt.str();
    if (result.text.size() > prompt_hard_bytes)
        throw std::runtime_error("Semantic planner produced an oversized prompt; planner thresholds need adjustment");
    return result;
}

std::string output_schema_json()
{
    return R"JSON({"type":"object","additionalProperties":false,"properties":{"group_kind":{"type":"string","enum":["configuration_group","registry_configuration","service","driver","application","component","executable","library","user","group","security_policy","network_configuration","device","com_class","shell_extension","startup_entry","scheduled_task","certificate","file_association","protocol_handler","environment_setting","unknown_configuration"]},"group_label":{"type":"string","maxLength":96},"summary":{"type":"string","maxLength":320},"entities":{"type":"array","maxItems":16,"items":{"type":"object","additionalProperties":false,"properties":{"local_id":{"type":"string","maxLength":24},"kind":{"type":"string","enum":["registry_configuration","service","driver","application","component","executable","library","user","group","security_policy","network_configuration","device","com_class","shell_extension","startup_entry","scheduled_task","certificate","file_association","protocol_handler","environment_setting","unknown_configuration"]},"label":{"type":"string","maxLength":96},"summary":{"type":"string","maxLength":240},"source_refs":{"type":"array","maxItems":64,"items":{"type":"string","maxLength":32}}},"required":["local_id","kind","label","summary","source_refs"]}},"relationships":{"type":"array","maxItems":24,"items":{"type":"object","additionalProperties":false,"properties":{"from":{"type":"string","maxLength":24},"relation":{"type":"string","enum":["contains","configures","launches","loads","depends_on","belongs_to","references","signed_by","registered_as","runs_as","starts_with","associated_with","inherits_from"]},"to":{"type":"string","maxLength":24}},"required":["from","relation","to"]}},"unresolved":{"type":"array","maxItems":16,"items":{"type":"string","maxLength":160}}},"required":["group_kind","group_label","summary","entities","relationships","unresolved"]})JSON";
}

std::string system_prompt()
{
    return "You are the Nylon Semantic Lifter. Convert bounded offline Windows registry evidence into compact semantic structure. "
           "You are not a security verdict engine. Never invent missing files, users, services, or relationships. "
           "Return one JSON object with fields group_kind, group_label, summary, entities, relationships, unresolved. "
           "Entity local_id values must be short unique IDs such as e1, e2. The reserved local_id group refers to the returned group. "
           "Use source_refs to ground entities in exact rk:/rv: references from the evidence. Prefer unknown_configuration over guessing.";
}

class internet_handle_t
{
public:
    internet_handle_t() noexcept = default;
    explicit internet_handle_t(HINTERNET value) noexcept : value_(value) {}
    ~internet_handle_t() { if (value_ != nullptr) WinHttpCloseHandle(value_); }
    internet_handle_t(const internet_handle_t&) = delete;
    internet_handle_t& operator=(const internet_handle_t&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
private:
    HINTERNET value_ = nullptr;
};

struct http_result_t
{
    DWORD status = 0;
    std::string body;
};

http_result_t http_request(std::wstring_view method, std::wstring_view path, std::string_view body = {})
{
    internet_handle_t session(WinHttpOpen(L"NylonSemantic/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr) throw std::runtime_error("WinHttpOpen failed");
    WinHttpSetTimeouts(session.get(), 5000, 5000, 120000, 120000);
    internet_handle_t connection(WinHttpConnect(session.get(), L"127.0.0.1", llama_port, 0));
    if (connection.get() == nullptr) throw std::runtime_error("WinHttpConnect failed");
    internet_handle_t request(WinHttpOpenRequest(connection.get(), std::wstring(method).c_str(), std::wstring(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (request.get() == nullptr) throw std::runtime_error("WinHttpOpenRequest failed");
    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
    const DWORD header_length = body.empty() ? 0U : static_cast<DWORD>(-1L);
    LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD data_size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), headers, header_length, data, data_size, data_size, 0) || !WinHttpReceiveResponse(request.get(), nullptr))
        throw std::runtime_error("Local llama.cpp request failed");
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Unable to read llama.cpp HTTP status");
    std::string response;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throw std::runtime_error("Unable to read llama.cpp response");
        if (available == 0U) break;
        const std::size_t old_size = response.size();
        response.resize(old_size + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.data() + old_size, available, &read)) throw std::runtime_error("Unable to read llama.cpp response");
        response.resize(old_size + read);
    }
    return http_result_t{status, std::move(response)};
}

bool llama_healthy()
{
    try
    {
        const auto result = http_request(L"GET", L"/health");
        return result.status >= 200U && result.status < 300U;
    }
    catch (...)
    {
        return false;
    }
}

std::filesystem::path semantic_tools_root()
{
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local == nullptr || *local == L'\0') throw std::runtime_error("LOCALAPPDATA is not available");
    return std::filesystem::path(local) / L"Temp" / L"nylon" / L"tools" / L"semantic";
}

class process_guard_t
{
public:
    process_guard_t() noexcept = default;
    ~process_guard_t()
    {
        if (process_ != nullptr)
        {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
        }
    }
    process_guard_t(const process_guard_t&) = delete;
    process_guard_t& operator=(const process_guard_t&) = delete;
    void set(HANDLE process) noexcept { process_ = process; }
private:
    HANDLE process_ = nullptr;
};

void ensure_llama_server(process_guard_t& owned_process)
{
    if (llama_healthy()) return;
    const auto tools = semantic_tools_root();
    const auto server = tools / L"llama" / L"b11146" / L"llama-server.exe";
    const auto model = tools / L"models" / L"qwen3.5-4b-q8_0" / L"Qwen3.5-4B-Q8_0.gguf";
    const auto verified = tools / L"models" / L"qwen3.5-4b-q8_0" / L".nylon-verified";
    if (!std::filesystem::is_regular_file(server) || !std::filesystem::is_regular_file(model) || !std::filesystem::is_regular_file(verified))
        throw std::runtime_error("Semantic runtime is not installed. Run semantic-setup.cmd first.");

    std::wstring command = L"\"" + server.wstring() + L"\" -m \"" + model.wstring() +
        L"\" --host 127.0.0.1 --port " + std::to_wstring(llama_port) +
        L" -ngl 999 -c 32768 -np 4 -b 2048 -ub 512 -cb --no-warmup";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("Unable to start pinned llama.cpp Vulkan server");
    CloseHandle(process.hThread);
    owned_process.set(process.hProcess);
    for (int i = 0; i < 120; ++i)
    {
        if (llama_healthy()) return;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            throw std::runtime_error("llama.cpp exited while loading the semantic model");
        Sleep(500);
    }
    throw std::runtime_error("llama.cpp did not become ready");
}

std::string build_request(const prompt_t& prompt)
{
    const std::string schema = output_schema_json();
    std::ostringstream request;
    request << "{\"messages\":[{\"role\":\"system\",\"content\":\"" << json_escape(system_prompt())
            << "\"},{\"role\":\"user\",\"content\":\"" << json_escape(prompt.text)
            << "\"}],\"temperature\":0,\"max_tokens\":512,\"stream\":false,"
            << "\"chat_template_kwargs\":{\"enable_thinking\":false},"
            << "\"response_format\":{\"type\":\"json_schema\",\"schema\":" << schema << "}}";
    return request.str();
}

std::string extract_model_content(sqlite3* database, std::string_view response)
{
    statement_t statement(database, "SELECT json_extract(?1,'$.choices[0].message.content') WHERE json_valid(?1);");
    sqlite3_bind_text(statement.get(), 1, response.data(), static_cast<int>(response.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) == SQLITE_NULL)
        throw std::runtime_error("llama.cpp returned an invalid chat-completion response");
    return column_text(statement.get(), 0);
}

std::string json_scalar_text(sqlite3* database, std::string_view json, std::string_view path)
{
    statement_t statement(database, "SELECT json_extract(?1,?2) WHERE json_valid(?1);");
    sqlite3_bind_text(statement.get(), 1, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) == SQLITE_NULL) return {};
    return column_text(statement.get(), 0);
}

void delete_job_output(sqlite3* map, std::int64_t job_id)
{
    statement_t edges(map, "DELETE FROM semantic_edges WHERE job_id=?1;");
    sqlite3_bind_int64(edges.get(), 1, job_id);
    if (sqlite3_step(edges.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
    statement_t sources(map, "DELETE FROM semantic_sources WHERE semantic_id IN (SELECT id FROM semantic_nodes WHERE job_id=?1);");
    sqlite3_bind_int64(sources.get(), 1, job_id);
    if (sqlite3_step(sources.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
    statement_t nodes(map, "DELETE FROM semantic_nodes WHERE job_id=?1;");
    sqlite3_bind_int64(nodes.get(), 1, job_id);
    if (sqlite3_step(nodes.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
    statement_t unresolved(map, "DELETE FROM unresolved WHERE job_id=?1;");
    sqlite3_bind_int64(unresolved.get(), 1, job_id);
    if (sqlite3_step(unresolved.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

std::int64_t insert_semantic_node(sqlite3* map, std::int64_t job_id, std::optional<std::int64_t> parent, std::string_view local_id, std::string_view kind, std::string_view label, std::string_view summary, double confidence)
{
    const std::string stable = "j" + std::to_string(job_id) + ":" + std::string(local_id);
    statement_t insert(map, "INSERT INTO semantic_nodes(stable_key,job_id,parent_id,local_id,kind,label,summary,confidence) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);");
    sqlite3_bind_text(insert.get(), 1, stable.c_str(), static_cast<int>(stable.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 2, job_id);
    if (parent.has_value()) sqlite3_bind_int64(insert.get(), 3, *parent); else sqlite3_bind_null(insert.get(), 3);
    sqlite3_bind_text(insert.get(), 4, local_id.data(), static_cast<int>(local_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 5, kind.data(), static_cast<int>(kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 6, label.data(), static_cast<int>(label.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 7, summary.data(), static_cast<int>(summary.size()), SQLITE_TRANSIENT);
    sqlite3_bind_double(insert.get(), 8, confidence);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
    return sqlite3_last_insert_rowid(map);
}

void insert_edge(sqlite3* map, std::int64_t job_id, std::int64_t source, std::string_view relation, std::int64_t target)
{
    statement_t insert(map, "INSERT INTO semantic_edges(job_id,source_id,relation,target_id) VALUES(?1,?2,?3,?4);");
    sqlite3_bind_int64(insert.get(), 1, job_id);
    sqlite3_bind_int64(insert.get(), 2, source);
    sqlite3_bind_text(insert.get(), 3, relation.data(), static_cast<int>(relation.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 4, target);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

void insert_source(sqlite3* map, std::int64_t semantic_id, std::string_view ref)
{
    statement_t insert(map, "INSERT OR IGNORE INTO semantic_sources(semantic_id,source_ref) VALUES(?1,?2);");
    sqlite3_bind_int64(insert.get(), 1, semantic_id);
    sqlite3_bind_text(insert.get(), 2, ref.data(), static_cast<int>(ref.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

std::string root_label(sqlite3* source, std::int64_t key_id)
{
    statement_t statement(source, "SELECT name FROM registry_keys WHERE id=?1;");
    sqlite3_bind_int64(statement.get(), 1, key_id);
    return sqlite3_step(statement.get()) == SQLITE_ROW ? column_text(statement.get(), 0) : std::string("Registry configuration");
}

void mark_job(sqlite3* map, std::int64_t job_id, int status, std::string_view error = {})
{
    statement_t update(map, "UPDATE lift_jobs SET status=?2,error=?3,completed_ms=CASE WHEN ?2 IN (2,3,4) THEN unixepoch('subsec')*1000 ELSE completed_ms END WHERE id=?1;");
    sqlite3_bind_int64(update.get(), 1, job_id);
    sqlite3_bind_int(update.get(), 2, status);
    sqlite3_bind_text(update.get(), 3, error.data(), static_cast<int>(error.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(update.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

void apply_deterministic(sqlite3* source, sqlite3* map, const job_t& job)
{
    exec(map, "BEGIN IMMEDIATE;");
    try
    {
        delete_job_output(map, job.id);
        std::string group_label;
        if (job.deterministic_kind == "service") group_label = job.roots.size() == 1U ? "Windows service" : "Windows services";
        else if (job.deterministic_kind == "com_class") group_label = job.roots.size() == 1U ? "COM registration" : "COM registrations";
        else if (job.deterministic_kind == "startup_entry") group_label = "Startup configuration";
        else if (job.deterministic_kind == "device") group_label = "Device configuration";
        else group_label = "Registry configuration";
        const std::int64_t group_id = insert_semantic_node(map, job.id, std::nullopt, "group", "configuration_group", group_label, "Deterministically recognized Windows registry structure.", 1.0);
        for (std::size_t i = 0; i < job.roots.size(); ++i)
        {
            const std::string local = "e" + std::to_string(i + 1U);
            const std::string label = root_label(source, job.roots[i]);
            const std::int64_t entity = insert_semantic_node(map, job.id, group_id, local, job.deterministic_kind, label, "Known Windows registry structure.", 1.0);
            insert_edge(map, job.id, group_id, "contains", entity);
            insert_source(map, entity, "rk:" + std::to_string(job.roots[i]));
        }
        mark_job(map, job.id, 2);
        exec(map, "COMMIT;");
    }
    catch (...)
    {
        try { exec(map, "ROLLBACK;"); } catch (...) {}
        throw;
    }
}

void apply_ai_delta(sqlite3* map, const job_t& job, const prompt_t& prompt, std::string_view delta)
{
    statement_t valid(map, "SELECT json_valid(?1);");
    sqlite3_bind_text(valid.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(valid.get()) != SQLITE_ROW || sqlite3_column_int(valid.get(), 0) == 0) throw std::runtime_error("Model delta was not valid JSON");

    const std::string group_kind = json_scalar_text(map, delta, "$.group_kind");
    const std::string group_label = json_scalar_text(map, delta, "$.group_label");
    const std::string summary = json_scalar_text(map, delta, "$.summary");
    if (!semantic_kinds.contains(group_kind) || group_label.empty()) throw std::runtime_error("Model delta failed ontology validation");

    std::unordered_set<std::string> local_ids = {"group"};
    statement_t entities_validate(map, "SELECT json_extract(value,'$.local_id'),json_extract(value,'$.kind') FROM json_each(?1,'$.entities');");
    sqlite3_bind_text(entities_validate.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(entities_validate.get()) == SQLITE_ROW)
    {
        const std::string local = column_text(entities_validate.get(), 0);
        const std::string kind = column_text(entities_validate.get(), 1);
        if (local.empty() || local == "group" || !local_ids.insert(local).second || !semantic_kinds.contains(kind))
            throw std::runtime_error("Model entity failed local-ID or ontology validation");
    }

    statement_t refs_validate(map, "SELECT ref.value FROM json_each(?1,'$.entities') entity,json_each(entity.value,'$.source_refs') ref;");
    sqlite3_bind_text(refs_validate.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(refs_validate.get()) == SQLITE_ROW)
    {
        const std::string ref = column_text(refs_validate.get(), 0);
        if (!prompt.allowed_refs.contains(ref)) throw std::runtime_error("Model referenced evidence outside its work unit");
    }

    statement_t relations_validate(map, "SELECT json_extract(value,'$.from'),json_extract(value,'$.relation'),json_extract(value,'$.to') FROM json_each(?1,'$.relationships');");
    sqlite3_bind_text(relations_validate.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(relations_validate.get()) == SQLITE_ROW)
    {
        const std::string from = column_text(relations_validate.get(), 0);
        const std::string relation = column_text(relations_validate.get(), 1);
        const std::string to = column_text(relations_validate.get(), 2);
        if (!local_ids.contains(from) || !local_ids.contains(to) || !semantic_relations.contains(relation))
            throw std::runtime_error("Model relationship failed validation");
    }

    exec(map, "BEGIN IMMEDIATE;");
    try
    {
        delete_job_output(map, job.id);
        const std::int64_t group_id = insert_semantic_node(map, job.id, std::nullopt, "group", group_kind, group_label, summary, 0.80);
        for (const auto& ref : prompt.root_refs) insert_source(map, group_id, ref);
        std::unordered_map<std::string, std::int64_t> entity_ids;
        entity_ids.emplace("group", group_id);

        statement_t entities(map, "SELECT json_extract(value,'$.local_id'),json_extract(value,'$.kind'),json_extract(value,'$.label'),json_extract(value,'$.summary'),value FROM json_each(?1,'$.entities');");
        sqlite3_bind_text(entities.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(entities.get()) == SQLITE_ROW)
        {
            const std::string local = column_text(entities.get(), 0);
            const std::string kind = column_text(entities.get(), 1);
            const std::string label = column_text(entities.get(), 2);
            const std::string entity_summary = column_text(entities.get(), 3);
            const std::string entity_json = column_text(entities.get(), 4);
            const std::int64_t entity_id = insert_semantic_node(map, job.id, group_id, local, kind, label, entity_summary, 0.80);
            entity_ids.emplace(local, entity_id);
            insert_edge(map, job.id, group_id, "contains", entity_id);
            statement_t refs(map, "SELECT value FROM json_each(?1,'$.source_refs');");
            sqlite3_bind_text(refs.get(), 1, entity_json.c_str(), static_cast<int>(entity_json.size()), SQLITE_TRANSIENT);
            while (sqlite3_step(refs.get()) == SQLITE_ROW) insert_source(map, entity_id, column_text(refs.get(), 0));
        }

        statement_t relations(map, "SELECT json_extract(value,'$.from'),json_extract(value,'$.relation'),json_extract(value,'$.to') FROM json_each(?1,'$.relationships');");
        sqlite3_bind_text(relations.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(relations.get()) == SQLITE_ROW)
        {
            const std::string from = column_text(relations.get(), 0);
            const std::string relation = column_text(relations.get(), 1);
            const std::string to = column_text(relations.get(), 2);
            if (relation == "contains" && from == "group") continue;
            insert_edge(map, job.id, entity_ids.at(from), relation, entity_ids.at(to));
        }

        statement_t unresolved(map, "SELECT value FROM json_each(?1,'$.unresolved');");
        sqlite3_bind_text(unresolved.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(unresolved.get()) == SQLITE_ROW)
        {
            const std::string text = column_text(unresolved.get(), 0);
            statement_t insert(map, "INSERT INTO unresolved(job_id,text) VALUES(?1,?2);");
            sqlite3_bind_int64(insert.get(), 1, job.id);
            sqlite3_bind_text(insert.get(), 2, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
            if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
        }
        mark_job(map, job.id, 3);
        exec(map, "COMMIT;");
    }
    catch (...)
    {
        try { exec(map, "ROLLBACK;"); } catch (...) {}
        throw;
    }
}

void set_running(sqlite3* map, std::int64_t job_id, std::string_view input_hash)
{
    statement_t update(map, "UPDATE lift_jobs SET status=1,attempts=attempts+1,input_hash=?2,error='',started_ms=unixepoch('subsec')*1000 WHERE id=?1;");
    sqlite3_bind_int64(update.get(), 1, job_id);
    sqlite3_bind_text(update.get(), 2, input_hash.data(), static_cast<int>(input_hash.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(update.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

std::vector<std::int64_t> pending_jobs(sqlite3* map)
{
    statement_t statement(map, "SELECT id FROM lift_jobs WHERE deterministic_kind='' AND status IN (0,4) AND attempts<3 ORDER BY id;");
    std::vector<std::int64_t> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) result.push_back(sqlite3_column_int64(statement.get(), 0));
    return result;
}

void run_deterministic_pass(sqlite3* source, sqlite3* map)
{
    statement_t jobs(map, "SELECT id FROM lift_jobs WHERE deterministic_kind<>'' AND status NOT IN (2,3) ORDER BY id;");
    std::vector<std::int64_t> ids;
    while (sqlite3_step(jobs.get()) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(jobs.get(), 0));
    for (const std::int64_t id : ids)
    {
        const job_t job = load_job(map, id);
        apply_deterministic(source, map, job);
    }
}

std::mutex console_mutex;

void print_progress(sqlite3* map)
{
    statement_t statement(map, R"SQL(
SELECT COUNT(*),
       SUM(CASE WHEN status IN (2,3) THEN 1 ELSE 0 END),
       SUM(CASE WHEN status=1 THEN 1 ELSE 0 END),
       SUM(CASE WHEN status=4 THEN 1 ELSE 0 END),
       SUM(source_count),
       SUM(CASE WHEN status IN (2,3) THEN source_count ELSE 0 END)
FROM lift_jobs;
)SQL");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return;
    const auto total_jobs = sqlite3_column_int64(statement.get(), 0);
    const auto done_jobs = sqlite3_column_int64(statement.get(), 1);
    const auto running = sqlite3_column_int64(statement.get(), 2);
    const auto failed = sqlite3_column_int64(statement.get(), 3);
    const auto total_sources = sqlite3_column_int64(statement.get(), 4);
    const auto done_sources = sqlite3_column_int64(statement.get(), 5);
    const double percent = total_sources == 0 ? 100.0 : (100.0 * static_cast<double>(done_sources) / static_cast<double>(total_sources));
    std::lock_guard lock(console_mutex);
    std::cout << "semantic " << done_jobs << '/' << total_jobs << " jobs | " << done_sources << '/' << total_sources
              << " sources | " << percent << "% | " << running << " running | " << failed << " failed\n";
}

void worker_loop(const std::filesystem::path& source_path, const std::filesystem::path& map_path, const std::vector<std::int64_t>& jobs, std::atomic<std::size_t>& cursor)
{
    auto source = database_t::open_readonly(source_path);
    auto map = database_t::open_map(map_path);
    exec(source.get(), "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY;");
    exec(map.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
    for (;;)
    {
        const std::size_t index = cursor.fetch_add(1U);
        if (index >= jobs.size()) break;
        const std::int64_t job_id = jobs[index];
        try
        {
            const job_t job = load_job(map.get(), job_id);
            const prompt_t prompt = build_prompt(source.get(), job);
            const std::string input_hash = sha256_hex(prompt.text);
            set_running(map.get(), job_id, input_hash);
            const std::string request = build_request(prompt);
            const http_result_t response = http_request(L"POST", L"/v1/chat/completions", request);
            if (response.status < 200U || response.status >= 300U)
                throw std::runtime_error("llama.cpp returned HTTP " + std::to_string(response.status));
            const std::string delta = extract_model_content(map.get(), response.body);
            apply_ai_delta(map.get(), job, prompt, delta);
        }
        catch (const std::exception& error)
        {
            try { mark_job(map.get(), job_id, 4, error.what()); } catch (...) {}
            std::lock_guard lock(console_mutex);
            std::cerr << "semantic job " << job_id << " failed: " << error.what() << '\n';
        }
        if ((index % 16U) == 0U) print_progress(map.get());
    }
}

void finalize_map(sqlite3* map)
{
    const std::int64_t remaining = scalar_i64(map, "SELECT COUNT(*) FROM lift_jobs WHERE status NOT IN (2,3);");
    statement_t update(map, "INSERT OR REPLACE INTO metadata(key,value) VALUES('complete',?1);");
    const char* value = remaining == 0 ? "1" : "0";
    sqlite3_bind_text(update.get(), 1, value, -1, SQLITE_STATIC);
    if (sqlite3_step(update.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        bool plan_only = false;
        std::filesystem::path source_path;
        for (int i = 1; i < argc; ++i)
        {
            if (std::wstring_view(argv[i]) == L"--plan-only") plan_only = true;
            else if (source_path.empty()) source_path = argv[i];
            else throw std::runtime_error("usage: nylon-semantic.exe [image.db] [--plan-only]");
        }
        if (source_path.empty()) source_path = default_database_path();
        std::error_code error;
        if (!std::filesystem::is_regular_file(source_path, error) || error)
            throw std::runtime_error("Nylon source database was not found");

        auto source = database_t::open_readonly(source_path);
        exec(source.get(), "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY;");
        const auto schema = scalar_text(source.get(), "SELECT value FROM metadata WHERE meta_key='schema_version';");
        if (!schema.has_value() || *schema != "2") throw std::runtime_error("Semantic lifter requires Nylon schema version 2");
        const std::string fingerprint = source_fingerprint(source.get(), source_path);
        const std::filesystem::path map_path = map_path_for(source_path);
        if (!map_compatible(map_path, fingerprint)) remove_map_files(map_path);
        auto map = database_t::open_map(map_path);
        initialize_map(map.get(), fingerprint, source_path);

        std::cout << "Nylon Semantic Lifter\n"
                  << "source: " << path_utf8(source_path) << '\n'
                  << "map:    " << path_utf8(map_path) << '\n'
                  << "model:  " << model_name << "\n";

        const auto plan = build_plan(source.get());
        persist_plan(map.get(), plan);
        const std::int64_t total_sources = scalar_i64(map.get(), "SELECT COALESCE(SUM(source_count),0) FROM lift_jobs;");
        std::cout << "plan:   " << plan.size() << " work units | " << total_sources << " raw sources\n";
        run_deterministic_pass(source.get(), map.get());
        print_progress(map.get());
        if (plan_only)
        {
            std::cout << "plan-only complete\n";
            return 0;
        }

        std::vector<std::int64_t> jobs = pending_jobs(map.get());
        if (!jobs.empty())
        {
            process_guard_t model_process;
            ensure_llama_server(model_process);
            std::cout << "local model ready on RX/Vulkan backend; processing " << jobs.size() << " AI work units\n";
            std::atomic<std::size_t> cursor{0};
            std::vector<std::thread> workers;
            workers.reserve(worker_count);
            for (unsigned i = 0; i < worker_count; ++i)
                workers.emplace_back(worker_loop, source_path, map_path, std::cref(jobs), std::ref(cursor));
            for (auto& worker : workers) worker.join();
        }

        finalize_map(map.get());
        print_progress(map.get());
        const std::int64_t failed = scalar_i64(map.get(), "SELECT COUNT(*) FROM lift_jobs WHERE status=4;");
        if (failed != 0)
        {
            std::cerr << "semantic map incomplete: " << failed << " work units failed after retry budget\n";
            return 2;
        }
        std::cout << "semantic map complete\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
