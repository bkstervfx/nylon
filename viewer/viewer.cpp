#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <shellapi.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr std::size_t max_request_bytes = 16U * 1024U;
constexpr int graph_node_limit = 500;

class socket_guard_t
{
public:
    socket_guard_t() noexcept = default;
    explicit socket_guard_t(SOCKET socket) noexcept : socket_(socket) {}
    ~socket_guard_t() { reset(); }
    socket_guard_t(const socket_guard_t&) = delete;
    socket_guard_t& operator=(const socket_guard_t&) = delete;
    socket_guard_t(socket_guard_t&& other) noexcept : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {}
    socket_guard_t& operator=(socket_guard_t&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        }
        return *this;
    }
    [[nodiscard]] SOCKET get() const noexcept { return socket_; }
    [[nodiscard]] SOCKET release() noexcept { return std::exchange(socket_, INVALID_SOCKET); }
    void reset(SOCKET socket = INVALID_SOCKET) noexcept
    {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = socket;
    }
private:
    SOCKET socket_ = INVALID_SOCKET;
};

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
    [[nodiscard]] sqlite3* get() const noexcept { return handle_; }
    void reset() noexcept
    {
        if (handle_ != nullptr) sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
    static std::optional<database_t> open_readonly(const std::filesystem::path& path, std::string& error)
    {
        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 0)
        {
            error = "Unable to encode database path";
            return std::nullopt;
        }
        std::string utf8(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, utf8.data(), required, nullptr, nullptr) <= 0)
        {
            error = "Unable to encode database path";
            return std::nullopt;
        }
        utf8.resize(static_cast<std::size_t>(required - 1));

        sqlite3* handle = nullptr;
        const int rc = sqlite3_open_v2(
            utf8.c_str(),
            &handle,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
            nullptr);
        if (rc != SQLITE_OK)
        {
            error = handle != nullptr ? sqlite3_errmsg(handle) : "sqlite3_open_v2 failed";
            if (handle != nullptr) sqlite3_close_v2(handle);
            return std::nullopt;
        }
        sqlite3_busy_timeout(handle, 500);
        char* pragma_error = nullptr;
        const int pragma_rc = sqlite3_exec(
            handle,
            "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY; PRAGMA foreign_keys=OFF;",
            nullptr,
            nullptr,
            &pragma_error);
        if (pragma_rc != SQLITE_OK)
        {
            error = pragma_error != nullptr ? pragma_error : "Unable to configure read-only SQLite connection";
            if (pragma_error != nullptr) sqlite3_free(pragma_error);
            sqlite3_close_v2(handle);
            return std::nullopt;
        }
        database_t result;
        result.handle_ = handle;
        return result;
    }
private:
    sqlite3* handle_ = nullptr;
};

std::string json_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 16U);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20U)
            {
                out += "\\u00";
                out.push_back(hex[(c >> 4U) & 0x0fU]);
                out.push_back(hex[c & 0x0fU]);
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string json_string(std::string_view value)
{
    return std::string("\"") + json_escape(value) + "\"";
}

std::string column_text(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite3_column_text(statement, column);
    if (text == nullptr) return {};
    const int length = sqlite3_column_bytes(statement, column);
    if (length <= 0) return {};
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
}

std::optional<std::string> scalar_text(sqlite3* database, std::string_view sql)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, std::string(sql).c_str(), -1, &statement, nullptr) != SQLITE_OK) return std::nullopt;
    const int rc = sqlite3_step(statement);
    std::optional<std::string> result;
    if (rc == SQLITE_ROW) result = column_text(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

std::int64_t scalar_int(sqlite3* database, std::string_view sql)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, std::string(sql).c_str(), -1, &statement, nullptr) != SQLITE_OK) return 0;
    const int rc = sqlite3_step(statement);
    const std::int64_t result = rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
    sqlite3_finalize(statement);
    return result;
}

std::string url_decode(std::string_view value)
{
    auto digit = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '+' )
        {
            out.push_back(' ');
        }
        else if (value[i] == '%' && i + 2U < value.size())
        {
            const int high = digit(value[i + 1U]);
            const int low = digit(value[i + 2U]);
            if (high >= 0 && low >= 0)
            {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2U;
            }
            else
            {
                out.push_back(value[i]);
            }
        }
        else
        {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(std::string_view query)
{
    std::map<std::string, std::string> result;
    std::size_t cursor = 0;
    while (cursor <= query.size())
    {
        const std::size_t amp = query.find('&', cursor);
        const std::string_view part = query.substr(cursor, amp == std::string_view::npos ? query.size() - cursor : amp - cursor);
        const std::size_t equal = part.find('=');
        if (equal == std::string_view::npos)
        {
            if (!part.empty()) result[url_decode(part)] = {};
        }
        else
        {
            result[url_decode(part.substr(0, equal))] = url_decode(part.substr(equal + 1U));
        }
        if (amp == std::string_view::npos) break;
        cursor = amp + 1U;
    }
    return result;
}

std::optional<std::int64_t> parse_i64(std::string_view value)
{
    if (value.empty()) return std::nullopt;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

std::string api_error(std::string_view message)
{
    return std::string("{\"error\":") + json_string(message) + "}";
}

struct graph_builder_t
{
    std::ostringstream nodes;
    std::ostringstream edges;
    int node_count = 0;
    int edge_count = 0;
    bool truncated = false;

    void node(std::string_view id, std::string_view kind, std::string_view label, std::string_view path = {})
    {
        if (node_count >= graph_node_limit)
        {
            truncated = true;
            return;
        }
        if (node_count++ != 0) nodes << ',';
        nodes << "{\"id\":" << json_string(id)
              << ",\"kind\":" << json_string(kind)
              << ",\"label\":" << json_string(label)
              << ",\"path\":" << json_string(path) << '}';
    }

    void edge(std::string_view source, std::string_view target, std::string_view kind)
    {
        if (node_count > graph_node_limit) return;
        if (edge_count++ != 0) edges << ',';
        edges << "{\"source\":" << json_string(source)
              << ",\"target\":" << json_string(target)
              << ",\"kind\":" << json_string(kind) << '}';
    }

    [[nodiscard]] std::string finish() const
    {
        std::ostringstream out;
        out << "{\"nodes\":[" << nodes.str() << "],\"edges\":[" << edges.str()
            << "],\"truncated\":" << (truncated ? "true" : "false") << '}';
        return out.str();
    }
};

std::string api_info(sqlite3* database, const std::filesystem::path& path)
{
    const std::string schema = scalar_text(database, "SELECT value FROM metadata WHERE meta_key='schema_version';").value_or("");
    const std::string image = scalar_text(database, "SELECT value FROM metadata WHERE meta_key='source_image_name';").value_or("");
    const std::string source = scalar_text(database, "SELECT value FROM metadata WHERE meta_key='source_kind';").value_or("");
    const std::string index = scalar_text(database, "SELECT value FROM metadata WHERE meta_key='source_image_index';").value_or("");
    const std::int64_t hives = scalar_int(database, "SELECT COUNT(*) FROM registry_hives;");
    const std::int64_t keys = scalar_int(database, "SELECT COUNT(*) FROM registry_keys;");
    const std::int64_t values = scalar_int(database, "SELECT COUNT(*) FROM registry_values;");

    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8_path;
    if (required > 0)
    {
        utf8_path.resize(static_cast<std::size_t>(required));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, utf8_path.data(), required, nullptr, nullptr) > 0)
            utf8_path.resize(static_cast<std::size_t>(required - 1));
        else
            utf8_path.clear();
    }

    std::ostringstream out;
    out << "{\"schema\":" << json_string(schema)
        << ",\"image\":" << json_string(image)
        << ",\"source\":" << json_string(source)
        << ",\"index\":" << json_string(index)
        << ",\"path\":" << json_string(utf8_path)
        << ",\"hives\":" << hives
        << ",\"keys\":" << keys
        << ",\"values\":" << values << '}';
    return out.str();
}

