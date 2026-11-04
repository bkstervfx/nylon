// Nylon semantic runtime v4.
//
// Request contract remains v3, so existing valid image.map.db progress is reusable.
// This translation unit removes avoidable orchestration costs:
//   * reuse an already-persisted plan instead of rescanning image.db on resume
//   * batch deterministic commits instead of one SQLite transaction per job
//   * reuse one WinHTTP session/connection per worker
//   * retry failed model jobs while the model stays resident in VRAM
//   * increment retry attempts before prompt construction, so local failures cannot loop forever

#define worker_loop nylon_semantic_base_worker_loop
#define wmain nylon_semantic_base_wmain
#include "semantic.cpp"
#undef wmain
#undef worker_loop

namespace
{

constexpr std::string_view request_contract_version_v3 = "3";
constexpr std::size_t deterministic_batch_size = 512U;

bool request_contract_compatible_v3(const std::filesystem::path& path, std::string_view fingerprint)
{
    if (!map_compatible(path, fingerprint)) return false;
    try
    {
        auto database = database_t::open_readonly(path);
        const auto contract = scalar_text(database.get(), "SELECT value FROM metadata WHERE key='request_contract_version';");
        return contract.has_value() && *contract == request_contract_version_v3;
    }
    catch (...)
    {
        return false;
    }
}

void write_request_contract_v3(sqlite3* map)
{
    statement_t statement(map, "INSERT OR REPLACE INTO metadata(key,value) VALUES('request_contract_version',?1);");
    sqlite3_bind_text(statement.get(), 1, request_contract_version_v3.data(), static_cast<int>(request_contract_version_v3.size()), SQLITE_STATIC);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(map));
}

prompt_t build_prompt_v3(sqlite3* source, const job_t& job)
{
    prompt_t prompt = build_prompt(source, job);
    constexpr std::string_view old_text = "Relationships may reference local entity IDs or the literal local ID group.\n\n";
    constexpr std::string_view new_text = "Do not generate relationship edges; Nylon reconstructs them deterministically.\n\n";
    const std::size_t position = prompt.text.find(old_text);
    if (position != std::string::npos) prompt.text.replace(position, old_text.size(), new_text);
    return prompt;
}

std::string output_schema_json_v3()
{
    return R"JSON({"type":"object","additionalProperties":false,"properties":{"group_kind":{"type":"string","enum":["configuration_group","registry_configuration","service","driver","application","component","executable","library","user","group","security_policy","network_configuration","device","com_class","shell_extension","startup_entry","scheduled_task","certificate","file_association","protocol_handler","environment_setting","unknown_configuration"]},"group_label":{"type":"string","maxLength":80},"summary":{"type":"string","maxLength":180},"entities":{"type":"array","maxItems":6,"items":{"type":"object","additionalProperties":false,"properties":{"kind":{"type":"string","enum":["registry_configuration","service","driver","application","component","executable","library","user","group","security_policy","network_configuration","device","com_class","shell_extension","startup_entry","scheduled_task","certificate","file_association","protocol_handler","environment_setting","unknown_configuration"]},"label":{"type":"string","maxLength":80},"source_refs":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"string","maxLength":32}}},"required":["kind","label","source_refs"]}},"unresolved":{"type":"array","maxItems":8,"items":{"type":"string","maxLength":120}}},"required":["group_kind","group_label","summary","entities","unresolved"]})JSON";
}

std::string system_prompt_v3()
{
    return "You are the Nylon Semantic Lifter. Convert bounded offline Windows registry evidence into a compact neutral semantic label set. "
           "This is structural normalization, not malware analysis. Never invent missing files, users, services, or facts. "
           "Return only the schema fields group_kind, group_label, summary, entities, unresolved. "
           "Do not create IDs or relationship edges; Nylon reconstructs those deterministically. "
           "Every entity must be grounded by one or more exact rk:/rv: source_refs shown in the evidence. "
           "Prefer unknown_configuration over guessing. Keep output terse.";
}

std::string build_request_v3(const prompt_t& prompt)
{
    const std::string schema = output_schema_json_v3();
    std::ostringstream request;
    request << "{\"messages\":[{\"role\":\"system\",\"content\":\"" << json_escape(system_prompt_v3())
            << "\"},{\"role\":\"user\",\"content\":\"" << json_escape(prompt.text)
            << "\"}],\"temperature\":0,\"max_tokens\":1024,\"stream\":false,"
            << "\"chat_template_kwargs\":{\"enable_thinking\":false},"
            << "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":" << schema << "}}}";
    return request.str();
}

