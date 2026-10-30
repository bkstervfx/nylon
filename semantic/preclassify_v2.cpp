#define wmain nylon_preclassify_v1_wmain
#include "preclassify.cpp"
#undef wmain

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

        statement_t jobs(map.get(), "SELECT id,roots,mode,value_limit,source_count FROM lift_jobs WHERE status=0 AND deterministic_kind='' ORDER BY id;");
        statement_t update(map.get(), "UPDATE lift_jobs SET deterministic_kind=?2 WHERE id=?1 AND status=0 AND deterministic_kind='';");
        statement_t path_lookup(source.get(), "SELECT path FROM registry_keys WHERE id=?1;");
        std::unordered_map<std::string, std::size_t> counts;
        std::size_t classified = 0;
        std::size_t evidence_free = 0;

        exec(map.get(), "BEGIN IMMEDIATE;");
        try
        {
            while (sqlite3_step(jobs.get()) == SQLITE_ROW)
            {
                const std::int64_t job_id = sqlite3_column_int64(jobs.get(), 0);
                const auto roots = parse_roots(text(jobs.get(), 1));
                const std::string mode = text(jobs.get(), 2);
                const std::size_t value_limit = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(jobs.get(), 3)));
                const std::size_t source_count = static_cast<std::size_t>(std::max<std::int64_t>(0, sqlite3_column_int64(jobs.get(), 4)));

                std::string common_kind;
                bool paths_agree = !roots.empty();
                for (const std::int64_t root : roots)
                {
                    const std::string kind = classify_path(root_path(path_lookup, root));
                    if (kind.empty())
                    {
                        paths_agree = false;
                        break;
                    }
                    if (common_kind.empty()) common_kind = kind;
                    else if (common_kind != kind)
                    {
                        paths_agree = false;
                        break;
                    }
                }

                // A single raw source is necessarily a bare key with no values.
                // Likewise an own-mode job with value_limit=0 has no value evidence.
                // An LLM cannot recover additional grounded semantics from that, so
                // represent it neutrally and spend zero model tokens.
                if ((!paths_agree || common_kind.empty()) &&
                    (source_count == 1U || (mode == "own" && value_limit == 0U)))
                {
                    common_kind = "registry_configuration";
                    paths_agree = true;
                    ++evidence_free;
                }

                if (!paths_agree || common_kind.empty()) continue;
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
        if (evidence_free != 0U) std::cout << " | " << evidence_free << " evidence-free containers skipped";
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