std::string api_hives(sqlite3* database)
{
    const char* sql = R"SQL(
WITH key_counts AS (
    SELECT hive_id, COUNT(*) AS count FROM registry_keys GROUP BY hive_id
), value_counts AS (
    SELECT k.hive_id, COUNT(*) AS count
    FROM registry_values v JOIN registry_keys k ON k.id=v.key_id
    GROUP BY k.hive_id
)
SELECT h.id,h.name,h.source_path,h.required,
       COALESCE(k.count,0),COALESCE(v.count,0)
FROM registry_hives h
LEFT JOIN key_counts k ON k.hive_id=h.id
LEFT JOIN value_counts v ON v.hive_id=h.id
ORDER BY h.id;
)SQL";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
    std::ostringstream out;
    out << "[";
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << sqlite3_column_int64(statement, 0)
            << ",\"name\":" << json_string(column_text(statement, 1))
            << ",\"sourcePath\":" << json_string(column_text(statement, 2))
            << ",\"required\":" << (sqlite3_column_int(statement, 3) != 0 ? "true" : "false")
            << ",\"keys\":" << sqlite3_column_int64(statement, 4)
            << ",\"values\":" << sqlite3_column_int64(statement, 5) << '}';
    }
    sqlite3_finalize(statement);
    out << ']';
    return out.str();
}

