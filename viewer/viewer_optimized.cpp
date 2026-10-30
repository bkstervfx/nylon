#define semantic_route nylon_semantic_route
#define semantic_serve_client nylon_semantic_serve_client
#define wmain nylon_semantic_wmain
#include "viewer_semantic.cpp"
#undef wmain
#undef semantic_serve_client
#undef semantic_route

namespace
{

void replace_required(std::string& text, std::string_view before, std::string_view after)
{
    const std::size_t position = text.find(before);
    if (position == std::string::npos)
        throw std::runtime_error("Viewer semantic optimization patch no longer matches embedded UI");
    text.replace(position, before.size(), after);
}

std::string optimized_html()
{
    std::string html = enhanced_html();

    replace_required(
        html,
        "const sctx=sc.getContext('2d');let semJobs=[],semState=new Map(),semView=false,semDpr=1,semHover=null,semLayout={size:0,gap:2,cols:1,x:0,y:0};const transitions=new Map();",
        "const sctx=sc.getContext('2d');let semJobs=[],semJobById=new Map(),semState=new Map(),semView=false,semDpr=1,semHover=null,semLayout={size:0,gap:2,cols:1,x:0,y:0};const transitions=new Map();let semDrawPending=false,semAnimationTimer=0,semBasePhysics=null;");

    replace_required(
        html,
        "function mix(a,b,t){return `rgb(${Math.round(a[0]+(b[0]-a[0])*t)},${Math.round(a[1]+(b[1]-a[1])*t)},${Math.round(a[2]+(b[2]-a[2])*t)})`}\n",
        "");

    replace_required(
        html,
        "function semResize(){const r=sc.getBoundingClientRect();semDpr=Math.max(1,Math.min(2,devicePixelRatio||1));sc.width=Math.max(1,Math.floor(r.width*semDpr));sc.height=Math.max(1,Math.floor(r.height*semDpr));if(!semJobs.length)return;const n=semJobs.length,ratio=Math.max(.25,r.width/Math.max(1,r.height)),cols=Math.max(1,Math.ceil(Math.sqrt(n*ratio))),rows=Math.ceil(n/cols);let gap=n>6000?1:2,size=Math.floor(Math.min((r.width-gap*(cols-1))/cols,(r.height-gap*(rows-1))/rows));size=Math.max(2,Math.min(16,size));const width=cols*size+(cols-1)*gap,height=rows*size+(rows-1)*gap;semLayout={size,gap,cols,x:Math.floor((r.width-width)/2),y:Math.max(0,Math.floor((r.height-height)/2))}}\nnew ResizeObserver(semResize).observe(sc);",
        "function scheduleSemanticDraw(){if(!semView||semDrawPending)return;semDrawPending=true;requestAnimationFrame(drawSemantic)}\nfunction scheduleSemanticAnimation(){if(!semView||semAnimationTimer)return;semAnimationTimer=setTimeout(()=>{semAnimationTimer=0;scheduleSemanticDraw()},66)}\nfunction semResize(){const r=sc.getBoundingClientRect();semDpr=Math.max(1,Math.min(2,devicePixelRatio||1));sc.width=Math.max(1,Math.floor(r.width*semDpr));sc.height=Math.max(1,Math.floor(r.height*semDpr));if(!semJobs.length){scheduleSemanticDraw();return}const n=semJobs.length,ratio=Math.max(.25,r.width/Math.max(1,r.height)),cols=Math.max(1,Math.ceil(Math.sqrt(n*ratio))),rows=Math.ceil(n/cols);let gap=n>6000?1:2,size=Math.floor(Math.min((r.width-gap*(cols-1))/cols,(r.height-gap*(rows-1))/rows));size=Math.max(2,Math.min(16,size));const width=cols*size+(cols-1)*gap,height=rows*size+(rows-1)*gap;semLayout={size,gap,cols,x:Math.floor((r.width-width)/2),y:Math.max(0,Math.floor((r.height-height)/2))};scheduleSemanticDraw()}\nnew ResizeObserver(semResize).observe(sc);");

    replace_required(
        html,
        "function drawSemantic(now){if(semView){const r=sc.getBoundingClientRect();sctx.setTransform(semDpr,0,0,semDpr,0,0);sctx.clearRect(0,0,r.width,r.height);for(let i=0;i<semJobs.length;i++){const j=semJobs[i],p=tilePosition(i),state=semState.get(j.id)??j.status??0,tr=transitions.get(j.id);let color=palette[state]||palette[0];if(tr){const t=Math.min(1,(now-tr.at)/280);color=[Math.round(tr.from[0]+(tr.to[0]-tr.from[0])*t),Math.round(tr.from[1]+(tr.to[1]-tr.from[1])*t),Math.round(tr.from[2]+(tr.to[2]-tr.from[2])*t)];if(t>=1)transitions.delete(j.id)}if(state===1){const pulse=.72+.28*(.5+.5*Math.sin(now/260+i*.07));color=color.map(v=>Math.round(v*pulse))}sctx.fillStyle=`rgb(${color[0]},${color[1]},${color[2]})`;sctx.fillRect(p.x,p.y,p.size,p.size);if(semHover===j.id){sctx.strokeStyle='#f1f5f8';sctx.lineWidth=1;sctx.strokeRect(p.x-.5,p.y-.5,p.size+1,p.size+1)}}}requestAnimationFrame(drawSemantic)}requestAnimationFrame(drawSemantic);",
        "function drawSemantic(now){semDrawPending=false;if(!semView)return;const r=sc.getBoundingClientRect();sctx.setTransform(semDpr,0,0,semDpr,0,0);sctx.clearRect(0,0,r.width,r.height);let animate=false;for(let i=0;i<semJobs.length;i++){const j=semJobs[i],p=tilePosition(i),state=semState.get(j.id)??j.status??0,tr=transitions.get(j.id);let color=palette[state]||palette[0];if(tr){const t=Math.min(1,(now-tr.at)/280);color=[Math.round(tr.from[0]+(tr.to[0]-tr.from[0])*t),Math.round(tr.from[1]+(tr.to[1]-tr.from[1])*t),Math.round(tr.from[2]+(tr.to[2]-tr.from[2])*t)];if(t>=1)transitions.delete(j.id);else animate=true}if(state===1){const pulse=.78+.22*(.5+.5*Math.sin(now/310+i*.07));color=color.map(v=>Math.round(v*pulse));animate=true}sctx.fillStyle=`rgb(${color[0]},${color[1]},${color[2]})`;sctx.fillRect(p.x,p.y,p.size,p.size);if(semHover===j.id){sctx.strokeStyle='#f1f5f8';sctx.lineWidth=1;sctx.strokeRect(p.x-.5,p.y-.5,p.size+1,p.size+1)}}if(animate)scheduleSemanticAnimation()}"
    );

    replace_required(
        html,
        "sc.addEventListener('pointermove',e=>{const r=sc.getBoundingClientRect(),j=hitSemantic(e.clientX-r.left,e.clientY-r.top);semHover=j?.id??null;if(!j){tip.style.display='none';return}const state=semState.get(j.id)??j.status??0;tip.innerHTML=`<strong>${esc(j.path||'Work unit '+j.id)}</strong>${statusName(state)} · ${fmt(j.sources)} source objects`;tip.style.display='block';const paneRect=pane.getBoundingClientRect();tip.style.left=Math.min(pane.clientWidth-tip.offsetWidth-10,e.clientX-paneRect.left+12)+'px';tip.style.top=Math.min(pane.clientHeight-tip.offsetHeight-10,e.clientY-paneRect.top+12)+'px'});\nsc.addEventListener('pointerleave',()=>{semHover=null;tip.style.display='none'});",
        "sc.addEventListener('pointermove',e=>{const r=sc.getBoundingClientRect(),j=hitSemantic(e.clientX-r.left,e.clientY-r.top),next=j?.id??null;if(next!==semHover){semHover=next;scheduleSemanticDraw()}if(!j){tip.style.display='none';return}const state=semState.get(j.id)??j.status??0;tip.innerHTML=`<strong>${esc(j.path||'Work unit '+j.id)}</strong>${statusName(state)} · ${fmt(j.sources)} source objects`;tip.style.display='block';const paneRect=pane.getBoundingClientRect();tip.style.left=Math.min(pane.clientWidth-tip.offsetWidth-10,e.clientX-paneRect.left+12)+'px';tip.style.top=Math.min(pane.clientHeight-tip.offsetHeight-10,e.clientY-paneRect.top+12)+'px'});\nsc.addEventListener('pointerleave',()=>{if(semHover!==null){semHover=null;scheduleSemanticDraw()}tip.style.display='none'});"
    );

    replace_required(
        html,
        "function setSemanticView(on){semView=on;pane.classList.toggle('show',on);semBtn.classList.toggle('active',on);if(on){semResize();pollSemantic(true)}else{tip.style.display='none'}}",
        "function setSemanticView(on){semView=on;pane.classList.toggle('show',on);semBtn.classList.toggle('active',on);if(on){if(semBasePhysics===null&&typeof physics!=='undefined'){semBasePhysics=physics;physics=false}semResize();scheduleSemanticDraw();pollSemantic(true)}else{tip.style.display='none';if(semAnimationTimer){clearTimeout(semAnimationTimer);semAnimationTimer=0}if(semBasePhysics!==null&&typeof physics!=='undefined'){physics=semBasePhysics;semBasePhysics=null;if(typeof wake==='function')wake()}}}"
    );

    replace_required(
        html,
        "async function loadSemanticJobs(){try{semJobs=await api('/api/semantic/jobs');semResize()}catch{semJobs=[]}}",
        "async function loadSemanticJobs(){try{semJobs=await api('/api/semantic/jobs');semJobById=new Map(semJobs.map(j=>[j.id,j]));semState=new Map(semJobs.map(j=>[j.id,j.status??0]));semResize();scheduleSemanticDraw()}catch{semJobs=[];semJobById=new Map();semState=new Map();scheduleSemanticDraw()}}"
    );

    replace_required(
        html,
        "for(const pair of s.states||[]){const id=pair[0],next=pair[1],prev=semState.has(id)?semState.get(id):(semJobs.find(x=>x.id===id)?.status??0);if(prev!==next)transitions.set(id,{from:palette[prev]||palette[0],to:palette[next]||palette[0],at:performance.now()});semState.set(id,next)}",
        "let stateChanged=false;for(const pair of s.states||[]){const id=pair[0],next=pair[1],prev=semState.has(id)?semState.get(id):(semJobById.get(id)?.status??0);if(prev!==next){transitions.set(id,{from:palette[prev]||palette[0],to:palette[next]||palette[0],at:performance.now()});stateChanged=true}semState.set(id,next)}if(stateChanged||s.running>0)scheduleSemanticDraw()"
    );

    return html;
}

response_t optimized_route(sqlite3* database, const std::filesystem::path& database_path, std::string_view target)
{
    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    if (path == "/" || path == "/index.html")
    {
        try
        {
            return response_t{200, "text/html; charset=utf-8", optimized_html()};
        }
        catch (const std::exception& error)
        {
            return response_t{500, "text/plain; charset=utf-8", std::string("Viewer UI composition failed: ") + error.what()};
        }
    }
    return nylon_semantic_route(database, database_path, target);
}

void optimized_serve_client(SOCKET client, sqlite3* database, const std::filesystem::path& database_path)
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

    response_t response = optimized_route(database, database_path, target);
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
        optimized_serve_client(client, database.get(), database_path);
    }
    server.reset();
    WSACleanup();
    return 0;
}
