// Semantic runtime contract shim for the pinned llama.cpp b11146 server.
//
// The original lifter implementation remains the planner/parser/validator core.
// This translation unit replaces only the model request dispatch and top-level
// entry point so maps created with the old, unconstrained response_format shape
// are invalidated exactly once.

#define worker_loop nylon_semantic_v1_worker_loop
#define wmain nylon_semantic_v1_wmain
#include "semantic.cpp"
#undef wmain
#undef worker_loop

namespace
{

constexpr std::string_view request_contract_version = "2";

bool request_contract_compatible(const std::filesystem::path& path, std::string_view fingerprint)
{
    if (!map_compatible(path, fingerprint)) return false;
    try
    {
        auto database = database_t::open_readonly(path);
        const auto contract = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='request_contract_version';");
        return contract.has_value() && *contract == request_contract_version;
    }
    catch (...)
    {
        return false;
    }
}

void write_request_contract(sqlite3* map)
{
    statement_t statement(map, "INSERT OR REPLACE INTO metadata(key,value) VALUES('request_contract_version',?1);");
    sqlite3_bind_text(statement.get(), 1, request_contract_version.data(), static_cast<int>(request_contract_version.size()), SQLITE_STATIC);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

std::string build_request_v2(const prompt_t& prompt)
{
    const std::string schema = output_schema_json();
    std::ostringstream request;
    request << "{\"messages\":[{\"role\":\"system\",\"content\":\"" << json_escape(system_prompt())
            << "\"},{\"role\":\"user\",\"content\":\"" << json_escape(prompt.text)
            << "\"}],\"temperature\":0,\"max_tokens\":512,\"stream\":false,"
            << "\"chat_template_kwargs\":{\"enable_thinking\":false},"
            << "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":" << schema << "}}}";
    return request.str();
}

void worker_loop_v2(
    const std::filesystem::path& source_path,
    const std::filesystem::path& map_path,
    const std::vector<std::int64_t>& jobs,
    std::atomic<std::size_t>& cursor)
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
            const std::string request = build_request_v2(prompt);
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
        if (!request_contract_compatible(map_path, fingerprint)) remove_map_files(map_path);
        auto map = database_t::open_map(map_path);
        initialize_map(map.get(), fingerprint, source_path);
        write_request_contract(map.get());

        std::cout << "Nylon Semantic Lifter\n"
                  << "source: " << path_utf8(source_path) << '\n'
                  << "map:    " << path_utf8(map_path) << '\n'
                  << "model:  " << model_name << '\n'
                  << "request contract: " << request_contract_version << " (llama.cpp b11146 structured output)\n";

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
                workers.emplace_back(worker_loop_v2, source_path, map_path, std::cref(jobs), std::ref(cursor));
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