std::string api_overview(sqlite3* database)
{
    graph_builder_t graph;
    sqlite3_stmt* hives = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT id,name FROM registry_hives ORDER BY id;", -1, &hives, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    std::vector<std::int64_t> hive_ids;
    while (sqlite3_step(hives) == SQLITE_ROW)
    {
        const std::int64_t id = sqlite3_column_int64(hives, 0);
        hive_ids.push_back(id);
        graph.node("rh:" + std::to_string(id), "registry_hive", column_text(hives, 1));
    }
    sqlite3_finalize(hives);

    sqlite3_stmt* roots = nullptr;
    const char* sql = "SELECT id,hive_id,name,path FROM registry_keys WHERE parent_id IS NULL ORDER BY hive_id,id;";
    if (sqlite3_prepare_v2(database, sql, -1, &roots, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
    std::vector<std::int64_t> root_ids;
    while (sqlite3_step(roots) == SQLITE_ROW && graph.node_count < graph_node_limit)
    {
        const std::int64_t id = sqlite3_column_int64(roots, 0);
        const std::int64_t hive = sqlite3_column_int64(roots, 1);
        root_ids.push_back(id);
        const std::string key_id = "rk:" + std::to_string(id);
        graph.node(key_id, "registry_key", column_text(roots, 2), column_text(roots, 3));
        graph.edge("rh:" + std::to_string(hive), key_id, "contains");
    }
    sqlite3_finalize(roots);

    sqlite3_stmt* children = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT id,name,path FROM registry_keys WHERE parent_id=?1 ORDER BY name LIMIT 80;", -1, &children, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    for (const auto root : root_ids)
    {
        sqlite3_reset(children);
        sqlite3_clear_bindings(children);
        sqlite3_bind_int64(children, 1, root);
        while (sqlite3_step(children) == SQLITE_ROW && graph.node_count < graph_node_limit)
        {
            const std::int64_t id = sqlite3_column_int64(children, 0);
            const std::string child_id = "rk:" + std::to_string(id);
            graph.node(child_id, "registry_key", column_text(children, 1), column_text(children, 2));
            graph.edge("rk:" + std::to_string(root), child_id, "parent");
        }
    }
    sqlite3_finalize(children);
    return graph.finish();
}

bool add_key_node(sqlite3* database, graph_builder_t& graph, std::int64_t key_id)
{
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT id,name,path FROM registry_keys WHERE id=?1;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(statement, 1, key_id);
    const int rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW)
    {
        graph.node("rk:" + std::to_string(key_id), "registry_key", column_text(statement, 1), column_text(statement, 2));
        sqlite3_finalize(statement);
        return true;
    }
    sqlite3_finalize(statement);
    return false;
}

std::string api_neighborhood(sqlite3* database, std::string_view node_id, bool include_values)
{
    graph_builder_t graph;
    if (node_id.starts_with("rh:"))
    {
        const auto hive_id = parse_i64(node_id.substr(3));
        if (!hive_id.has_value()) return api_error("Invalid hive id");
        sqlite3_stmt* hive = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT name FROM registry_hives WHERE id=?1;", -1, &hive, nullptr) != SQLITE_OK)
            return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(hive, 1, *hive_id);
        if (sqlite3_step(hive) != SQLITE_ROW)
        {
            sqlite3_finalize(hive);
            return api_error("Hive not found");
        }
        graph.node(std::string(node_id), "registry_hive", column_text(hive, 0));
        sqlite3_finalize(hive);

        sqlite3_stmt* roots = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT id,name,path FROM registry_keys WHERE hive_id=?1 AND parent_id IS NULL ORDER BY id LIMIT 32;", -1, &roots, nullptr) != SQLITE_OK)
            return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(roots, 1, *hive_id);
        std::vector<std::int64_t> root_ids;
        while (sqlite3_step(roots) == SQLITE_ROW)
        {
            const std::int64_t id = sqlite3_column_int64(roots, 0);
            root_ids.push_back(id);
            const std::string key = "rk:" + std::to_string(id);
            graph.node(key, "registry_key", column_text(roots, 1), column_text(roots, 2));
            graph.edge(node_id, key, "contains");
        }
        sqlite3_finalize(roots);
        sqlite3_stmt* children = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT id,name,path FROM registry_keys WHERE parent_id=?1 ORDER BY name LIMIT 180;", -1, &children, nullptr) != SQLITE_OK)
            return api_error(sqlite3_errmsg(database));
        for (const auto root : root_ids)
        {
            sqlite3_reset(children);
            sqlite3_clear_bindings(children);
            sqlite3_bind_int64(children, 1, root);
            while (sqlite3_step(children) == SQLITE_ROW && graph.node_count < graph_node_limit)
            {
                const std::int64_t id = sqlite3_column_int64(children, 0);
                const std::string child = "rk:" + std::to_string(id);
                graph.node(child, "registry_key", column_text(children, 1), column_text(children, 2));
                graph.edge("rk:" + std::to_string(root), child, "parent");
            }
        }
        sqlite3_finalize(children);
        return graph.finish();
    }

    if (node_id.starts_with("rv:"))
    {
        const auto value_id = parse_i64(node_id.substr(3));
        if (!value_id.has_value()) return api_error("Invalid value id");
        sqlite3_stmt* value = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT key_id,CASE WHEN name='' THEN '(Default)' ELSE name END FROM registry_values WHERE id=?1;", -1, &value, nullptr) != SQLITE_OK)
            return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(value, 1, *value_id);
        if (sqlite3_step(value) != SQLITE_ROW)
        {
            sqlite3_finalize(value);
            return api_error("Value not found");
        }
        const std::int64_t key_id = sqlite3_column_int64(value, 0);
        const std::string label = column_text(value, 1);
        sqlite3_finalize(value);
        if (!add_key_node(database, graph, key_id)) return api_error("Owning key not found");
        graph.node(std::string(node_id), "registry_value", label);
        graph.edge("rk:" + std::to_string(key_id), node_id, "owns");
        node_id = std::string_view{};
        sqlite3_stmt* key_info = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT parent_id FROM registry_keys WHERE id=?1;", -1, &key_info, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(key_info, 1, key_id);
            if (sqlite3_step(key_info) == SQLITE_ROW && sqlite3_column_type(key_info, 0) != SQLITE_NULL)
            {
                const std::int64_t parent = sqlite3_column_int64(key_info, 0);
                if (add_key_node(database, graph, parent)) graph.edge("rk:" + std::to_string(parent), "rk:" + std::to_string(key_id), "parent");
            }
            sqlite3_finalize(key_info);
        }
        return graph.finish();
    }

    if (!node_id.starts_with("rk:")) return api_error("Unsupported node id");
    const auto key_id = parse_i64(node_id.substr(3));
    if (!key_id.has_value()) return api_error("Invalid key id");
    if (!add_key_node(database, graph, *key_id)) return api_error("Key not found");

    sqlite3_stmt* key_info = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT hive_id,parent_id FROM registry_keys WHERE id=?1;", -1, &key_info, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    sqlite3_bind_int64(key_info, 1, *key_id);
    std::int64_t hive_id = 0;
    std::optional<std::int64_t> parent;
    if (sqlite3_step(key_info) == SQLITE_ROW)
    {
        hive_id = sqlite3_column_int64(key_info, 0);
        if (sqlite3_column_type(key_info, 1) != SQLITE_NULL) parent = sqlite3_column_int64(key_info, 1);
    }
    sqlite3_finalize(key_info);

    if (parent.has_value() && add_key_node(database, graph, *parent))
        graph.edge("rk:" + std::to_string(*parent), "rk:" + std::to_string(*key_id), "parent");
    else
    {
        sqlite3_stmt* hive = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT name FROM registry_hives WHERE id=?1;", -1, &hive, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(hive, 1, hive_id);
            if (sqlite3_step(hive) == SQLITE_ROW)
            {
                const std::string hive_node = "rh:" + std::to_string(hive_id);
                graph.node(hive_node, "registry_hive", column_text(hive, 0));
                graph.edge(hive_node, "rk:" + std::to_string(*key_id), "contains");
            }
            sqlite3_finalize(hive);
        }
    }

    sqlite3_stmt* children = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT id,name,path FROM registry_keys WHERE parent_id=?1 ORDER BY name LIMIT 260;", -1, &children, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    sqlite3_bind_int64(children, 1, *key_id);
    while (sqlite3_step(children) == SQLITE_ROW && graph.node_count < graph_node_limit)
    {
        const std::int64_t id = sqlite3_column_int64(children, 0);
        const std::string child = "rk:" + std::to_string(id);
        graph.node(child, "registry_key", column_text(children, 1), column_text(children, 2));
        graph.edge("rk:" + std::to_string(*key_id), child, "parent");
    }
    if (sqlite3_step(children) == SQLITE_ROW) graph.truncated = true;
    sqlite3_finalize(children);

    if (include_values && graph.node_count < graph_node_limit)
    {
        sqlite3_stmt* values = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT id,CASE WHEN name='' THEN '(Default)' ELSE name END FROM registry_values WHERE key_id=?1 ORDER BY name LIMIT 220;", -1, &values, nullptr) != SQLITE_OK)
            return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(values, 1, *key_id);
        while (sqlite3_step(values) == SQLITE_ROW && graph.node_count < graph_node_limit)
        {
            const std::int64_t id = sqlite3_column_int64(values, 0);
            const std::string value = "rv:" + std::to_string(id);
            graph.node(value, "registry_value", column_text(values, 1));
            graph.edge("rk:" + std::to_string(*key_id), value, "owns");
        }
        if (sqlite3_step(values) == SQLITE_ROW) graph.truncated = true;
        sqlite3_finalize(values);
    }
    return graph.finish();
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

