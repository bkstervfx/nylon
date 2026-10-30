// Nylon semantic request contract v3.
//
// The planner, source extraction, deterministic pass, and map schema remain in
// semantic.cpp. This translation unit narrows the model's responsibility to a
// small grounded semantic delta. The model no longer creates local IDs or
// relationship edges; Nylon assigns IDs and reconstructs hierarchy/edges
// deterministically after lifting.

#define worker_loop nylon_semantic_v1_worker_loop
#define wmain nylon_semantic_v1_wmain
#include "semantic.cpp"
#undef wmain
#undef worker_loop

namespace
{

constexpr std::string_view request_contract_version_v3 = "3";

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
    if (position != std::string::npos)
        prompt.text.replace(position, old_text.size(), new_text);
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
            if (prompt.allowed_refs.contains(ref) && seen.insert(ref).second)
                entity.refs.push_back(ref);
        }
        // Ungrounded model entities are discarded rather than poisoning the
        // whole work unit. The group itself remains grounded to planned roots.
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

void worker_loop_v3(
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
            const prompt_t prompt = build_prompt_v3(source.get(), job);
            const std::string input_hash = sha256_hex(prompt.text);
            set_running(map.get(), job_id, input_hash);
            const std::string request = build_request_v3(prompt);
            const http_result_t response = http_request(L"POST", L"/v1/chat/completions", request);
            if (response.status < 200U || response.status >= 300U)
                throw std::runtime_error("llama.cpp returned HTTP " + std::to_string(response.status));
            const std::string finish_reason = json_scalar_text(map.get(), response.body, "$.choices[0].finish_reason");
            if (finish_reason == "length")
                throw std::runtime_error("Model output reached the 1024-token safety cap");
            const std::string delta = extract_model_content(map.get(), response.body);
            apply_ai_delta_v3(map.get(), job, prompt, delta);
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
        if (!request_contract_compatible_v3(map_path, fingerprint)) remove_map_files(map_path);
        auto map = database_t::open_map(map_path);
        initialize_map(map.get(), fingerprint, source_path);
        write_request_contract_v3(map.get());

        std::cout << "Nylon Semantic Lifter\n"
                  << "source: " << path_utf8(source_path) << '\n'
                  << "map:    " << path_utf8(map_path) << '\n'
                  << "model:  " << model_name << '\n'
                  << "request contract: " << request_contract_version_v3 << " (grounded entities; deterministic edges)\n";

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
                workers.emplace_back(worker_loop_v3, source_path, map_path, std::cref(jobs), std::ref(cursor));
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
