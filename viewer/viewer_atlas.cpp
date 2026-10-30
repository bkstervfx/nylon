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
    if (sqlite3_prepare_v2(database, "SELECT id,status FROM lift_jobs ORDER BY id;", -1, &statement, nullptr) != SQLITE_OK)
        return api_error(sqlite3_errmsg(database));
    bool first = true;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        if (!first) out << ',';
        first = false;
        out << '[' << sqlite3_column_int64(statement, 0) << ',' << sqlite3_column_int(statement, 1) << ']';
    }
    sqlite3_finalize(statement);
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
    const char* sql = R"SQL(
SELECT j.id,j.path,j.source_count,j.estimated_bytes,j.mode,j.deterministic_kind,j.status,
       COALESCE(n.kind,''),COALESCE(n.label,'')
FROM lift_jobs j
LEFT JOIN semantic_nodes n ON n.job_id=j.id AND n.local_id='group'
ORDER BY j.id;
)SQL";
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
            << ",\"status\":" << sqlite3_column_int(statement, 6)
            << ",\"kind\":" << json_string(column_text(statement, 7))
            << ",\"label\":" << json_string(column_text(statement, 8)) << '}';
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
<style id="atlas-style">
:root{--bg:#090b0e;--panel:#0d1014;--raised:#12161b;--hover:#181d24;--border:#20262e;--border2:#303844}
.header{background:#0c0f13}.sidebar,.inspector{background:#0c0f13}.toolbar{background:#0c0f13e8;border-color:#242b34;box-shadow:none}.toolbar #physicsBtn{display:none}.canvas-wrap{background:#090b0e}
.atlas-pane,.progress-pane{position:absolute;inset:0;z-index:5;background:#090b0e;display:none;overflow:hidden}.atlas-pane.show,.progress-pane.show{display:block}.atlas-canvas,.progress-canvas{position:absolute!important;inset:0!important;width:100%!important;height:100%!important}.atlas-canvas{cursor:grab}.atlas-canvas.dragging{cursor:grabbing}.atlas-head,.progress-head{position:absolute;left:14px;right:14px;top:11px;height:32px;display:flex;align-items:center;justify-content:space-between;z-index:8;pointer-events:none}.atlas-title,.progress-title{font-size:11px;font-weight:600;letter-spacing:.04em;text-transform:uppercase;color:#dce3e9}.atlas-meta,.progress-meta{font-size:10px;color:var(--dim);font-variant-numeric:tabular-nums}.atlas-tools{position:absolute;left:14px;top:47px;z-index:9;display:flex;gap:5px}.atlas-tools button{height:25px;padding:0 9px;border:1px solid var(--border);border-radius:4px;background:#0d1014dd;color:var(--muted);font:inherit;font-size:10px;cursor:pointer}.atlas-tools button:hover{color:var(--text);border-color:var(--border2)}.atlas-tooltip,.progress-tooltip{position:absolute;z-index:12;display:none;max-width:430px;padding:7px 9px;background:#12161bf4;border:1px solid var(--border2);border-radius:5px;color:var(--muted);font-size:11px;pointer-events:none}.atlas-tooltip strong,.progress-tooltip strong{display:block;color:var(--text);font-weight:550;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.progress-canvas{inset:50px 12px 28px 12px!important;width:calc(100% - 24px)!important;height:calc(100% - 78px)!important;cursor:default}.progress-legend{position:absolute;left:16px;bottom:9px;display:flex;gap:13px;font-size:10px;color:var(--dim);z-index:8;pointer-events:none}.progress-legend i{width:7px;height:7px;display:inline-block;margin-right:5px;border-radius:1px}.empty-map{position:absolute;inset:0;display:none;place-items:center;text-align:center;color:var(--dim);font-size:12px;pointer-events:none}.empty-map b{display:block;color:var(--text);font-size:15px;margin-bottom:5px}.sem-node{margin-top:12px;padding-top:10px;border-top:1px solid var(--border)}.sem-node:first-of-type{margin-top:14px}.sem-node-kind{color:var(--dim);font-size:10px;text-transform:uppercase;letter-spacing:.08em}.sem-node-label{font-weight:600;margin-top:2px}.sem-node-summary{color:var(--muted);margin-top:4px;line-height:1.5}#atlasBtn.active,#semanticBtn.active{color:var(--text)}#semanticBtn.live::before{content:'';display:inline-block;width:6px;height:6px;border-radius:50%;background:#66d29a;margin-right:6px;vertical-align:1px}#semanticBtn.running::before{background:#819fff}
</style>
)HTML";
    const std::string old_button = "<button id=\"overviewBtn\">Overview</button>";
    const std::string new_buttons = "<button id=\"atlasBtn\">Atlas</button><button id=\"overviewBtn\">Raw</button><button id=\"semanticBtn\" title=\"Semantic lift progress\">Progress</button>";
    if (const auto pos = html.find(old_button); pos != std::string::npos) html.replace(pos, old_button.size(), new_buttons);
    const std::string panes = R"HTML(
<div class="atlas-pane" id="atlasPane"><div class="atlas-head"><strong class="atlas-title">Full image atlas</strong><span class="atlas-meta" id="atlasMeta">Loading map…</span></div><div class="atlas-tools"><button id="atlasFit">Fit all</button></div><canvas class="atlas-canvas" id="atlasCanvas"></canvas><div class="atlas-tooltip" id="atlasTooltip"></div><div class="empty-map" id="atlasEmpty"><div><b>No semantic map yet</b>Run semantic.cmd to build the image atlas.</div></div></div>
<div class="progress-pane" id="progressPane"><div class="progress-head"><strong class="progress-title">Semantic lift</strong><span class="progress-meta" id="progressMeta">Not started</span></div><canvas class="progress-canvas" id="progressCanvas"></canvas><div class="progress-tooltip" id="progressTooltip"></div><div class="empty-map" id="progressEmpty"><div><b>No semantic map yet</b>Run semantic.cmd to begin mapping this image.</div></div><div class="progress-legend"><span><i style="background:#20262e"></i>Pending</span><span><i style="background:#819fff"></i>Active</span><span><i style="background:#66d29a"></i>Mapped</span><span><i style="background:#de747b"></i>Failed</span></div></div>
)HTML";
    if (const auto pos = html.find("</main>"); pos != std::string::npos) html.insert(pos, panes);
    if (const auto pos = html.find("</head>"); pos != std::string::npos) html.insert(pos, css);

    const std::string script = R"HTML(
<script id="atlas-script">
(()=>{
'use strict';
const atlasBtn=document.querySelector('#atlasBtn'),rawBtn=document.querySelector('#overviewBtn'),progressBtn=document.querySelector('#semanticBtn');
const atlasPane=document.querySelector('#atlasPane'),atlasCanvas=document.querySelector('#atlasCanvas'),atlasTip=document.querySelector('#atlasTooltip'),atlasMeta=document.querySelector('#atlasMeta'),atlasEmpty=document.querySelector('#atlasEmpty');
const progressPane=document.querySelector('#progressPane'),progressCanvas=document.querySelector('#progressCanvas'),progressTip=document.querySelector('#progressTooltip'),progressMeta=document.querySelector('#progressMeta'),progressEmpty=document.querySelector('#progressEmpty');
if(!atlasBtn||!atlasPane||!atlasCanvas||!progressBtn||!progressCanvas)return;
const actx=atlasCanvas.getContext('2d'),pctx=progressCanvas.getContext('2d');
const WORLD={w:1800,h:1100};
let jobs=[],states=new Map(),groups=[],current='atlas',atlasDpr=1,progressDpr=1,scale=1,tx=0,ty=0,dragAtlas=null,hoverJob=null,progressLayout={size:4,gap:1,cols:1,x:0,y:0};
const statusName=s=>s===1?'Active':s===2?'Mapped · deterministic':s===3?'Mapped · local model':s===4?'Failed':'Pending';
const statusColors={0:'#20262e',1:'#819fff',4:'#de747b'};
const kindColors={service:'#75a7ff',driver:'#8fb7ff',application:'#b39cff',component:'#a59be8',com_class:'#77cdb6',security_policy:'#e5b96f',network_configuration:'#6ec7cf',device:'#7fb8dd',startup_entry:'#d9a66b',file_association:'#9ba9d9',environment_setting:'#91c9a8',registry_configuration:'#748398',configuration_group:'#748398',unknown_configuration:'#5b6571'};
const pathParts=p=>String(p||'').split('\\').filter(Boolean),rootName=p=>pathParts(p)[0]||'Registry',leafName=p=>{const a=pathParts(p);return a[a.length-1]||'Registry'};
function colorFor(j){const st=states.get(j.id)??j.status??0;if(st===0||st===1||st===4)return statusColors[st];return kindColors[j.kind||j.known]||'#69b896'}
function buildLayout(){
  const byRoot=new Map();for(const j of jobs){const root=rootName(j.path);if(!byRoot.has(root))byRoot.set(root,[]);byRoot.get(root).push(j)}
  const entries=[...byRoot.entries()].sort((a,b)=>b[1].reduce((s,j)=>s+j.sources,0)-a[1].reduce((s,j)=>s+j.sources,0)||a[0].localeCompare(b[0]));
  const cols=Math.max(1,Math.ceil(Math.sqrt(entries.length*1.55))),rows=Math.max(1,Math.ceil(entries.length/cols)),gutter=22,cw=(WORLD.w-gutter*(cols+1))/cols,ch=(WORLD.h-gutter*(rows+1))/rows;groups=[];
  entries.forEach(([name,list],gi)=>{list.sort((a,b)=>String(a.path).localeCompare(String(b.path))||a.id-b.id);const gx=gutter+(gi%cols)*(cw+gutter),gy=gutter+Math.floor(gi/cols)*(ch+gutter),ix=gx+12,iy=gy+34,iw=cw-24,ih=ch-46,ratio=Math.max(.2,iw/Math.max(1,ih)),jc=Math.max(1,Math.ceil(Math.sqrt(list.length*ratio))),jr=Math.max(1,Math.ceil(list.length/jc)),pitch=Math.max(1,Math.min(iw/jc,ih/jr)),tile=Math.max(.9,Math.min(8,pitch*.72));list.forEach((j,i)=>{j._x=ix+(i%jc)*pitch+pitch*.5;j._y=iy+Math.floor(i/jc)*pitch+pitch*.5;j._tile=tile;j._group=gi});groups.push({name,jobs:list,x:gx,y:gy,w:cw,h:ch,sources:list.reduce((s,j)=>s+j.sources,0)})});
}
function resize(c){const r=c.getBoundingClientRect(),d=Math.max(1,Math.min(2,devicePixelRatio||1));c.width=Math.max(1,Math.floor(r.width*d));c.height=Math.max(1,Math.floor(r.height*d));return{r,d}}
function fitAtlas(){const r=atlasCanvas.getBoundingClientRect();scale=Math.min(r.width/WORLD.w,r.height/WORLD.h)*.95;tx=(r.width-WORLD.w*scale)/2;ty=(r.height-WORLD.h*scale)/2;drawAtlas()}
const screen=(x,y)=>({x:x*scale+tx,y:y*scale+ty}),world=(x,y)=>({x:(x-tx)/scale,y:(y-ty)/scale});
function drawAtlas(){if(current!=='atlas')return;const z=resize(atlasCanvas),r=z.r;atlasDpr=z.d;actx.setTransform(atlasDpr,0,0,atlasDpr,0,0);actx.clearRect(0,0,r.width,r.height);actx.fillStyle='#090b0e';actx.fillRect(0,0,r.width,r.height);for(const g of groups){const a=screen(g.x,g.y),b=screen(g.x+g.w,g.y+g.h);if(b.x<0||b.y<0||a.x>r.width||a.y>r.height)continue;actx.strokeStyle='#20262e';actx.lineWidth=1;actx.strokeRect(a.x,a.y,b.x-a.x,b.y-a.y);if(scale>.32){actx.fillStyle='#dbe3ea';actx.font='600 11px Segoe UI';actx.fillText(g.name,a.x+8,a.y+17);actx.fillStyle='#65717d';actx.font='9px Segoe UI';actx.fillText(`${fmt(g.jobs.length)} regions · ${fmt(g.sources)} sources`,a.x+8,a.y+29)}for(const j of g.jobs){const p=screen(j._x,j._y),sz=Math.max(1,Math.min(13,j._tile*scale));if(p.x+sz<0||p.y+sz<0||p.x-sz>r.width||p.y-sz>r.height)continue;actx.fillStyle=colorFor(j);actx.fillRect(p.x-sz*.5,p.y-sz*.5,sz,sz);if(hoverJob===j){actx.strokeStyle='#f3f6f8';actx.strokeRect(p.x-sz*.5-1,p.y-sz*.5-1,sz+2,sz+2)}if(scale>4&&sz>6){actx.fillStyle='#9faab5';actx.font='9px Segoe UI';actx.fillText((j.label||leafName(j.path)).slice(0,34),p.x+sz,p.y+3)}}}}
function atlasHit(cx,cy){const r=atlasCanvas.getBoundingClientRect(),p=world(cx-r.left,cy-r.top);for(const g of groups){if(p.x<g.x||p.x>g.x+g.w||p.y<g.y||p.y>g.y+g.h)continue;let best=null,bd=Infinity;for(const j of g.jobs){const dx=j._x-p.x,dy=j._y-p.y,d=dx*dx+dy*dy;if(d<bd){bd=d;best=j}}const rad=Math.max(3,(best?best._tile:1)*1.8/Math.max(scale,.2));return best&&bd<=rad*rad?best:null}return null}
function showTip(tip,pane,j,e){if(!j){tip.style.display='none';return}const st=states.get(j.id)??j.status??0;tip.innerHTML=`<strong>${esc(j.label||j.path||('Work unit '+j.id))}</strong>${esc(j.kind||j.known||'registry region')} · ${statusName(st)} · ${fmt(j.sources)} source objects`;tip.style.display='block';const pr=pane.getBoundingClientRect();tip.style.left=Math.min(pane.clientWidth-tip.offsetWidth-10,e.clientX-pr.left+12)+'px';tip.style.top=Math.min(pane.clientHeight-tip.offsetHeight-10,e.clientY-pr.top+12)+'px'}
atlasCanvas.addEventListener('pointerdown',e=>{dragAtlas={x:e.clientX,y:e.clientY,tx,ty,moved:false};atlasCanvas.setPointerCapture(e.pointerId);atlasCanvas.classList.add('dragging')});
atlasCanvas.addEventListener('pointermove',e=>{if(dragAtlas){const dx=e.clientX-dragAtlas.x,dy=e.clientY-dragAtlas.y;if(Math.abs(dx)+Math.abs(dy)>2)dragAtlas.moved=true;tx=dragAtlas.tx+dx;ty=dragAtlas.ty+dy;drawAtlas();return}hoverJob=atlasHit(e.clientX,e.clientY);showTip(atlasTip,atlasPane,hoverJob,e);drawAtlas()});
atlasCanvas.addEventListener('pointerup',async e=>{const d=dragAtlas;dragAtlas=null;atlasCanvas.classList.remove('dragging');try{atlasCanvas.releasePointerCapture(e.pointerId)}catch{}if(d&&!d.moved){const j=atlasHit(e.clientX,e.clientY);if(j)await inspectJob(j)}});
atlasCanvas.addEventListener('pointerleave',()=>{if(!dragAtlas){hoverJob=null;atlasTip.style.display='none';drawAtlas()}});
atlasCanvas.addEventListener('wheel',e=>{e.preventDefault();const r=atlasCanvas.getBoundingClientRect(),mx=e.clientX-r.left,my=e.clientY-r.top,before=world(mx,my),next=Math.max(.18,Math.min(9,scale*Math.exp(-e.deltaY*.0012)));scale=next;tx=mx-before.x*scale;ty=my-before.y*scale;drawAtlas()},{passive:false});
atlasCanvas.addEventListener('dblclick',e=>{const j=atlasHit(e.clientX,e.clientY);if(!j)return;const r=atlasCanvas.getBoundingClientRect();scale=Math.max(scale,4.5);tx=r.width/2-j._x*scale;ty=r.height/2-j._y*scale;drawAtlas()});
document.querySelector('#atlasFit').onclick=fitAtlas;
function progressResize(){const z=resize(progressCanvas),r=z.r;progressDpr=z.d;if(!jobs.length)return;const n=jobs.length,ratio=Math.max(.25,r.width/Math.max(1,r.height)),cols=Math.max(1,Math.ceil(Math.sqrt(n*ratio))),rows=Math.ceil(n/cols),gap=n>7000?1:2,size=Math.max(2,Math.min(16,Math.floor(Math.min((r.width-gap*(cols-1))/cols,(r.height-gap*(rows-1))/rows))));const width=cols*size+(cols-1)*gap,height=rows*size+(rows-1)*gap;progressLayout={size,gap,cols,x:Math.floor((r.width-width)/2),y:Math.max(0,Math.floor((r.height-height)/2))}}
function progressPos(i){const l=progressLayout;return{x:l.x+(i%l.cols)*(l.size+l.gap),y:l.y+Math.floor(i/l.cols)*(l.size+l.gap),size:l.size}}
function drawProgress(){if(current!=='progress')return;progressResize();const r=progressCanvas.getBoundingClientRect();pctx.setTransform(progressDpr,0,0,progressDpr,0,0);pctx.clearRect(0,0,r.width,r.height);for(let i=0;i<jobs.length;i++){const j=jobs[i],p=progressPos(i),st=states.get(j.id)??j.status??0;pctx.fillStyle=st===2||st===3?'#66d29a':statusColors[st]||statusColors[0];pctx.fillRect(p.x,p.y,p.size,p.size)}}
function progressHit(x,y){const l=progressLayout;if(x<l.x||y<l.y)return null;const col=Math.floor((x-l.x)/(l.size+l.gap)),row=Math.floor((y-l.y)/(l.size+l.gap)),i=row*l.cols+col;if(col<0||row<0||col>=l.cols||i<0||i>=jobs.length)return null;const p=progressPos(i);return x<=p.x+p.size&&y<=p.y+p.size?jobs[i]:null}
progressCanvas.addEventListener('pointermove',e=>{const r=progressCanvas.getBoundingClientRect(),j=progressHit(e.clientX-r.left,e.clientY-r.top);showTip(progressTip,progressPane,j,e)});progressCanvas.addEventListener('pointerleave',()=>progressTip.style.display='none');progressCanvas.addEventListener('click',async e=>{const r=progressCanvas.getBoundingClientRect(),j=progressHit(e.clientX-r.left,e.clientY-r.top);if(j)await inspectJob(j)});
async function inspectJob(j){try{const d=await api('/api/semantic/job?id='+j.id),state=statusName(d.status),nodes=(d.nodes||[]).map(n=>`<div class="sem-node"><div class="sem-node-kind">${esc(n.kind)}</div><div class="sem-node-label">${esc(n.label)}</div>${n.summary?`<div class="sem-node-summary">${esc(n.summary)}</div>`:''}</div>`).join(''),unresolved=(d.unresolved||[]).length?`<div class="section-title" style="margin-top:16px">Unresolved</div><div class="subtle">${d.unresolved.map(esc).join('<br>')}</div>`:'';document.querySelector('#inspector').innerHTML=`<h2>${esc(d.nodes?.[0]?.label||leafName(d.path))}</h2><div class="subtle">${esc(state)}</div><div class="path">${esc(d.path)}</div><div class="meta"><div class="k">Sources</div><div class="v">${fmt(d.sources)}</div><div class="k">Mode</div><div class="v">${esc(d.mode)}</div><div class="k">Attempts</div><div class="v">${fmt(d.attempts)}</div></div>${d.error?`<div class="value-text error">${esc(d.error)}</div>`:''}${nodes||'<div class="subtle" style="margin-top:14px">No semantic output committed yet.</div>'}${unresolved}`}catch(err){notice(err.message)}}
function setView(v){current=v;atlasPane.classList.toggle('show',v==='atlas');progressPane.classList.toggle('show',v==='progress');atlasBtn.classList.toggle('active',v==='atlas');progressBtn.classList.toggle('active',v==='progress');if(v==='atlas'){atlasTip.style.display='none';drawAtlas()}else if(v==='progress'){progressTip.style.display='none';drawProgress()}else{wake()}updateModeStatus()}
function updateModeStatus(){if(current==='atlas')document.querySelector('#statusRight').textContent=`Atlas · ${fmt(jobs.length)} regions · full map`;else if(current==='progress')document.querySelector('#statusRight').textContent=`Progress · ${fmt(jobs.length)} work units`;}
atlasBtn.onclick=()=>setView('atlas');rawBtn.addEventListener('click',()=>setView('raw'));progressBtn.onclick=()=>setView('progress');
async function loadJobs(){jobs=await api('/api/semantic/jobs');buildLayout();if(!jobs.length)return;atlasEmpty.style.display='none';progressEmpty.style.display='none';fitAtlas();drawProgress()}
async function poll(){try{const s=await api('/api/semantic/status');if(!s.available){atlasEmpty.style.display='grid';progressEmpty.style.display='grid';atlasMeta.textContent='Semantic map not started';progressMeta.textContent='Not started';return}if(!jobs.length||jobs.length!==s.jobs)await loadJobs();let changed=false;for(const pair of s.states||[]){if(states.get(pair[0])!==pair[1])changed=true;states.set(pair[0],pair[1])}const pct=s.sources?100*s.mapped/s.sources:0;atlasMeta.textContent=`${fmt(s.jobs)} regions · ${fmt(s.sources)} source objects · ${pct.toFixed(1)}% mapped`;progressMeta.textContent=`${pct.toFixed(2)}% · ${fmt(s.mapped)} / ${fmt(s.sources)} sources${s.running?` · ${s.running} active`:''}${s.failed?` · ${s.failed} failed`:''}`;progressBtn.classList.add('live');progressBtn.classList.toggle('running',s.running>0);if(changed){if(current==='atlas')drawAtlas();if(current==='progress')drawProgress()}updateModeStatus()}catch(e){notice(e.message)}}
new ResizeObserver(()=>{if(current==='atlas')drawAtlas();if(current==='progress')drawProgress()}).observe(document.querySelector('#canvasWrap'));
/* Raw graph: damped spring system with an explicit sleep state. It wakes on drag/load, settles, then becomes completely still. */
let rawSteps=0,rawWakeUntil=0;wake=function(){frame=0;rawSteps=0;rawWakeUntil=performance.now()+900};
simulate=function(){if(!physics||!graph.nodes.length){frame=10000;return}const nodes=graph.nodes,N=nodes.length,cx=wrap.clientWidth/2,cy=wrap.clientHeight/2;rawSteps++;for(const n of nodes){if(n.pinned||drag===n.id)continue;n.vx+=(cx-n.x)*0.00055;n.vy+=(cy-n.y)*0.00055}
for(const e of graph.edges){const a=graph.map.get(e.source),b=graph.map.get(e.target);if(!a||!b)continue;let dx=b.x-a.x,dy=b.y-a.y,d=Math.hypot(dx,dy)||1;const rest=e.kind==='owns'?58:92,f=Math.max(-1.8,Math.min(1.8,(d-rest)*0.0038)),fx=dx/d*f,fy=dy/d*f;if(!a.pinned&&drag!==a.id){a.vx+=fx;a.vy+=fy}if(!b.pinned&&drag!==b.id){b.vx-=fx;b.vy-=fy}}
const cell=82,grid=new Map();for(let i=0;i<N;i++){const n=nodes[i],k=`${Math.floor(n.x/cell)},${Math.floor(n.y/cell)}`;if(!grid.has(k))grid.set(k,[]);grid.get(k).push(i)}for(let i=0;i<N;i++){const a=nodes[i];if(a.pinned||drag===a.id)continue;const gx=Math.floor(a.x/cell),gy=Math.floor(a.y/cell);for(let ox=-1;ox<=1;ox++)for(let oy=-1;oy<=1;oy++){const bucket=grid.get(`${gx+ox},${gy+oy}`);if(!bucket)continue;for(const j of bucket){if(j<=i)continue;const b=nodes[j];let dx=b.x-a.x,dy=b.y-a.y,d2=dx*dx+dy*dy;if(d2<.01){dx=.1;dy=.1;d2=.02}if(d2>7200)continue;const d=Math.sqrt(d2),force=Math.min(.9,90/(d2+18)),fx=dx/d*force,fy=dy/d*force;if(!a.pinned&&drag!==a.id){a.vx-=fx;a.vy-=fy}if(!b.pinned&&drag!==b.id){b.vx+=fx;b.vy+=fy}}}}
let energy=0;for(const n of nodes){if(n.pinned||drag===n.id){n.vx=n.vy=0;continue}n.vx*=.76;n.vy*=.76;const speed=Math.hypot(n.vx,n.vy),limit=3.2;if(speed>limit){n.vx*=limit/speed;n.vy*=limit/speed}n.x+=n.vx;n.y+=n.vy;energy+=n.vx*n.vx+n.vy*n.vy}frame++;const avg=energy/Math.max(1,N);if(!drag&&rawSteps>35&&avg<.0015&&performance.now()>rawWakeUntil){for(const n of nodes)n.vx=n.vy=0;frame=10000}}
const baseRender=render;render=function(){if(current==='raw')baseRender();else requestAnimationFrame(render)};
physics=true;document.querySelector('#physicsBtn')?.classList.add('active');
setInterval(poll,900);poll().then(()=>setView('atlas'));
})();
</script>
)HTML";
    if (const auto pos = html.rfind("</body>"); pos != std::string::npos) html.insert(pos, script);
    return html;
}