std::string api_node(sqlite3* database, std::string_view node_id)
{
    if (node_id.starts_with("rh:"))
    {
        const auto id = parse_i64(node_id.substr(3));
        if (!id.has_value()) return api_error("Invalid hive id");
        sqlite3_stmt* statement = nullptr;
        const char* sql = R"SQL(
SELECT h.name,h.source_path,h.required,
       (SELECT COUNT(*) FROM registry_keys k WHERE k.hive_id=h.id),
       (SELECT COUNT(*) FROM registry_values v JOIN registry_keys k ON k.id=v.key_id WHERE k.hive_id=h.id)
FROM registry_hives h WHERE h.id=?1;
)SQL";
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(statement, 1, *id);
        if (sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            return api_error("Hive not found");
        }
        std::ostringstream out;
        out << "{\"id\":" << json_string(node_id) << ",\"kind\":\"Registry hive\",\"name\":" << json_string(column_text(statement, 0))
            << ",\"path\":" << json_string(column_text(statement, 1))
            << ",\"required\":" << (sqlite3_column_int(statement, 2) != 0 ? "true" : "false")
            << ",\"children\":" << sqlite3_column_int64(statement, 3)
            << ",\"values\":" << sqlite3_column_int64(statement, 4) << '}';
        sqlite3_finalize(statement);
        return out.str();
    }
    if (node_id.starts_with("rk:"))
    {
        const auto id = parse_i64(node_id.substr(3));
        if (!id.has_value()) return api_error("Invalid key id");
        sqlite3_stmt* statement = nullptr;
        const char* sql = R"SQL(
SELECT k.name,k.path,k.cell_offset,h.name,
       (SELECT COUNT(*) FROM registry_keys c WHERE c.parent_id=k.id),
       (SELECT COUNT(*) FROM registry_values v WHERE v.key_id=k.id)
FROM registry_keys k JOIN registry_hives h ON h.id=k.hive_id WHERE k.id=?1;
)SQL";
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(statement, 1, *id);
        if (sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            return api_error("Key not found");
        }
        std::ostringstream cell;
        cell << "0x" << std::hex << std::uppercase << sqlite3_column_int64(statement, 2);
        std::ostringstream out;
        out << "{\"id\":" << json_string(node_id) << ",\"kind\":\"Registry key\",\"name\":" << json_string(column_text(statement, 0))
            << ",\"path\":" << json_string(column_text(statement, 1))
            << ",\"cell\":" << json_string(cell.str())
            << ",\"hive\":" << json_string(column_text(statement, 3))
            << ",\"children\":" << sqlite3_column_int64(statement, 4)
            << ",\"values\":" << sqlite3_column_int64(statement, 5) << '}';
        sqlite3_finalize(statement);
        return out.str();
    }
    if (node_id.starts_with("rv:"))
    {
        const auto id = parse_i64(node_id.substr(3));
        if (!id.has_value()) return api_error("Invalid value id");
        sqlite3_stmt* statement = nullptr;
        const char* sql = R"SQL(
SELECT CASE WHEN v.name='' THEN '(Default)' ELSE v.name END,
       k.path || '::' || v.name,
       v.value_type,
       length(v.data),
       substr(COALESCE(v.text_value,''),1,65536),
       hex(substr(v.data,1,256)),
       k.id
FROM registry_values v JOIN registry_keys k ON k.id=v.key_id WHERE v.id=?1;
)SQL";
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
        sqlite3_bind_int64(statement, 1, *id);
        if (sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            return api_error("Value not found");
        }
        const std::int64_t type = sqlite3_column_int64(statement, 2);
        std::ostringstream out;
        out << "{\"id\":" << json_string(node_id) << ",\"kind\":\"Registry value\",\"name\":" << json_string(column_text(statement, 0))
            << ",\"path\":" << json_string(column_text(statement, 1))
            << ",\"type\":" << type
            << ",\"typeName\":" << json_string(registry_type_name(type))
            << ",\"bytes\":" << sqlite3_column_int64(statement, 3)
            << ",\"text\":" << json_string(column_text(statement, 4))
            << ",\"hex\":" << json_string(column_text(statement, 5))
            << ",\"keyId\":" << sqlite3_column_int64(statement, 6) << '}';
        sqlite3_finalize(statement);
        return out.str();
    }
    return api_error("Unsupported node id");
}

std::string api_search(sqlite3* database, std::string_view query)
{
    if (query.size() < 2U) return "[]";
    std::string pattern = "%";
    pattern.append(query);
    pattern.push_back('%');
    const char* sql = R"SQL(
SELECT kind,id,name,path FROM (
    SELECT 'registry_key' AS kind,id,name,path,0 AS rank
    FROM registry_keys
    WHERE name LIKE ?1 ESCAPE '\\' OR path LIKE ?1 ESCAPE '\\'
    LIMIT 35
)
UNION ALL
SELECT kind,id,name,path FROM (
    SELECT 'registry_value' AS kind,v.id,
           CASE WHEN v.name='' THEN '(Default)' ELSE v.name END AS name,
           k.path || '::' || v.name AS path,1 AS rank
    FROM registry_values v JOIN registry_keys k ON k.id=v.key_id
    WHERE v.name LIKE ?1 ESCAPE '\\'
    LIMIT 35
)
LIMIT 60;
)SQL";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
    sqlite3_bind_text(statement, 1, pattern.c_str(), static_cast<int>(pattern.size()), SQLITE_TRANSIENT);
    std::ostringstream out;
    out << '[';
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        const std::string kind = column_text(statement, 0);
        const std::string prefix = kind == "registry_key" ? "rk:" : "rv:";
        out << "{\"id\":" << json_string(prefix + std::to_string(sqlite3_column_int64(statement, 1)))
            << ",\"kind\":" << json_string(kind)
            << ",\"name\":" << json_string(column_text(statement, 2))
            << ",\"path\":" << json_string(column_text(statement, 3)) << '}';
    }
    sqlite3_finalize(statement);
    out << ']';
    return out.str();
}