void apply_ai_delta_v3(sqlite3* map, const job_t& job, const prompt_t& prompt, std::string_view delta)
{
    statement_t valid(map, "SELECT json_valid(?1);");
    sqlite3_bind_text(valid.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(valid.get()) != SQLITE_ROW || sqlite3_column_int(valid.get(), 0) == 0)
        throw std::runtime_error("Model delta was not valid JSON");

    const std::string group_kind = json_scalar_text(map, delta, "$.group_kind");
    const std::string group_label = json_scalar_text(map, delta, "$.group_label");
    const std::string summary = json_scalar_text(map, delta, "$.summary");
    if (!semantic_kinds.contains(group_kind) || group_label.empty())
        throw std::runtime_error("Model delta failed group ontology validation");

    struct grounded_entity_t
    {
        std::string kind;
        std::string label;
        std::vector<std::string> refs;
    };
    std::vector<grounded_entity_t> grounded;

    statement_t entities(map, "SELECT json_extract(value,'$.kind'),json_extract(value,'$.label'),value FROM json_each(?1,'$.entities');");
    sqlite3_bind_text(entities.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(entities.get()) == SQLITE_ROW)
    {
        grounded_entity_t entity;
        entity.kind = column_text(entities.get(), 0);
        entity.label = column_text(entities.get(), 1);
        if (!semantic_kinds.contains(entity.kind) || entity.label.empty()) continue;

        const std::string entity_json = column_text(entities.get(), 2);
        statement_t refs(map, "SELECT value FROM json_each(?1,'$.source_refs');");
        sqlite3_bind_text(refs.get(), 1, entity_json.c_str(), static_cast<int>(entity_json.size()), SQLITE_TRANSIENT);
        std::unordered_set<std::string> seen;
        while (sqlite3_step(refs.get()) == SQLITE_ROW)
        {
            const std::string ref = column_text(refs.get(), 0);
            if (prompt.allowed_refs.contains(ref) && seen.insert(ref).second) entity.refs.push_back(ref);
        }
        if (!entity.refs.empty()) grounded.push_back(std::move(entity));
    }

    exec(map, "BEGIN IMMEDIATE;");
    try
    {
        delete_job_output(map, job.id);
        const std::int64_t group_id = insert_semantic_node(map, job.id, std::nullopt, "group", group_kind, group_label, summary, 0.85);
        for (const auto& ref : prompt.root_refs) insert_source(map, group_id, ref);

        for (std::size_t i = 0; i < grounded.size(); ++i)
        {
            const auto& entity = grounded[i];
            const std::string local = "e" + std::to_string(i + 1U);
            const std::int64_t entity_id = insert_semantic_node(
                map, job.id, group_id, local, entity.kind, entity.label,
                "Grounded semantic entity recovered from this work unit.", 0.85);
            insert_edge(map, job.id, group_id, "contains", entity_id);
            for (const auto& ref : entity.refs) insert_source(map, entity_id, ref);
        }

        statement_t unresolved(map, "SELECT value FROM json_each(?1,'$.unresolved');");
        sqlite3_bind_text(unresolved.get(), 1, delta.data(), static_cast<int>(delta.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(unresolved.get()) == SQLITE_ROW)
        {
            const std::string text = column_text(unresolved.get(), 0);
            if (text.empty()) continue;
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

std::string deterministic_group_label(std::string_view kind, std::size_t root_count)
{
    const bool plural = root_count != 1U;
    if (kind == "service") return plural ? "Windows services" : "Windows service";
    if (kind == "driver") return plural ? "Windows drivers" : "Windows driver";
    if (kind == "com_class") return plural ? "COM registrations" : "COM registration";
    if (kind == "startup_entry") return "Startup configuration";
    if (kind == "device") return "Device configuration";
    if (kind == "application") return plural ? "Application registrations" : "Application registration";
    if (kind == "component") return plural ? "Windows components" : "Windows component";
    if (kind == "network_configuration") return "Network configuration";
    if (kind == "environment_setting") return "Environment configuration";
    if (kind == "security_policy") return "Security policy";
    if (kind == "file_association") return plural ? "File associations" : "File association";
    if (kind == "shell_extension") return plural ? "Shell extensions" : "Shell extension";
    if (kind == "certificate") return plural ? "Certificates" : "Certificate";
    return "Registry configuration";
}

void apply_deterministic_no_transaction(sqlite3* source, sqlite3* map, const job_t& job)
{
    delete_job_output(map, job.id);
    const std::string group_label = deterministic_group_label(job.deterministic_kind, job.roots.size());
    const std::int64_t group_id = insert_semantic_node(
        map, job.id, std::nullopt, "group", "configuration_group", group_label,
        "Deterministically recognized Windows registry structure.", 1.0);
    for (std::size_t i = 0; i < job.roots.size(); ++i)
    {
        const std::string local = "e" + std::to_string(i + 1U);
        const std::string label = root_label(source, job.roots[i]);
        const std::int64_t entity = insert_semantic_node(
            map, job.id, group_id, local, job.deterministic_kind, label,
            "Known Windows registry structure.", 1.0);
        insert_edge(map, job.id, group_id, "contains", entity);
        insert_source(map, entity, "rk:" + std::to_string(job.roots[i]));
    }
    mark_job(map, job.id, 2);
}

void run_deterministic_pass_batched(sqlite3* source, sqlite3* map)
{
    statement_t jobs(map, "SELECT id FROM lift_jobs WHERE deterministic_kind<>'' AND status NOT IN (2,3) ORDER BY id;");
    std::vector<std::int64_t> ids;
    while (sqlite3_step(jobs.get()) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(jobs.get(), 0));
    if (ids.empty()) return;

    for (std::size_t begin = 0; begin < ids.size(); begin += deterministic_batch_size)
    {
        const std::size_t end = std::min(ids.size(), begin + deterministic_batch_size);
        exec(map, "BEGIN IMMEDIATE;");
        try
        {
            for (std::size_t index = begin; index < end; ++index)
                apply_deterministic_no_transaction(source, map, load_job(map, ids[index]));
            exec(map, "COMMIT;");
        }
        catch (...)
        {
            try { exec(map, "ROLLBACK;"); } catch (...) {}
            throw;
        }
    }
}

class persistent_llama_client_t
{
public:
    persistent_llama_client_t()
    {
        session_ = WinHttpOpen(L"NylonSemantic/4", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_ == nullptr) throw std::runtime_error("WinHttpOpen failed");
        WinHttpSetTimeouts(session_, 5000, 5000, 120000, 120000);
        connection_ = WinHttpConnect(session_, L"127.0.0.1", llama_port, 0);
        if (connection_ == nullptr)
        {
            WinHttpCloseHandle(session_);
            session_ = nullptr;
            throw std::runtime_error("WinHttpConnect failed");
        }
    }

    ~persistent_llama_client_t()
    {
        if (connection_ != nullptr) WinHttpCloseHandle(connection_);
        if (session_ != nullptr) WinHttpCloseHandle(session_);
    }

    persistent_llama_client_t(const persistent_llama_client_t&) = delete;
    persistent_llama_client_t& operator=(const persistent_llama_client_t&) = delete;

    http_result_t post(std::string_view body)
    {
        internet_handle_t request(WinHttpOpenRequest(
            connection_, L"POST", L"/v1/chat/completions", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        if (request.get() == nullptr) throw std::runtime_error("WinHttpOpenRequest failed");
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        LPVOID data = const_cast<char*>(body.data());
        const DWORD data_size = static_cast<DWORD>(body.size());
        if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1L), data, data_size, data_size, 0) ||
            !WinHttpReceiveResponse(request.get(), nullptr))
            throw std::runtime_error("Local llama.cpp request failed");

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
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
            if (!WinHttpReadData(request.get(), response.data() + old_size, available, &read))
                throw std::runtime_error("Unable to read llama.cpp response");
            response.resize(old_size + read);
        }
        return http_result_t{status, std::move(response)};
    }

private:
    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
};

void worker_loop_v4(
    const std::filesystem::path& source_path,
    const std::filesystem::path& map_path,
    const std::vector<std::int64_t>& jobs,
    std::atomic<std::size_t>& cursor)
{
    auto source = database_t::open_readonly(source_path);
    auto map = database_t::open_map(map_path);
    exec(source.get(), "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY;");
    exec(map.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
    persistent_llama_client_t client;

    for (;;)
    {
        const std::size_t index = cursor.fetch_add(1U);
        if (index >= jobs.size()) break;
        const std::int64_t job_id = jobs[index];
        try
        {
            const job_t job = load_job(map.get(), job_id);
            // Mark running before doing any local prompt work. This both avoids an
            // unused per-job SHA-256 and guarantees malformed local evidence cannot
            // bypass the retry budget forever.
            set_running(map.get(), job_id, {});
            const prompt_t prompt = build_prompt_v3(source.get(), job);
            const std::string request = build_request_v3(prompt);
            const http_result_t response = client.post(request);
            if (response.status < 200U || response.status >= 300U)
                throw std::runtime_error("llama.cpp returned HTTP " + std::to_string(response.status));
            const std::string finish_reason = json_scalar_text(map.get(), response.body, "$.choices[0].finish_reason");
            if (finish_reason == "length") throw std::runtime_error("Model output reached the 1024-token safety cap");
            const std::string delta = extract_model_content(map.get(), response.body);
            apply_ai_delta_v3(map.get(), job, prompt, delta);
        }
        catch (const std::exception& error)
        {
            try { mark_job(map.get(), job_id, 4, error.what()); } catch (...) {}
            std::lock_guard lock(console_mutex);
            std::cerr << "semantic job " << job_id << " failed: " << error.what() << '\n';
        }
        if ((index % 64U) == 0U) print_progress(map.get());
    }
}

bool persisted_plan_available(sqlite3* map)
{
    return scalar_i64(map, "SELECT COUNT(*) FROM lift_jobs;") > 0;
}

void dispatch_model_round(
    const std::filesystem::path& source_path,
    const std::filesystem::path& map_path,
    const std::vector<std::int64_t>& jobs)
{
    std::atomic<std::size_t> cursor{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (unsigned i = 0; i < worker_count; ++i)
        workers.emplace_back(worker_loop_v4, source_path, map_path, std::cref(jobs), std::ref(cursor));
    for (auto& worker : workers) worker.join();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        bool plan_only = false;
        bool deterministic_only = false;
        std::filesystem::path source_path;
        for (int i = 1; i < argc; ++i)
        {
            if (std::wstring_view(argv[i]) == L"--plan-only") plan_only = true;
            else if (std::wstring_view(argv[i]) == L"--deterministic-only") deterministic_only = true;
            else if (source_path.empty()) source_path = argv[i];
            else throw std::runtime_error("usage: nylon-semantic.exe [image.db] [--plan-only|--deterministic-only]");
        }
        if (plan_only && deterministic_only) throw std::runtime_error("Choose only one semantic execution mode");
        if (source_path.empty()) source_path = default_database_path();

        std::error_code error;
        if (!std::filesystem::is_regular_file(source_path, error) || error)
            throw std::runtime_error("Nylon source database was not found");

        auto source_lock = nylon::semantic_binding::lock_source(source_path);
        auto source = database_t::open_readonly(source_path);
        nylon::semantic_binding::require_registry_policy(source.get());
        exec(source.get(), "PRAGMA query_only=ON; PRAGMA temp_store=MEMORY;");
        const auto schema = scalar_text(source.get(), "SELECT value FROM metadata WHERE meta_key='schema_version';");
        if (!schema.has_value() || (*schema != "2" && *schema != "3" && *schema != "4" && *schema != "5" && *schema != "6" && *schema != "7"))
            throw std::runtime_error("Semantic lifter requires compatible Nylon registry schema 2, 3, 4, 5, 6 or 7");

        const std::string fingerprint = source_fingerprint(source.get(), source_path);
        const std::filesystem::path map_path = map_path_for(source_path);
        if (!request_contract_compatible_v3(map_path, fingerprint)) remove_map_files(map_path);
        auto map = database_t::open_map(map_path);
        initialize_map(map.get(), fingerprint, source_path, scalar_text(source.get(), "SELECT value FROM metadata WHERE meta_key='audit_id';").value_or(""));
        write_request_contract_v3(map.get());

        std::cout << "Nylon Semantic Lifter\n"
                  << "source: " << path_utf8(source_path) << '\n'
                  << "map:    " << path_utf8(map_path) << '\n'
                  << "model:  " << model_name << '\n'
                  << "request contract: " << request_contract_version_v3 << " (grounded entities; deterministic edges)\n"
                  << "runtime: performance-v1 (plan reuse; batched deterministic writes; hot retries; persistent HTTP)\n";

        if (!persisted_plan_available(map.get()))
        {
            const auto plan = build_plan(source.get());
            persist_plan(map.get(), plan);
            const std::int64_t total_sources = scalar_i64(map.get(), "SELECT COALESCE(SUM(source_count),0) FROM lift_jobs;");
            std::cout << "plan:   " << plan.size() << " work units | " << total_sources << " raw sources\n";
        }
        else
        {
            const std::int64_t jobs = scalar_i64(map.get(), "SELECT COUNT(*) FROM lift_jobs;");
            const std::int64_t total_sources = scalar_i64(map.get(), "SELECT COALESCE(SUM(source_count),0) FROM lift_jobs;");
            std::cout << "plan:   reused " << jobs << " persisted work units | " << total_sources << " raw sources\n";
        }

        run_deterministic_pass_batched(source.get(), map.get());
        print_progress(map.get());
        if (plan_only || deterministic_only)
        {
            std::cout << (plan_only ? "plan-only complete\n" : "deterministic-only complete\n");
            return 0;
        }

        std::vector<std::int64_t> jobs = pending_jobs(map.get());
        if (!jobs.empty())
        {
            process_guard_t model_process;
            ensure_llama_server(model_process);
            for (unsigned round = 1; round <= 3U; ++round)
            {
                jobs = pending_jobs(map.get());
                if (jobs.empty()) break;
                if (round == 1U)
                    std::cout << "local model ready on RX/Vulkan backend; processing " << jobs.size() << " AI work units\n";
                else
                    std::cout << "retrying " << jobs.size() << " failed AI work units while model remains resident (round " << round << "/3)\n";
                dispatch_model_round(source_path, map_path, jobs);
            }
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
