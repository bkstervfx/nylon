#ifndef NYLON_VIEWER_SQLITE_PREPARE_HPP
#define NYLON_VIEWER_SQLITE_PREPARE_HPP

#include <sqlite3.h>

#include <string>
#include <string_view>

inline int nylon_viewer_prepare_v2(
    sqlite3* database,
    const char* sql,
    int byte_count,
    sqlite3_stmt** statement,
    const char** tail)
{
    if (sql == nullptr || tail != nullptr)
    {
        return sqlite3_prepare_v2(database, sql, byte_count, statement, tail);
    }

    const std::string_view input = byte_count < 0
        ? std::string_view(sql)
        : std::string_view(sql, static_cast<std::size_t>(byte_count));

    constexpr std::string_view invalid_escape = "ESCAPE '\\\\'";
    constexpr std::string_view valid_escape = "ESCAPE '\\'";
    if (input.find(invalid_escape) == std::string_view::npos)
    {
        return sqlite3_prepare_v2(database, sql, byte_count, statement, nullptr);
    }

    std::string normalized(input);
    std::size_t cursor = 0;
    while ((cursor = normalized.find(invalid_escape, cursor)) != std::string::npos)
    {
        normalized.replace(cursor, invalid_escape.size(), valid_escape);
        cursor += valid_escape.size();
    }

    return sqlite3_prepare_v2(
        database,
        normalized.c_str(),
        static_cast<int>(normalized.size()),
        statement,
        nullptr);
}

#define sqlite3_prepare_v2 nylon_viewer_prepare_v2

#endif