const char* viewer_html = R"NYLON(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>Nylon Viewer</title>
<style>
:root{--bg:#0b0d10;--panel:#0f1216;--raised:#15191f;--hover:#1b2027;--border:#242b34;--border2:#313a46;--text:#f2f5f7;--muted:#9aa6b2;--dim:#697683;--accent:#8da7ff;--hive:#e0b56d;--key:#86a9ff;--value:#70d5b1;--danger:#ee7d7d}*{box-sizing:border-box}html,body{height:100%;margin:0;background:var(--bg);color:var(--text);font:13px/1.45 "Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;overflow:hidden}button,input{font:inherit;color:inherit}.app{height:100%;display:grid;grid-template-rows:48px 1fr 28px}.header{display:flex;align-items:center;border-bottom:1px solid var(--border);background:var(--panel);padding:0 14px;gap:16px}.brand{font-weight:700;letter-spacing:.12em;font-size:12px}.brand span{font-weight:400;color:var(--dim);letter-spacing:0;margin-left:8px}.db-title{min-width:0;flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:var(--muted)}.db-title strong{color:var(--text);font-weight:600;margin-right:8px}.layout{min-height:0;display:grid;grid-template-columns:276px 1fr 360px}.sidebar,.inspector{background:var(--panel);min-height:0;overflow:auto}.sidebar{border-right:1px solid var(--border)}.inspector{border-left:1px solid var(--border)}.section{padding:14px;border-bottom:1px solid var(--border)}.section-title{font-size:11px;text-transform:uppercase;letter-spacing:.09em;color:var(--dim);margin-bottom:10px}.search{position:relative}.search input{width:100%;height:36px;border:1px solid var(--border2);background:var(--bg);border-radius:6px;padding:0 34px 0 11px;outline:none}.search input:focus{border-color:var(--accent);box-shadow:0 0 0 2px #8da7ff22}.search .hint{position:absolute;right:10px;top:9px;color:var(--dim);font-size:11px}.results{margin-top:8px;max-height:330px;overflow:auto}.result,.hive-row{padding:8px 9px;border-radius:5px;cursor:pointer}.result:hover,.hive-row:hover{background:var(--hover)}.result-name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.result-path{font-size:11px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}.kind{display:inline-block;font-size:9px;letter-spacing:.08em;color:var(--dim);margin-right:6px}.hive-row{display:flex;align-items:center;justify-content:space-between;gap:8px}.hive-name{font-weight:550}.count{font-variant-numeric:tabular-nums;color:var(--dim);font-size:11px}.canvas-wrap{position:relative;min-width:0;min-height:0;background:var(--bg);overflow:hidden}.canvas-wrap canvas{position:absolute;inset:0;width:100%;height:100%;touch-action:none}.toolbar{position:absolute;left:12px;top:12px;display:flex;gap:4px;padding:4px;background:#0f1216e8;border:1px solid var(--border);border-radius:7px;backdrop-filter:blur(8px)}button{height:30px;border:1px solid transparent;background:transparent;border-radius:5px;padding:0 9px;cursor:pointer}button:hover{background:var(--hover)}button.active{background:#8da7ff1d;border-color:#8da7ff38;color:#cbd7ff}.notice{position:absolute;left:50%;bottom:18px;transform:translateX(-50%);padding:8px 12px;background:#15191ff2;border:1px solid var(--border2);border-radius:6px;color:var(--muted);display:none}.notice.show{display:block}.inspector-inner{padding:16px}.inspector h2{font-size:16px;margin:0 0 4px;font-weight:600}.subtle{color:var(--dim);font-size:12px}.path{margin:12px 0;padding:9px 10px;background:var(--bg);border:1px solid var(--border);border-radius:6px;color:var(--muted);font:11px/1.5 "Cascadia Mono",Consolas,monospace;overflow-wrap:anywhere}.meta{display:grid;grid-template-columns:112px minmax(0,1fr);gap:7px 10px;margin-top:14px}.meta .k{color:var(--dim)}.meta .v{min-width:0;overflow-wrap:anywhere}.value-text{white-space:pre-wrap;overflow-wrap:anywhere;max-height:260px;overflow:auto;padding:10px;background:var(--bg);border:1px solid var(--border);border-radius:6px;font:11px/1.5 "Cascadia Mono",Consolas,monospace;margin-top:12px}.hex{word-spacing:4px;color:#b8c1cc}.actions{display:flex;gap:6px;margin-top:14px;flex-wrap:wrap}.actions button{background:var(--raised);border-color:var(--border)}.status{height:28px;border-top:1px solid var(--border);background:var(--panel);display:flex;align-items:center;justify-content:space-between;padding:0 12px;color:var(--dim);font-size:11px;font-variant-numeric:tabular-nums}.empty{height:100%;display:grid;place-items:center;color:var(--dim);text-align:center}.empty strong{display:block;color:var(--text);font-size:18px;margin-bottom:6px}.dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:7px}.legend{display:flex;gap:14px;color:var(--dim);font-size:11px;align-items:center}.legend .dot{width:7px;height:7px}.error{color:var(--danger)}@media(max-width:1150px){.layout{grid-template-columns:240px 1fr 310px}}@media(max-width:950px){.layout{grid-template-columns:220px 1fr}.inspector{display:none}}
</style>
</head>
<body>
<div class="app">
<header class="header"><div class="brand">NYLON <span>Viewer</span></div><div class="db-title" id="dbTitle">Opening audit database…</div><button id="overviewBtn">Overview</button></header>
<div class="layout">
<aside class="sidebar">
<div class="section"><div class="search"><input id="search" autocomplete="off" spellcheck="false" placeholder="Search registry…"><span class="hint">/</span></div><div class="results" id="results"></div></div>
<div class="section"><div class="section-title">Registry hives</div><div id="hives"></div></div>
</aside>
<main class="canvas-wrap" id="canvasWrap"><canvas id="graph"></canvas><div class="toolbar"><button id="fitBtn" title="Fit graph">Fit</button><button id="physicsBtn" class="active" title="Toggle graph physics">Physics</button><button id="valuesBtn" class="active" title="Show registry values">Values</button><button id="backBtn" title="Previous focus">Back</button></div><div class="notice" id="notice"></div></main>
<aside class="inspector"><div class="inspector-inner" id="inspector"><div class="empty"><div><strong>Nylon Viewer</strong>Select a node to inspect it.</div></div></div></aside>
</div>
<footer class="status"><div id="statusLeft">Loading…</div><div class="legend"><span><i class="dot" style="background:var(--hive)"></i>Hive</span><span><i class="dot" style="background:var(--key)"></i>Key</span><span><i class="dot" style="background:var(--value)"></i>Value</span></div><div id="statusRight">Local · read-only</div></footer>
</div>
<script>
'use strict';
const $=s=>document.querySelector(s);const canvas=$('#graph'),wrap=$('#canvasWrap'),ctx=canvas.getContext('2d');let DPR=Math.max(1,Math.min(2,devicePixelRatio||1));
let graph={nodes:[],edges:[],map:new Map(),adj:new Map()},selected=null,hovered=null,drag=null,pan=null,physics=true,showValues=true,history=[],frame=0;
let cam={x:0,y:0,z:1};
const colors={registry_hive:'#e0b56d',registry_key:'#86a9ff',registry_value:'#70d5b1'};
const fmt=n=>new Intl.NumberFormat().format(n||0);
async function api(path){const r=await fetch(path,{cache:'no-store'});const j=await r.json();if(!r.ok||j.error)throw new Error(j.error||('HTTP '+r.status));return j}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function resize(){const r=wrap.getBoundingClientRect();DPR=Math.max(1,Math.min(2,devicePixelRatio||1));canvas.width=Math.max(1,Math.floor(r.width*DPR));canvas.height=Math.max(1,Math.floor(r.height*DPR));canvas.style.width=r.width+'px';canvas.style.height=r.height+'px'}new ResizeObserver(resize).observe(wrap);resize();
function setGraph(payload,focusId=null,pushHistory=true){if(pushHistory&&selected)history.push(selected);const old=graph.map;graph={nodes:[],edges:payload.edges||[],map:new Map(),adj:new Map()};const cx=wrap.clientWidth/2,cy=wrap.clientHeight/2;for(let i=0;i<(payload.nodes||[]).length;i++){const p=payload.nodes[i],prev=old.get(p.id);const a=i*2.3999632297,r=28*Math.sqrt(i+1);const n={...p,x:prev?.x??cx+Math.cos(a)*r,y:prev?.y??cy+Math.sin(a)*r,vx:0,vy:0,pinned:prev?.pinned||false};graph.nodes.push(n);graph.map.set(n.id,n);graph.adj.set(n.id,new Set())}for(const e of graph.edges){graph.adj.get(e.source)?.add(e.target);graph.adj.get(e.target)?.add(e.source)}selected=focusId&&graph.map.has(focusId)?focusId:null;hovered=null;if(payload.truncated)notice('View capped at 500 nodes. Focus a branch to go deeper.');fit();wake();updateStatus()}
function notice(text){const n=$('#notice');n.textContent=text;n.classList.add('show');clearTimeout(notice.t);notice.t=setTimeout(()=>n.classList.remove('show'),3500)}
function wake(){frame=0}
function simulate(){if(!physics||!graph.nodes.length)return;frame++;const nodes=graph.nodes,N=nodes.length;for(const n of nodes){if(n.pinned||drag===n.id)continue;n.vx+=(wrap.clientWidth/2-n.x)*0.00035;n.vy+=(wrap.clientHeight/2-n.y)*0.00035}for(const e of graph.edges){const a=graph.map.get(e.source),b=graph.map.get(e.target);if(!a||!b)continue;let dx=b.x-a.x,dy=b.y-a.y,d=Math.hypot(dx,dy)||1;const rest=e.kind==='owns'?72:105;const f=(d-rest)*0.0024,fx=dx/d*f,fy=dy/d*f;if(!a.pinned&&drag!==a.id){a.vx+=fx;a.vy+=fy}if(!b.pinned&&drag!==b.id){b.vx-=fx;b.vy-=fy}}const samples=Math.min(12,Math.max(5,N-1));for(let i=0;i<N;i++){const a=nodes[i];if(a.pinned||drag===a.id)continue;for(let s=1;s<=samples;s++){const j=(i+s*53+frame*7)%N;if(j===i)continue;const b=nodes[j];let dx=a.x-b.x,dy=a.y-b.y,d2=dx*dx+dy*dy+80;if(d2>40000)continue;const f=38/d2;a.vx+=dx*f;a.vy+=dy*f}}let energy=0;for(const n of nodes){if(n.pinned||drag===n.id){n.vx=n.vy=0;continue}n.vx*=0.88;n.vy*=0.88;n.x+=n.vx;n.y+=n.vy;energy+=Math.abs(n.vx)+Math.abs(n.vy)}if(frame>500&&energy/N<0.015)frame=10000}
function worldToScreen(n){return{x:(n.x-cam.x)*cam.z,y:(n.y-cam.y)*cam.z}}
function screenToWorld(x,y){return{x:x/cam.z+cam.x,y:y/cam.z+cam.y}}
function nodeRadius(n){return n.kind==='registry_hive'?8:n.kind==='registry_key'?5.4:3.8}
function render(){if(frame<10000)simulate();ctx.setTransform(DPR,0,0,DPR,0,0);ctx.clearRect(0,0,wrap.clientWidth,wrap.clientHeight);const focus=selected?graph.adj.get(selected):null;for(const e of graph.edges){const a=graph.map.get(e.source),b=graph.map.get(e.target);if(!a||!b)continue;const A=worldToScreen(a),B=worldToScreen(b);const hot=selected&&(e.source===selected||e.target===selected);ctx.strokeStyle=hot?'rgba(141,167,255,.65)':'rgba(91,105,122,.22)';ctx.lineWidth=hot?1.35:.75;ctx.beginPath();ctx.moveTo(A.x,A.y);ctx.lineTo(B.x,B.y);ctx.stroke()}for(const n of graph.nodes){const p=worldToScreen(n),r=Math.max(2,nodeRadius(n)*Math.sqrt(cam.z));const related=!selected||n.id===selected||focus?.has(n.id);ctx.globalAlpha=related?1:.18;ctx.fillStyle=colors[n.kind]||'#b69af3';ctx.beginPath();ctx.arc(p.x,p.y,r,0,Math.PI*2);ctx.fill();if(n.id===selected){ctx.strokeStyle='#f2f5f7';ctx.lineWidth=2;ctx.beginPath();ctx.arc(p.x,p.y,r+4,0,Math.PI*2);ctx.stroke()}else if(n.id===hovered){ctx.strokeStyle='#c8d4e2';ctx.lineWidth=1.2;ctx.beginPath();ctx.arc(p.x,p.y,r+3,0,Math.PI*2);ctx.stroke()}if(n.id===selected||n.id===hovered||n.kind==='registry_hive'||(cam.z>1.55&&related)){ctx.globalAlpha=related?1:.28;ctx.fillStyle='#e8edf2';ctx.font='11px Segoe UI';ctx.textBaseline='middle';ctx.fillText(n.label.slice(0,52),p.x+r+6,p.y)}}ctx.globalAlpha=1;requestAnimationFrame(render)}requestAnimationFrame(render);
function fit(){if(!graph.nodes.length)return;let minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity;for(const n of graph.nodes){minX=Math.min(minX,n.x);minY=Math.min(minY,n.y);maxX=Math.max(maxX,n.x);maxY=Math.max(maxY,n.y)}const w=Math.max(120,maxX-minX),h=Math.max(120,maxY-minY),pad=55;cam.z=Math.max(.15,Math.min(2.2,Math.min((wrap.clientWidth-pad*2)/w,(wrap.clientHeight-pad*2)/h)));cam.x=(minX+maxX)/2-wrap.clientWidth/(2*cam.z);cam.y=(minY+maxY)/2-wrap.clientHeight/(2*cam.z)}
function hit(x,y){let best=null,dist=Infinity;for(const n of graph.nodes){const p=worldToScreen(n),d=Math.hypot(x-p.x,y-p.y),r=Math.max(8,nodeRadius(n)*Math.sqrt(cam.z)+5);if(d<r&&d<dist){best=n;dist=d}}return best}
canvas.addEventListener('pointerdown',e=>{canvas.setPointerCapture(e.pointerId);const n=hit(e.offsetX,e.offsetY);if(n){drag=n.id;selected=n.id;n.pinned=true;inspect(n.id);wake()}else pan={x:e.clientX,y:e.clientY,cx:cam.x,cy:cam.y}});canvas.addEventListener('pointermove',e=>{if(drag){const n=graph.map.get(drag),p=screenToWorld(e.offsetX,e.offsetY);if(n){n.x=p.x;n.y=p.y;n.vx=n.vy=0;wake()}}else if(pan){cam.x=pan.cx-(e.clientX-pan.x)/cam.z;cam.y=pan.cy-(e.clientY-pan.y)/cam.z}else{const n=hit(e.offsetX,e.offsetY);hovered=n?.id||null;canvas.style.cursor=n?'pointer':'default'}});canvas.addEventListener('pointerup',()=>{if(drag){const n=graph.map.get(drag);if(n)n.pinned=false;drag=null;wake()}pan=null});canvas.addEventListener('wheel',e=>{e.preventDefault();const before=screenToWorld(e.offsetX,e.offsetY),factor=e.deltaY<0?1.12:.89;cam.z=Math.max(.12,Math.min(4,cam.z*factor));const after=screenToWorld(e.offsetX,e.offsetY);cam.x+=before.x-after.x;cam.y+=before.y-after.y},{passive:false});canvas.addEventListener('dblclick',e=>{const n=hit(e.offsetX,e.offsetY);if(n)loadNeighborhood(n.id,true)});
async function inspect(id){try{const d=await api('/api/node?id='+encodeURIComponent(id));const rows=[];const add=(k,v)=>{if(v!==undefined&&v!==null&&v!=='')rows.push(`<div class="k">${esc(k)}</div><div class="v">${esc(v)}</div>`)};add('Hive',d.hive);add('Children',d.children!==undefined?fmt(d.children):null);add('Values',d.values!==undefined?fmt(d.values):null);add('Cell',d.cell);add('Type',d.typeName?`${d.typeName} · ${d.type}`:null);add('Size',d.bytes!==undefined?fmt(d.bytes)+' bytes':null);add('Required',d.required!==undefined?(d.required?'Yes':'No'):null);let extra='';if(d.text)extra+=`<div class="section-title" style="margin-top:16px">Decoded text</div><div class="value-text">${esc(d.text)}</div>`;if(d.hex)extra+=`<div class="section-title" style="margin-top:16px">Raw preview</div><div class="value-text hex">${esc((d.hex.match(/../g)||[]).join(' '))}</div>`;$('#inspector').innerHTML=`<h2>${esc(d.name||d.id)}</h2><div class="subtle">${esc(d.kind)}</div>${d.path?`<div class="path">${esc(d.path)}</div>`:''}<div class="meta">${rows.join('')}</div>${extra}<div class="actions"><button id="focusAction">Focus graph</button>${d.path?'<button id="copyAction">Copy path</button>':''}</div>`;$('#focusAction')?.addEventListener('click',()=>loadNeighborhood(id,true));$('#copyAction')?.addEventListener('click',async()=>{await navigator.clipboard.writeText(d.path);notice('Copied path')})}catch(e){$('#inspector').innerHTML=`<div class="error">${esc(e.message)}</div>`}}
async function loadNeighborhood(id,push=true){try{const p=await api('/api/neighborhood?id='+encodeURIComponent(id)+'&values='+(showValues?'1':'0'));setGraph(p,id,push);inspect(id)}catch(e){notice(e.message)}}
async function overview(){try{const p=await api('/api/overview');setGraph(p,null,false);$('#inspector').innerHTML='<div class="empty"><div><strong>Database overview</strong>Select a hive or registry node.</div></div>'}catch(e){notice(e.message)}}
function updateStatus(){const edges=graph.edges.length;$('#statusRight').textContent=`${fmt(graph.nodes.length)} nodes · ${fmt(edges)} edges · read-only`}
$('#fitBtn').onclick=fit;$('#overviewBtn').onclick=overview;$('#physicsBtn').onclick=()=>{physics=!physics;$('#physicsBtn').classList.toggle('active',physics);wake()};$('#valuesBtn').onclick=()=>{showValues=!showValues;$('#valuesBtn').classList.toggle('active',showValues);if(selected)loadNeighborhood(selected,false)};$('#backBtn').onclick=()=>{const id=history.pop();if(id)loadNeighborhood(id,false)};
window.addEventListener('keydown',e=>{if(e.key==='/'&&document.activeElement!==$('#search')){e.preventDefault();$('#search').focus()}if(e.key==='Escape'){$('#search').blur();$('#results').innerHTML=''}if((e.key==='f'||e.key==='F')&&selected){const n=graph.map.get(selected);if(n){cam.x=n.x-wrap.clientWidth/(2*cam.z);cam.y=n.y-wrap.clientHeight/(2*cam.z)}}});
let searchTimer=0,searchController=null;$('#search').addEventListener('input',()=>{clearTimeout(searchTimer);const q=$('#search').value.trim();if(q.length<2){$('#results').innerHTML='';return}searchTimer=setTimeout(async()=>{try{searchController?.abort();searchController=new AbortController();const r=await fetch('/api/search?q='+encodeURIComponent(q),{cache:'no-store',signal:searchController.signal});const list=await r.json();$('#results').innerHTML=list.map(x=>`<div class="result" data-id="${esc(x.id)}"><div class="result-name"><span class="kind">${x.kind==='registry_key'?'KEY':'VALUE'}</span>${esc(x.name)}</div><div class="result-path">${esc(x.path)}</div></div>`).join('')||'<div class="subtle" style="padding:8px">No results</div>';document.querySelectorAll('.result').forEach(el=>el.onclick=()=>loadNeighborhood(el.dataset.id,true))}catch(e){if(e.name!=='AbortError')notice(e.message)}},260)});
async function init(){try{const info=await api('/api/info');if(info.schema!=='2')throw new Error('Unsupported Nylon schema '+info.schema+' (viewer expects schema 2)');$('#dbTitle').innerHTML=`<strong>${esc(info.image||'Nylon audit')}</strong>${esc(info.path)}`;$('#statusLeft').textContent=`${fmt(info.keys)} keys · ${fmt(info.values)} values · ${fmt(info.hives)} hives`;const h=await api('/api/hives');$('#hives').innerHTML=h.map(x=>`<div class="hive-row" data-id="rh:${x.id}"><div><div class="hive-name"><i class="dot" style="background:var(--hive)"></i>${esc(x.name)}</div><div class="count">${fmt(x.keys)} keys · ${fmt(x.values)} values</div></div></div>`).join('');document.querySelectorAll('.hive-row').forEach(el=>el.onclick=()=>loadNeighborhood(el.dataset.id,true));await overview()}catch(e){$('#dbTitle').innerHTML='<span class="error">Unable to open database</span>';$('#inspector').innerHTML=`<div class="error">${esc(e.message)}</div>`;$('#statusLeft').textContent='Viewer error'}}init();
</script>
</body>
</html>)NYLON";

