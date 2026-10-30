#define route nylon_base_route
#define serve_client nylon_base_serve_client
#define wmain nylon_base_wmain
#include "viewer.cpp"
#undef wmain
#undef serve_client
#undef route

namespace
{

std::filesystem::path semantic_map_path(const std::filesystem::path& source)
{
    std::filesystem::path result = source;
    result.replace_extension(L".map.db");
    return result;
}

std::optional<database_t> open_semantic_map(const std::filesystem::path& source, std::string& error)
{
    const auto map = semantic_map_path(source);
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(map, filesystem_error) || filesystem_error) return std::nullopt;
    return database_t::open_readonly(map, error);
}

std::string api_semantic_status(const std::filesystem::path& source)
{
    std::string error;
    auto map_result = open_semantic_map(source, error);
    if (!map_result.has_value()) return "{\"available\":false}";
    auto map = std::move(*map_result);
    sqlite3* database = map.get();
    sqlite3_stmt* summary = nullptr;
    const char* sql = R"SQL(
SELECT COUNT(*),
       COALESCE(SUM(CASE WHEN status IN (2,3) THEN 1 ELSE 0 END),0),
       COALESCE(SUM(CASE WHEN status=1 THEN 1 ELSE 0 END),0),
       COALESCE(SUM(CASE WHEN status=4 THEN 1 ELSE 0 END),0),
       COALESCE(SUM(CASE WHEN status=2 THEN 1 ELSE 0 END),0),
       COALESCE(SUM(CASE WHEN status=3 THEN 1 ELSE 0 END),0),
       COALESCE(SUM(source_count),0),
       COALESCE(SUM(CASE WHEN status IN (2,3) THEN source_count ELSE 0 END),0)
FROM lift_jobs;
)SQL";
    if (sqlite3_prepare_v2(database, sql, -1, &summary, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(database));
    if (sqlite3_step(summary) != SQLITE_ROW)
    {
        sqlite3_finalize(summary);
        return api_error("Unable to read semantic progress");
    }
    const auto jobs = sqlite3_column_int64(summary, 0);
    const auto done = sqlite3_column_int64(summary, 1);
    const auto running = sqlite3_column_int64(summary, 2);
    const auto failed = sqlite3_column_int64(summary, 3);
    const auto deterministic = sqlite3_column_int64(summary, 4);
    const auto ai = sqlite3_column_int64(summary, 5);
    const auto sources = sqlite3_column_int64(summary, 6);
    const auto mapped = sqlite3_column_int64(summary, 7);
    sqlite3_finalize(summary);
    const std::string complete = scalar_text(database, "SELECT value FROM metadata WHERE key='complete';").value_or("0");
    const std::string model = scalar_text(database, "SELECT value FROM metadata WHERE key='model_name';").value_or("");

    std::ostringstream out;
    out << "{\"available\":true,\"jobs\":" << jobs
        << ",\"done\":" << done
        << ",\"running\":" << running
        << ",\"failed\":" << failed
        << ",\"deterministic\":" << deterministic
        << ",\"ai\":" << ai
        << ",\"sources\":" << sources
        << ",\"mapped\":" << mapped
        << ",\"complete\":" << (complete == "1" ? "true" : "false")
        << ",\"model\":" << json_string(model)
        << ",\"states\":[";
    sqlite3_stmt* states = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT id,status FROM lift_jobs ORDER BY id;", -1, &states, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    bool first = true;
    while (sqlite3_step(states) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        out << '[' << sqlite3_column_int64(states, 0) << ',' << sqlite3_column_int(states, 1) << ']';
    }
    sqlite3_finalize(states);
    out << "]}";
    return out.str();
}

std::string api_semantic_jobs(const std::filesystem::path& source)
{
    std::string error;
    auto map_result = open_semantic_map(source, error);
    if (!map_result.has_value()) return "[]";
    auto map = std::move(*map_result);
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT id,path,source_count,estimated_bytes,mode,deterministic_kind,status FROM lift_jobs ORDER BY id;";
    if (sqlite3_prepare_v2(map.get(), sql, -1, &statement, nullptr) != SQLITE_OK) return api_error(sqlite3_errmsg(map.get()));
    std::ostringstream out;
    out << '[';
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << sqlite3_column_int64(statement, 0)
            << ",\"path\":" << json_string(column_text(statement, 1))
            << ",\"sources\":" << sqlite3_column_int64(statement, 2)
            << ",\"bytes\":" << sqlite3_column_int64(statement, 3)
            << ",\"mode\":" << json_string(column_text(statement, 4))
            << ",\"known\":" << json_string(column_text(statement, 5))
            << ",\"status\":" << sqlite3_column_int(statement, 6) << '}';
    }
    sqlite3_finalize(statement);
    out << ']';
    return out.str();
}