response_t atlas_route(sqlite3* database, const std::filesystem::path& database_path, std::string_view target)
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

void atlas_serve_client(SOCKET client, sqlite3* database, const std::filesystem::path& database_path)
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
    response_t response = atlas_route(database, database_path, target);
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
    if (server.get() == INVALID_SOCKET){WSACleanup();return 1;}
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
    if (bind(server.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server.get(), 16) == SOCKET_ERROR){WSACleanup();return 1;}
    int address_length=sizeof(address);if(getsockname(server.get(),reinterpret_cast<sockaddr*>(&address),&address_length)==SOCKET_ERROR){WSACleanup();return 1;}
    const unsigned short port=ntohs(address.sin_port);const std::wstring url=L"http://127.0.0.1:"+std::to_wstring(port)+L"/";
    std::wcout << L"Nylon Viewer\n" << L"database: " << database_path << L"\n" << L"semantic map: " << semantic_map_path(database_path) << L"\n" << L"url: " << url << L"\n" << L"Press Ctrl+C to stop the local viewer.\n";
    const HINSTANCE opened=ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(opened)<=32)std::wcout<<L"Open this URL manually: "<<url<<L'\n';
    for(;;){SOCKET client=accept(server.get(),nullptr,nullptr);if(client==INVALID_SOCKET)break;atlas_serve_client(client,database.get(),database_path);}server.reset();WSACleanup();return 0;
}