struct response_t
{
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

response_t route(sqlite3* database, const std::filesystem::path& database_path, std::string_view target)
{
    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    const auto query = question == std::string_view::npos ? std::map<std::string, std::string>{} : parse_query(target.substr(question + 1U));
    if (path == "/" || path == "/index.html")
    {
        return response_t{ 200, "text/html; charset=utf-8", viewer_html };
    }
    if (path == "/favicon.ico") return response_t{ 204, "image/x-icon", {} };
    if (path == "/api/info") return response_t{ 200, "application/json; charset=utf-8", api_info(database, database_path) };
    if (path == "/api/hives") return response_t{ 200, "application/json; charset=utf-8", api_hives(database) };
    if (path == "/api/overview") return response_t{ 200, "application/json; charset=utf-8", api_overview(database) };
    if (path == "/api/neighborhood")
    {
        const auto found = query.find("id");
        if (found == query.end()) return response_t{ 400, "application/json; charset=utf-8", api_error("Missing node id") };
        const bool values = query.find("values") == query.end() || query.at("values") != "0";
        return response_t{ 200, "application/json; charset=utf-8", api_neighborhood(database, found->second, values) };
    }
    if (path == "/api/node")
    {
        const auto found = query.find("id");
        if (found == query.end()) return response_t{ 400, "application/json; charset=utf-8", api_error("Missing node id") };
        return response_t{ 200, "application/json; charset=utf-8", api_node(database, found->second) };
    }
    if (path == "/api/search")
    {
        const auto found = query.find("q");
        return response_t{ 200, "application/json; charset=utf-8", found == query.end() ? "[]" : api_search(database, found->second) };
    }
    return response_t{ 404, "application/json; charset=utf-8", api_error("Not found") };
}

const char* status_text(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    default: return "Error";
    }
}

