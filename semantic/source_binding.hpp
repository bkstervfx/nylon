#ifndef NYLON_SEMANTIC_SOURCE_BINDING_HPP
#define NYLON_SEMANTIC_SOURCE_BINDING_HPP

#include <memory>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

#include "../src/platform/hash.hpp"
#include "../src/platform/win32.hpp"

namespace nylon::semantic_binding
{

inline void require_finalized(const std::filesystem::path& path)
{
    for (const auto* suffix : { L"-wal", L"-journal" })
    {
        const auto sidecar = std::filesystem::path(path.native() + suffix);
        std::error_code error;
        const bool present = std::filesystem::exists(sidecar, error);
        if (error) throw std::runtime_error("Cannot establish finalized raw database state");
        if (present)
        {
            const auto size = std::filesystem::file_size(sidecar, error);
            if (error || size != 0) throw std::runtime_error("Raw database has a nonempty WAL/journal; finalize it before semantic processing");
        }
    }
}

inline platform::unique_handle_t lock_source(const std::filesystem::path& path)
{
    auto locked = platform::open_read_lock(path);
    if (!locked.ok()) throw std::runtime_error(locked.error().message);
    require_finalized(path);
    return std::move(locked).value();
}

inline std::string fingerprint(const std::filesystem::path& path)
{
    require_finalized(path);
    auto hash = platform::sha256_file(path);
    if (!hash.ok()) throw std::runtime_error(hash.error().message);
    return "raw-db-sha256-v1:" + std::move(hash).value();
}

inline void require_map_binding(sqlite3* map, const std::filesystem::path& source)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(map, "SELECT value FROM metadata WHERE key='source_fingerprint';", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Semantic map has no readable source binding");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (sqlite3_step(raw) != SQLITE_ROW) throw std::runtime_error("Semantic map has no source binding");
    const auto* data = sqlite3_column_text(raw, 0);
    const std::string expected = data ? reinterpret_cast<const char*>(data) : "";
    if (expected != fingerprint(source)) throw std::runtime_error("Semantic map source binding mismatch; rebuild from this exact audit");
}

inline void require_registry_policy(sqlite3* source)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(source, "SELECT 1 FROM registry_hives WHERE name IS NULL OR upper(name) NOT IN ('SOFTWARE','SYSTEM','COMPONENTS','DEFAULT','BCD','BCD_EFI','AMCACHE') LIMIT 1;", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot establish registry input policy");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    const int result = sqlite3_step(raw);
    if (result == SQLITE_ROW) throw std::runtime_error("Sensitive or unreviewed registry hive input; re-scan with the current omission policy");
    if (result != SQLITE_DONE) throw std::runtime_error("Registry input policy query failed");
}

}

#endif
