#include <mutex>
#include <unordered_map>

namespace
{

std::mutex atlas_status_mutex;
std::unordered_map<std::int64_t, int> atlas_last_status;

std::string api_semantic_status(const std::filesystem::path& source)
{
    std::string error;
    auto map_result = open_semantic_map(source, error);
    if (!map_result.has_value()) return "{\"available\":false}";
    auto map = std::move(*map_result);
    sqlite3* database = map.get();
    sqlite3_stmt* statement = nullptr;
    const char* sql = R"SQL(
SELECT COUNT(*),
       COALESCE(SUM(CASE WHEN status IN (2,3) THEN 1 ELSE 0 END),0),
       COALESCE(SUM(CASE WHEN status=1 THEN 1 ELSE 0 END),0),
       COALESCE(SUM(CASE WHEN status=4 THEN 1 ELSE 0 END),0),
       COALESCE(SUM(source_count),0),
       COALESCE(SUM(CASE WHEN status IN (2,3) THEN source_count ELSE 0 END),0)
FROM lift_jobs;
)SQL";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
    if (sqlite3_step(statement) != SQLITE_ROW)
    {
        sqlite3_finalize(statement);
        return api_error("Unable to read semantic progress");
    }
    const auto jobs = sqlite3_column_int64(statement, 0);
    const auto done = sqlite3_column_int64(statement, 1);
    const auto running = sqlite3_column_int64(statement, 2);
    const auto failed = sqlite3_column_int64(statement, 3);
    const auto sources = sqlite3_column_int64(statement, 4);
    const auto mapped = sqlite3_column_int64(statement, 5);
    sqlite3_finalize(statement);

    const std::string complete = scalar_text(database, "SELECT value FROM metadata WHERE key='complete';").value_or("0");
    const std::string model = scalar_text(database, "SELECT value FROM metadata WHERE key='model_name';").value_or("");

    std::vector<std::pair<std::int64_t, int>> changed;
    if (sqlite3_prepare_v2(database, "SELECT id,status FROM lift_jobs ORDER BY id;", -1, &statement, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    {
        std::lock_guard lock(atlas_status_mutex);
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const std::int64_t id = sqlite3_column_int64(statement, 0);
            const int status = sqlite3_column_int(statement, 1);
            const auto found = atlas_last_status.find(id);
            if (found == atlas_last_status.end() || found->second != status)
            {
                changed.emplace_back(id, status);
                atlas_last_status[id] = status;
            }
        }
    }
    sqlite3_finalize(statement);

    std::ostringstream out;
    out << "{\"available\":true,\"jobs\":" << jobs
        << ",\"done\":" << done
        << ",\"running\":" << running
        << ",\"failed\":" << failed
        << ",\"sources\":" << sources
        << ",\"mapped\":" << mapped
        << ",\"complete\":" << (complete == "1" ? "true" : "false")
        << ",\"model\":" << json_string(model)
        << ",\"states\":[";
    bool first = true;
    for (const auto& [id, status] : changed)
    {
        if (!first) out << ',';
        first = false;
        out << '[' << id << ',' << status << ']';
    }
    out << "]}";
    return out.str();
}

} // namespace