bool send_all(SOCKET socket, std::string_view data)
{
    std::size_t offset = 0;
    while (offset < data.size())
    {
        const std::size_t remaining = data.size() - offset;
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int written = send(socket, data.data() + offset, chunk, 0);
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

void serve_client(SOCKET client, sqlite3* database, const std::filesystem::path& database_path)
{
    socket_guard_t guard(client);
    std::string request;
    request.reserve(4096U);
    std::array<char, 4096> buffer{};
    while (request.size() < max_request_bytes && request.find("\r\n\r\n") == std::string::npos)
    {
        const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) return;
        request.append(buffer.data(), static_cast<std::size_t>(received));
    }
    const std::size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos) return;
    const std::string_view first_line(request.data(), line_end);
    const std::size_t first_space = first_line.find(' ');
    const std::size_t second_space = first_space == std::string_view::npos ? std::string_view::npos : first_line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) return;
    const std::string_view method = first_line.substr(0, first_space);
    const std::string_view target = first_line.substr(first_space + 1U, second_space - first_space - 1U);
    if (method != "GET" && method != "HEAD") return;

    response_t response = route(database, database_path, target);
    std::ostringstream headers;
    headers << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\n"
            << "Content-Type: " << response.content_type << "\r\n"
            << "Content-Length: " << response.body.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "Referrer-Policy: no-referrer\r\n"
            << "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n"
            << "Connection: close\r\n\r\n";
    const std::string header_text = headers.str();
    if (!send_all(client, header_text)) return;
    if (method == "GET" && !response.body.empty()) (void)send_all(client, response.body);
}