std::string api_semantic_job(const std::filesystem::path& source, std::string_view id_text)
{
    const auto id = parse_i64(id_text);
    if (!id.has_value()) return api_error("Invalid semantic job id");
    std::string error;
    auto map_result = open_semantic_map(source, error);
    if (!map_result.has_value()) return api_error("Semantic map has not been created");
    auto map = std::move(*map_result);
    sqlite3_stmt* job = nullptr;
    if (sqlite3_prepare_v2(map.get(), "SELECT path,source_count,mode,deterministic_kind,status,attempts,error FROM lift_jobs WHERE id=?1;", -1, &job, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(map.get()));
    sqlite3_bind_int64(job, 1, *id);
    if (sqlite3_step(job) != SQLITE_ROW)
    {
        sqlite3_finalize(job);
        return api_error("Semantic work unit not found");
    }
    std::ostringstream out;
    out << "{\"id\":" << *id
        << ",\"path\":" << json_string(column_text(job, 0))
        << ",\"sources\":" << sqlite3_column_int64(job, 1)
        << ",\"mode\":" << json_string(column_text(job, 2))
        << ",\"known\":" << json_string(column_text(job, 3))
        << ",\"status\":" << sqlite3_column_int(job, 4)
        << ",\"attempts\":" << sqlite3_column_int(job, 5)
        << ",\"error\":" << json_string(column_text(job, 6))
        << ",\"nodes\":[";
    sqlite3_finalize(job);

    sqlite3_stmt* nodes = nullptr;
    if (sqlite3_prepare_v2(map.get(), "SELECT id,local_id,kind,label,summary,confidence FROM semantic_nodes WHERE job_id=?1 ORDER BY parent_id IS NOT NULL,id;", -1, &nodes, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(map.get()));
    sqlite3_bind_int64(nodes, 1, *id);
    bool first = true;
    while (sqlite3_step(nodes) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << sqlite3_column_int64(nodes, 0)
            << ",\"local\":" << json_string(column_text(nodes, 1))
            << ",\"kind\":" << json_string(column_text(nodes, 2))
            << ",\"label\":" << json_string(column_text(nodes, 3))
            << ",\"summary\":" << json_string(column_text(nodes, 4))
            << ",\"confidence\":" << sqlite3_column_double(nodes, 5) << '}';
    }
    sqlite3_finalize(nodes);
    out << "],\"unresolved\":[";
    sqlite3_stmt* unresolved = nullptr;
    if (sqlite3_prepare_v2(map.get(), "SELECT text FROM unresolved WHERE job_id=?1 ORDER BY id;", -1, &unresolved, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(map.get()));
    sqlite3_bind_int64(unresolved, 1, *id);
    first = true;
    while (sqlite3_step(unresolved) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        out << json_string(column_text(unresolved, 0));
    }
    sqlite3_finalize(unresolved);
    out << "]}";
    return out.str();
}

std::string enhanced_html()
{
    std::string html(viewer_html);
    const std::string css = R"HTML(
<style id="semantic-style">
.semantic-pane{position:absolute;inset:0;z-index:5;background:var(--bg);display:none;overflow:hidden}.semantic-pane.show{display:block}.semantic-head{position:absolute;left:16px;right:16px;top:14px;height:34px;display:flex;align-items:center;justify-content:space-between;z-index:2;pointer-events:none}.semantic-head strong{font-size:12px;font-weight:600;letter-spacing:.02em}.semantic-progress{font-size:11px;color:var(--dim);font-variant-numeric:tabular-nums}.semantic-canvas{position:absolute!important;inset:54px 12px 12px 12px!important;width:calc(100% - 24px)!important;height:calc(100% - 66px)!important}.semantic-tooltip{position:absolute;z-index:7;display:none;max-width:440px;padding:7px 9px;background:#15191ff4;border:1px solid var(--border2);border-radius:5px;color:var(--muted);font-size:11px;pointer-events:none;box-shadow:0 8px 24px #0006}.semantic-tooltip strong{display:block;color:var(--text);font-weight:550;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.semantic-empty{position:absolute;inset:0;display:grid;place-items:center;color:var(--dim);font-size:12px;text-align:center;pointer-events:none}.semantic-empty b{display:block;color:var(--text);font-size:15px;font-weight:600;margin-bottom:5px}.semantic-legend{position:absolute;left:16px;bottom:13px;display:flex;gap:13px;font-size:10px;color:var(--dim);z-index:3;pointer-events:none}.semantic-legend i{width:7px;height:7px;display:inline-block;margin-right:5px;border-radius:1px}.semantic-pane canvas{cursor:default}#semanticBtn.live::before{content:'';display:inline-block;width:6px;height:6px;border-radius:50%;background:#79d9a4;margin-right:6px;vertical-align:1px}#semanticBtn.running::before{background:#819fff;box-shadow:0 0 0 4px #819fff18}.sem-node{margin-top:12px;padding-top:10px;border-top:1px solid var(--border)}.sem-node:first-of-type{margin-top:14px}.sem-node-kind{color:var(--dim);font-size:10px;text-transform:uppercase;letter-spacing:.08em}.sem-node-label{font-weight:600;margin-top:2px}.sem-node-summary{color:var(--muted);margin-top:4px;line-height:1.5}
</style>
)HTML";
    const std::string button = "<button id=\"overviewBtn\">Overview</button>";
    const std::string buttons = button + "<button id=\"semanticBtn\" title=\"Semantic lift progress\">Semantic</button>";
    if (const auto pos = html.find(button); pos != std::string::npos) html.replace(pos, button.size(), buttons);
    const std::string pane = R"HTML(<div class="semantic-pane" id="semanticPane"><div class="semantic-head"><strong>Semantic lift</strong><span class="semantic-progress" id="semanticProgress">Not started</span></div><canvas class="semantic-canvas" id="semanticCanvas"></canvas><div class="semantic-tooltip" id="semanticTooltip"></div><div class="semantic-empty" id="semanticEmpty"><div><b>No semantic map yet</b>Run semantic.cmd to begin mapping this image.</div></div><div class="semantic-legend"><span><i style="background:#20262e"></i>Pending</span><span><i style="background:#819fff"></i>Active</span><span><i style="background:#66d29a"></i>Mapped</span><span><i style="background:#de747b"></i>Failed</span></div></div>)HTML";
    if (const auto pos = html.find("</main>"); pos != std::string::npos) html.insert(pos, pane);
    if (const auto pos = html.find("</head>"); pos != std::string::npos) html.insert(pos, css);

    const std::string script = R"HTML(
<script id="semantic-script">
(()=>{
'use strict';
const semBtn=document.querySelector('#semanticBtn'),pane=document.querySelector('#semanticPane'),sc=document.querySelector('#semanticCanvas'),tip=document.querySelector('#semanticTooltip'),empty=document.querySelector('#semanticEmpty'),progress=document.querySelector('#semanticProgress');
if(!semBtn||!pane||!sc)return;
const sctx=sc.getContext('2d');let semJobs=[],semState=new Map(),semView=false,semDpr=1,semHover=null,semLayout={size:0,gap:2,cols:1,x:0,y:0};const transitions=new Map();
const palette={0:[32,38,46],1:[129,159,255],2:[78,184,132],3:[102,210,154],4:[222,116,123]};
const statusName=s=>s===1?'Active':s===2?'Mapped · deterministic':s===3?'Mapped · local model':s===4?'Failed':'Pending';
function mix(a,b,t){return `rgb(${Math.round(a[0]+(b[0]-a[0])*t)},${Math.round(a[1]+(b[1]-a[1])*t)},${Math.round(a[2]+(b[2]-a[2])*t)})`}
function semResize(){const r=sc.getBoundingClientRect();semDpr=Math.max(1,Math.min(2,devicePixelRatio||1));sc.width=Math.max(1,Math.floor(r.width*semDpr));sc.height=Math.max(1,Math.floor(r.height*semDpr));if(!semJobs.length)return;const n=semJobs.length,ratio=Math.max(.25,r.width/Math.max(1,r.height)),cols=Math.max(1,Math.ceil(Math.sqrt(n*ratio))),rows=Math.ceil(n/cols);let gap=n>6000?1:2,size=Math.floor(Math.min((r.width-gap*(cols-1))/cols,(r.height-gap*(rows-1))/rows));size=Math.max(2,Math.min(16,size));const width=cols*size+(cols-1)*gap,height=rows*size+(rows-1)*gap;semLayout={size,gap,cols,x:Math.floor((r.width-width)/2),y:Math.max(0,Math.floor((r.height-height)/2))}}
new ResizeObserver(semResize).observe(sc);
function tilePosition(i){const {size,gap,cols,x,y}=semLayout;return{x:x+(i%cols)*(size+gap),y:y+Math.floor(i/cols)*(size+gap),size}}
function drawSemantic(now){if(semView){const r=sc.getBoundingClientRect();sctx.setTransform(semDpr,0,0,semDpr,0,0);sctx.clearRect(0,0,r.width,r.height);for(let i=0;i<semJobs.length;i++){const j=semJobs[i],p=tilePosition(i),state=semState.get(j.id)??j.status??0,tr=transitions.get(j.id);let color=palette[state]||palette[0];if(tr){const t=Math.min(1,(now-tr.at)/280);color=[Math.round(tr.from[0]+(tr.to[0]-tr.from[0])*t),Math.round(tr.from[1]+(tr.to[1]-tr.from[1])*t),Math.round(tr.from[2]+(tr.to[2]-tr.from[2])*t)];if(t>=1)transitions.delete(j.id)}if(state===1){const pulse=.72+.28*(.5+.5*Math.sin(now/260+i*.07));color=color.map(v=>Math.round(v*pulse))}sctx.fillStyle=`rgb(${color[0]},${color[1]},${color[2]})`;sctx.fillRect(p.x,p.y,p.size,p.size);if(semHover===j.id){sctx.strokeStyle='#f1f5f8';sctx.lineWidth=1;sctx.strokeRect(p.x-.5,p.y-.5,p.size+1,p.size+1)}}}requestAnimationFrame(drawSemantic)}requestAnimationFrame(drawSemantic);
function hitSemantic(x,y){if(!semJobs.length)return null;const {size,gap,cols,x:ox,y:oy}=semLayout;if(x<ox||y<oy)return null;const col=Math.floor((x-ox)/(size+gap)),row=Math.floor((y-oy)/(size+gap));if(col<0||row<0||col>=cols)return null;const index=row*cols+col;if(index>=semJobs.length)return null;const p=tilePosition(index);if(x>p.x+p.size||y>p.y+p.size)return null;return semJobs[index]}
sc.addEventListener('pointermove',e=>{const r=sc.getBoundingClientRect(),j=hitSemantic(e.clientX-r.left,e.clientY-r.top);semHover=j?.id??null;if(!j){tip.style.display='none';return}const state=semState.get(j.id)??j.status??0;tip.innerHTML=`<strong>${esc(j.path||'Work unit '+j.id)}</strong>${statusName(state)} · ${fmt(j.sources)} source objects`;tip.style.display='block';const paneRect=pane.getBoundingClientRect();tip.style.left=Math.min(pane.clientWidth-tip.offsetWidth-10,e.clientX-paneRect.left+12)+'px';tip.style.top=Math.min(pane.clientHeight-tip.offsetHeight-10,e.clientY-paneRect.top+12)+'px'});
sc.addEventListener('pointerleave',()=>{semHover=null;tip.style.display='none'});
sc.addEventListener('click',async e=>{const r=sc.getBoundingClientRect(),j=hitSemantic(e.clientX-r.left,e.clientY-r.top);if(!j)return;try{const d=await api('/api/semantic/job?id='+j.id);renderSemanticInspector(d)}catch(err){notice(err.message)}});
function renderSemanticInspector(d){const state=statusName(d.status),nodes=(d.nodes||[]).map(n=>`<div class="sem-node"><div class="sem-node-kind">${esc(n.kind)}</div><div class="sem-node-label">${esc(n.label)}</div>${n.summary?`<div class="sem-node-summary">${esc(n.summary)}</div>`:''}</div>`).join('');const unresolved=(d.unresolved||[]).length?`<div class="section-title" style="margin-top:16px">Unresolved</div><div class="subtle">${d.unresolved.map(esc).join('<br>')}</div>`:'';document.querySelector('#inspector').innerHTML=`<h2>Semantic work unit ${d.id}</h2><div class="subtle">${esc(state)}</div><div class="path">${esc(d.path)}</div><div class="meta"><div class="k">Sources</div><div class="v">${fmt(d.sources)}</div><div class="k">Mode</div><div class="v">${esc(d.mode)}</div><div class="k">Attempts</div><div class="v">${fmt(d.attempts)}</div></div>${d.error?`<div class="value-text error">${esc(d.error)}</div>`:''}${nodes||'<div class="subtle" style="margin-top:14px">No semantic output committed yet.</div>'}${unresolved}`}
function setSemanticView(on){semView=on;pane.classList.toggle('show',on);semBtn.classList.toggle('active',on);if(on){semResize();pollSemantic(true)}else{tip.style.display='none'}}
semBtn.onclick=()=>setSemanticView(!semView);document.querySelector('#overviewBtn')?.addEventListener('click',()=>setSemanticView(false));
async function loadSemanticJobs(){try{semJobs=await api('/api/semantic/jobs');semResize()}catch{semJobs=[]}}
async function pollSemantic(force=false){try{const s=await api('/api/semantic/status');if(!s.available){empty.style.display='grid';progress.textContent='Not started';semBtn.classList.remove('live','running');if(force&&semView)semResize();return}empty.style.display='none';semBtn.classList.add('live');semBtn.classList.toggle('running',s.running>0);if(!semJobs.length||semJobs.length!==s.jobs)await loadSemanticJobs();for(const pair of s.states||[]){const id=pair[0],next=pair[1],prev=semState.has(id)?semState.get(id):(semJobs.find(x=>x.id===id)?.status??0);if(prev!==next)transitions.set(id,{from:palette[prev]||palette[0],to:palette[next]||palette[0],at:performance.now()});semState.set(id,next)}const pct=s.sources?100*s.mapped/s.sources:0;progress.textContent=`${pct.toFixed(2)}% · ${fmt(s.mapped)} / ${fmt(s.sources)} sources${s.running?` · ${s.running} active`:''}${s.failed?` · ${s.failed} failed`:''}`;if(semView)document.querySelector('#statusRight').textContent=`Semantic · ${pct.toFixed(2)}% mapped · ${fmt(s.done)} / ${fmt(s.jobs)} units`;if(s.complete)semBtn.classList.remove('running')}catch(e){if(force)notice(e.message)}}
setInterval(()=>pollSemantic(false),900);pollSemantic(false);
})();
</script>
)HTML";
    if (const auto pos = html.rfind("</body>"); pos != std::string::npos) html.insert(pos, script);
    return html;
}

response_t semantic_route(sqlite3* database, const std::filesystem::path& database_path, std::string_view target)
{
    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    const auto query = question == std::string_view::npos ? std::map<std::string, std::string>{} : parse_query(target.substr(question + 1U));
    if (path == "/" || path == "/index.html") return response_t{200, "text/html; charset=utf-8", enhanced_html()};
    if (path == "/api/semantic/status") return response_t{200, "application/json; charset=utf-8", api_semantic_status(database_path)};
    if (path == "/api/semantic/jobs") return response_t{200, "application/json; charset=utf-8", api_semantic_jobs(database_path)};
    if (path == "/api/semantic/job")
    {
        const auto found = query.find("id");
        if (found == query.end()) return response_t{400, "application/json; charset=utf-8", api_error("Missing semantic job id")};
        return response_t{200, "application/json; charset=utf-8", api_semantic_job(database_path, found->second)};
    }
    return nylon_base_route(database, database_path, target);
}

void semantic_serve_client(SOCKET client, sqlite3* database, const std::filesystem::path& database_path)
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

    response_t response = semantic_route(database, database_path, target);
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
               << L"semantic map: " << semantic_map_path(database_path) << L"\n"
               << L"url: " << url << L"\n"
               << L"Press Ctrl+C to stop the local viewer.\n";
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) std::wcout << L"Open this URL manually: " << url << L'\n';
    for (;;)
    {
        SOCKET client = accept(server.get(), nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        semantic_serve_client(client, database.get(), database_path);
    }
    server.reset();
    WSACleanup();
    return 0;
}
