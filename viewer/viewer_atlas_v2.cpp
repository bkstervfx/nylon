#include <mutex>
#include <unordered_map>

#define api_semantic_status nylon_atlas_v1_api_semantic_status
#define atlas_route nylon_atlas_v1_route
#define atlas_serve_client nylon_atlas_v1_serve_client
#define wmain nylon_atlas_v1_wmain
#include "viewer_atlas.cpp"
#undef wmain
#undef atlas_serve_client
#undef atlas_route
#undef api_semantic_status

namespace
{

std::mutex atlas_status_mutex;
std::unordered_map<std::int64_t, int> atlas_last_status;

std::string api_semantic_status_v2(const std::filesystem::path& source)
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

response_t atlas_route_v2(sqlite3* database, const std::filesystem::path& database_path, std::string_view target)
{
    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    const auto query = question == std::string_view::npos ? std::map<std::string, std::string>{} : parse_query(target.substr(question + 1U));
    if (path == "/" || path == "/index.html") return response_t{200, "text/html; charset=utf-8", enhanced_html()};
    if (path == "/api/semantic/status") return response_t{200, "application/json; charset=utf-8", api_semantic_status_v2(database_path)};
    if (path == "/api/semantic/jobs") return response_t{200, "application/json; charset=utf-8", api_semantic_jobs(database_path)};
    if (path == "/api/semantic/job")
    {
        const auto found = query.find("id");
        if (found == query.end()) return response_t{400, "application/json; charset=utf-8", api_error("Missing semantic job id")};
        return response_t{200, "application/json; charset=utf-8", api_semantic_job(database_path, found->second)};
    }
    return nylon_base_route(database, database_path, target);
}

void atlas_serve_client_v2(SOCKET client, sqlite3* database, const std::filesystem::path& database_path)
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

    response_t response = atlas_route_v2(database, database_path, target);
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

} // namespace

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
    if (wsa_rc != 0) return 1;
    socket_guard_t server(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (server.get() == INVALID_SOCKET) { WSACleanup(); return 1; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(server.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server.get(), 16) == SOCKET_ERROR)
    {
        WSACleanup();
        return 1;
    }
    int address_length = sizeof(address);
    if (getsockname(server.get(), reinterpret_cast<sockaddr*>(&address), &address_length) == SOCKET_ERROR)
    {
        WSACleanup();
        return 1;
    }
    const unsigned short port = ntohs(address.sin_port);
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
    std::wcout << L"Nylon Viewer\n"
               << L"database: " << database_path << L"\n"
               << L"semantic map: " << semantic_map_path(database_path) << L"\n"
               << L"url: " << url << L"\n"
               << L"Press Ctrl+C to stop the local viewer.\n";
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) std::wcout << L"Open this URL manually: " << url << L'\n';
    for (;;)
    {
        SOCKET client = accept(server.get(), nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        atlas_serve_client_v2(client, database.get(), database_path);
    }
    server.reset();
    WSACleanup();
    return 0;
}