std::filesystem::path default_database_path()
{
    std::wstring buffer(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::filesystem::path(L"output") / L"image.db";
    buffer.resize(length);
    const auto executable = std::filesystem::path(buffer);
    const auto parent = executable.parent_path();
    if (_wcsicmp(parent.filename().c_str(), L"bin") == 0)
        return parent.parent_path() / L"output" / L"image.db";
    return parent / L"output" / L"image.db";
}

}

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path database_path = argc >= 2 ? std::filesystem::path(argv[1]) : default_database_path();
    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(database_path, exists_error) || exists_error)
    {
        std::wcerr << L"error: Nylon audit database was not found: " << database_path << L'\n';
        return 1;
    }

    std::string database_error;
    auto database_result = database_t::open_readonly(database_path, database_error);
    if (!database_result.has_value())
    {
        std::cerr << "error: Unable to open Nylon database read-only: " << database_error << '\n';
        return 1;
    }
    auto database = std::move(*database_result);
    const auto schema = scalar_text(database.get(), "SELECT value FROM metadata WHERE meta_key='schema_version';");
    if (!schema.has_value() || *schema != "2")
    {
        std::cerr << "error: Nylon Viewer requires schema version 2\n";
        return 1;
    }

    WSADATA wsa{};
    const int wsa_rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsa_rc != 0)
    {
        std::cerr << "error: WSAStartup failed (" << wsa_rc << ")\n";
        return 1;
    }

    socket_guard_t server(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (server.get() == INVALID_SOCKET)
    {
        std::cerr << "error: Unable to create viewer socket\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(server.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server.get(), 16) == SOCKET_ERROR)
    {
        std::cerr << "error: Unable to bind local viewer server\n";
        WSACleanup();
        return 1;
    }

    int address_length = sizeof(address);
    if (getsockname(server.get(), reinterpret_cast<sockaddr*>(&address), &address_length) == SOCKET_ERROR)
    {
        std::cerr << "error: Unable to determine viewer port\n";
        WSACleanup();
        return 1;
    }
    const unsigned short port = ntohs(address.sin_port);
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
    std::wcout << L"Nylon Viewer\n"
               << L"database: " << database_path << L"\n"
               << L"url: " << url << L"\n"
               << L"Press Ctrl+C to stop the local viewer.\n";

    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
        std::wcout << L"Open this URL manually: " << url << L'\n';

    for (;;)
    {
        SOCKET client = accept(server.get(), nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        serve_client(client, database.get(), database_path);
    }

    server.reset();
    WSACleanup();
    return 0;
}
