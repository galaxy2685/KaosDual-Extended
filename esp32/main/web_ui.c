/*
 * web_ui.c — KAOS Portal HTTP server
 *
 * Endpoints:
 *   GET  /                  Full web app (single page)
 *   GET  /api/state         JSON: file list, slot states, portal type
 *   POST /api/load          {slot, file} — load a file into a slot
 *   POST /api/unload        {slot} — unload a slot
 *   POST /api/sense         Re-announce portal to game
 *   POST /api/upload        multipart/form-data file upload to SPIFFS
 *   GET  /api/download?slot=N  Download current slot data as .bin
 */

#include "web_ui.h"
#include "Skylander.h"
#include "library.h"
#include "sky_editor.h"
#include "skylander_ids.h"
#include "pico_bridge.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <ctype.h>

static const char *TAG = "WebUI";

#define SPIFFS_MOUNT "/spiffs"

/* Export diagnostics.  CRC32 is calculated over the binary body only; it
 * does not include HTTP headers or any text representation. */
static uint32_t dump_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return ~crc;
}

static void log_download_dump(const char *stage, const uint8_t *data) {
    ESP_LOGI(TAG, "%s: bytes=%u crc32=%08" PRIX32, stage,
             SKYLANDER_DUMP_SIZE, dump_crc32(data, SKYLANDER_DUMP_SIZE));
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, 16, ESP_LOG_INFO);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data + SKYLANDER_DUMP_SIZE - 16, 16,
                             ESP_LOG_INFO);
}

extern SemaphoreHandle_t g_sky_mutex;
extern int  g_file_count;
extern char g_file_list[64][64];
extern void spiffs_full_path(const char *basename, char *out, size_t out_len);
extern void scan_files(void);


/* -----------------------------------------------------------------------
 * HTML page — complete rewrite with clean JS architecture
 * State machine approach: fetch state → diff → update only what changed
 * No innerHTML rebuilding of interactive elements
 * ----------------------------------------------------------------------- */
static const char HTML_PAGE[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Kaos Dual Extended</title>"
"<style>"
"@import url('https://fonts.googleapis.com/css2?family=Orbitron:wght@700&family=Exo+2:wght@400;600&display=swap');"
":root{"
  "--bg:#060612;--surface:#0e0e24;--card:#13132e;"
  "--border:#1e1e45;--accent:#6d28d9;--glow:#7c3aed;"
  "--accent2:#a855f7;--hot:#f43f5e;--ok:#10b981;"
  "--text:#e2e8f0;--muted:#475569;--bright:#f8fafc;"
"}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:var(--bg);color:var(--text);"
  "font-family:'Exo 2',system-ui,sans-serif;"
  "min-height:100vh;padding:20px 16px;"
  "background-image:radial-gradient(ellipse at 20% 0%,#1a0a3e 0%,transparent 60%),"
    "radial-gradient(ellipse at 80% 100%,#0a1a3e 0%,transparent 60%)}"

/* Header */
"header{text-align:center;margin-bottom:24px}"
"h1{font-family:'Orbitron',monospace;font-size:2rem;letter-spacing:.1em;"
  "background:linear-gradient(135deg,#a78bfa,#7c3aed,#4f46e5);"
  "-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;"
  "margin-bottom:4px}"
".sub{color:var(--muted);font-size:.8rem;letter-spacing:.05em}"

/* Container */
".wrap{max-width:680px;margin:0 auto}"

/* Portal type bar */
".pbar{background:var(--card);border:1px solid var(--border);"
  "border-radius:14px;padding:14px 16px;margin-bottom:16px;"
  "display:flex;align-items:center;gap:12px}"
".pbar-label{font-size:.75rem;color:var(--muted);white-space:nowrap;font-weight:600;letter-spacing:.05em;text-transform:uppercase}"
"#ptype{flex:1;background:#0a0a20;border:1px solid var(--border);"
  "border-radius:8px;color:var(--text);padding:8px 12px;"
  "font-size:.87rem;font-family:'Exo 2',sans-serif;cursor:pointer;outline:none}"
"#ptype:focus{border-color:var(--accent)}"
".pbadge{background:var(--accent);color:#fff;font-size:.7rem;"
  "padding:4px 10px;border-radius:20px;font-weight:600;white-space:nowrap}"

/* Slot grid */
".slots{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px;margin-bottom:16px}"
"@media(max-width:760px){.slots{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:500px){.slots{grid-template-columns:1fr}}"
".card{background:var(--card);border:1px solid var(--border);"
  "border-radius:14px;padding:16px;display:flex;flex-direction:column;gap:10px;"
  "transition:border-color .3s}"
".card.loaded{border-color:var(--accent);"
"box-shadow:0 0 20px rgba(109,40,217,.15)}"
".library{margin-top:22px;background:var(--card);border:1px solid var(--border);border-radius:14px;padding:16px}"
".libbar{display:grid;grid-template-columns:2fr 1fr;gap:8px;margin:10px 0}.libbar input{min-width:0;background:#0a0a20;border:1px solid var(--border);border-radius:7px;padding:10px;color:var(--text)}"
".libtools{display:flex;gap:8px;margin:8px 0;flex-wrap:wrap}.libtools .filter-btn{flex:1}.fav-btn{background:transparent;border:1px solid #eab308;color:#eab308}.fav-btn.on{background:#eab308;color:#18181b}.btn-libdel{background:#7f1d1d;color:#fff}.user-upload[hidden]{display:none}.user-upload{margin:10px 0;padding:10px;border:1px dashed var(--border);border-radius:8px}"
".libnav-title{font-size:.75rem;color:var(--muted);font-weight:600;letter-spacing:.08em;text-transform:uppercase;margin:14px 0 7px}.game-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}.filter-buttons{display:flex;flex-wrap:wrap;gap:7px}.filter-panel[hidden]{display:none}.filter-btn{width:auto;min-height:42px;padding:9px 12px;background:#11112b;border:1px solid var(--border);border-radius:8px;color:var(--text);font:600 .8rem 'Exo 2',sans-serif;cursor:pointer;transition:background .2s,border-color .2s,transform .1s}.filter-btn:hover:not(:disabled){border-color:var(--accent2);background:#191942}.filter-btn:active:not(:disabled){transform:scale(.97)}.filter-btn.selected{background:var(--accent);border-color:#c4b5fd;color:#fff}.filter-btn:disabled{opacity:.35;cursor:not-allowed}"
".liblist{display:grid;gap:8px}.libitem{display:flex;align-items:center;gap:10px;padding:10px;border:1px solid var(--border);border-radius:9px}.libmeta{flex:1;min-width:0}.libname{font-weight:600}.libdetail{font-size:.75rem;color:var(--muted);margin-top:3px}.libitem .btn{width:auto;padding:7px 9px;font-size:.72rem}.libfoot{display:flex;justify-content:space-between;align-items:center;margin-top:12px;color:var(--muted);font-size:.8rem}"
"@media(max-width:650px){.libbar{grid-template-columns:1fr}.libitem{align-items:flex-start;flex-wrap:wrap}.libitem .btn{flex:1}}@media(max-width:430px){.game-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.filter-btn{flex:1}}"
".card-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:4px}"
".lbl{font-size:.75rem;color:var(--muted);font-weight:600;letter-spacing:.1em;text-transform:uppercase}"
".pnum{font-family:'Orbitron',monospace;font-size:1.1rem;font-weight:700;color:var(--accent2);background:rgba(99,102,241,.15);padding:2px 8px;border-radius:6px}"

/* Character display */
".char-info{text-align:center;padding:8px 0}"
".char-name{font-weight:600;font-size:.95rem;color:var(--bright)}"
".char-elem{font-size:.72rem;color:var(--muted);margin-top:2px;text-transform:uppercase;letter-spacing:.05em}"
".char-file{font-size:.65rem;color:var(--muted);margin-top:4px;opacity:.6}"
".empty-slot{text-align:center;padding:12px 0;color:var(--muted);"
  "font-size:.8rem;display:flex;flex-direction:column;align-items:center;gap:6px}"
".empty-slot svg{opacity:.3}"

/* Element colors */
".el-Fire{color:#f97316}.el-Water{color:#38bdf8}.el-Earth{color:#a3e635}"
".el-Air{color:#67e8f9}.el-Life{color:#4ade80}.el-Undead{color:#c084fc}"
".el-Magic{color:#f472b6}.el-Tech{color:#fbbf24}.el-Light{color:#fef08a}"
".el-Dark{color:#94a3b8}.el-Kaos{color:#f43f5e}"

/* Buttons */
".btn{width:100%;padding:9px;border-radius:8px;font-size:.83rem;"
  "font-family:'Exo 2',sans-serif;font-weight:600;cursor:pointer;"
  "border:none;transition:opacity .2s,transform .1s}"
".btn:active{transform:scale(.98)}"
".btn:disabled{opacity:.4;cursor:not-allowed}"
".btn-load{background:var(--accent);color:#fff}"
".btn-load:hover:not(:disabled){background:var(--glow)}"
".btn-unload{background:transparent;border:1px solid var(--border);color:var(--muted)}"
".btn-unload:hover{border-color:var(--hot);color:var(--hot)}"
".btn-dl{background:transparent;border:1px solid var(--border);color:var(--muted)}"
".btn-dl:hover{border-color:var(--accent2);color:var(--accent2)}"
".btn-del{background:transparent;border:1px solid #374151;color:#6b7280;"
  "font-size:.75rem;padding:6px}"
".btn-del:hover{border-color:var(--hot);color:var(--hot)}"

".upbtn{display:inline-block;background:var(--accent);color:#fff;"
  "padding:9px 20px;border-radius:8px;font-size:.85rem;font-weight:600;"
  "cursor:pointer;transition:background .2s}"
".upbtn:hover{background:var(--glow)}"

/* Sense + Status */
".sense-btn{width:100%;max-width:680px;display:block;margin:0 auto 14px;"
  "background:transparent;border:1px solid var(--border);color:var(--muted);"
  "padding:11px;border-radius:10px;font-size:.83rem;font-family:'Exo 2',sans-serif;"
  "font-weight:600;cursor:pointer;transition:all .2s;letter-spacing:.05em}"
".sense-btn:hover{border-color:var(--accent2);color:var(--accent2)}"
".status{max-width:680px;margin:0 auto;display:flex;align-items:center;"
  "gap:8px;padding:10px 14px;background:var(--card);"
  "border:1px solid var(--border);border-radius:10px;font-size:.8rem}"
".dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex-shrink:0}"
".dot.ok{background:var(--ok);box-shadow:0 0 6px var(--ok)}"
".dot.err{background:var(--hot);box-shadow:0 0 6px var(--hot)}"
"#stxt{color:var(--muted)}"
".editor-modal[hidden]{display:none}.editor-modal{position:fixed;inset:0;z-index:20;background:rgba(0,0,0,.72);padding:18px;display:flex;align-items:center;justify-content:center}.editor-panel{width:min(100%,440px);max-height:90vh;overflow:auto;background:var(--card);border:1px solid var(--accent);border-radius:14px;padding:16px;box-shadow:0 0 36px rgba(124,58,237,.35)}.editor-head{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:12px}.editor-head h2{font-size:1.05rem}.editor-close{width:auto;padding:7px 10px}.editor-gold{font-size:1.65rem;font-weight:600;color:#facc15;margin:6px 0 14px}.editor-gold label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:6px}.editor-gold input{display:block;width:100%;margin:0 0 9px;padding:10px;border-radius:8px;border:1px solid var(--border);background:var(--bg);color:var(--text);font:inherit;font-size:1.1rem}.editor-actions{display:flex;gap:8px;flex-wrap:wrap}.editor-actions .btn{width:auto}.editor-area{border-top:1px solid var(--border);padding:10px 0;font-size:.84rem;line-height:1.65}.editor-level{font-size:1.1rem;font-weight:600;color:#facc15;margin:6px 0}.editor-ok{color:var(--ok)}.editor-bad,.editor-error{color:var(--hot)}.editor-note{font-size:.8rem;color:var(--muted);margin:0 0 8px}.btn-inspect{background:#334155;color:#fff}.btn-inspect:hover{background:#475569}"
"</style></head><body>"
"<div class='wrap'>"
"<header>"
  "<h1>◆ Kaos Dual Extended</h1>"
  "<p class='sub'>Skylander Portal Manager</p>"
"</header>"

/* Portal mode toggle */
"<div style='text-align:center;margin-bottom:12px'>"
  "<button id='btnMode' class='btn' onclick='toggleMode()' style='width:200px'>Mode: Traptanium</button>"
"</div>"

/* Indexed library: data is requested a page at a time, not folded into /api/state. */
"<section class='library'>"
  "<div class='card-hdr'><span class='lbl'>Skylander Library</span><button class='btn' onclick='rebuildLibrary()'>Rebuild Library</button></div>"
  "<div class='libbar'>"
    "<input id='libSearch' placeholder='Search name' oninput='queueLibrary()'>"
    "<input id='libCategory' placeholder='Category' oninput='queueLibrary()'>"
  "</div>"
  "<div class='libtools'><button id='favNav' class='filter-btn' onclick='selectLibraryMode(\"favourites\")'>Favourites</button><button id='userNav' class='filter-btn' onclick='selectLibraryMode(\"user\")'>User Added</button></div>"
  "<div id='userUpload' class='user-upload' hidden><label class='upbtn'>Upload Skylander<input id='userFile' type='file' accept='.sky,.bin,.dmp,.dump' onchange='uploadUserDump(this)'></label><div id='userStat'></div></div>"
  "<div class='libnav-title'>Games <button id='allGames' class='filter-btn' hidden onclick='selectGame()'>All Games</button></div>"
  "<div id='gameButtons' class='game-grid'></div>"
  "<div id='elementPanel' class='filter-panel' hidden><div class='libnav-title'>Elements</div><div id='elementButtons' class='filter-buttons'></div></div>"
  "<div id='typePanel' class='filter-panel' hidden><div class='libnav-title'>Types</div><div id='typeButtons' class='filter-buttons'></div></div>"
  "<div id='libList' class='liblist'></div>"
  "<div class='libfoot'><span id='libCount'>Library loading…</span><span><button class='btn' onclick='libraryPage(-1)'>&larr;</button> <button class='btn' onclick='libraryPage(1)'>&rarr;</button></span></div>"
"</section>"
"<div id='editorModal' class='editor-modal' hidden onclick='if(event.target===this)closeEditor()'><div class='editor-panel' role='dialog' aria-modal='true' aria-label='Skylander inspector'><div class='editor-head'><h2>Skylander Inspector</h2><button class='btn editor-close' onclick='closeEditor()'>Close</button></div><div id='editorBody'>Loading…</div></div></div>"

/* Slot cards — static structure, JS only updates inner content divs */
"<div class='slots'>"
  "<div class='card' id='card0'>"
    "<div class='card-hdr'>"
      "<span class='lbl'>Player 1 &nbsp;·&nbsp; Slot 0</span><span class='pnum'>P1</span>"
    "</div>"
    "<div id='info0'></div>"
    "<div id='extra0'></div>"
  "</div>"
  "<div class='card' id='card1'>"
    "<div class='card-hdr'>"
      "<span class='lbl'>Player 2 &nbsp;·&nbsp; Slot 1</span><span class='pnum'>P2</span>"
    "</div>"
    "<div id='info1'></div>"
    "<div id='extra1'></div>"
  "</div>"
  "<div class='card' id='card2'>"
    "<div class='card-hdr'>"
      "<span class='lbl'>Player 3 &nbsp;·&nbsp; Slot 2</span><span class='pnum'>P3</span>"
    "</div>"
    "<div id='info2'></div>"
    "<div id='extra2'></div>"
  "</div>"
  "<div class='card' id='card3'>"
    "<div class='card-hdr'>"
      "<span class='lbl'>Player 4 &nbsp;·&nbsp; Slot 3</span><span class='pnum'>P4</span>"
    "</div>"
    "<div id='info3'></div>"
    "<div id='extra3'></div>"
  "</div>"
"</div>"

/* Sense */
"<button class='sense-btn' onclick='doSense()'>&#8635; Force Sense</button>"

/* Status */
"<div class='status'><div class='dot' id='dot'></div><span id='stxt'>Connecting...</span></div>"
"</div>"

"<script>"
/* ── Constants ─────────────────────────────────────────── */
"const EL={"
  "Fire:'🔥',Water:'💧',Earth:'🌿',Air:'💨',"
  "Life:'🌱',Undead:'💀',Magic:'✨',Tech:'⚙️',"
  "Light:'☀️',Dark:'🌑',Kaos:'🔮'"
"};"
"const PT={0:\"Spyro's Adv\",1:'Giants/SwapForce',2:'Trap Team',3:'Imaginators'};"

/* ── State ─────────────────────────────────────────────── */
/* Single source of truth — never read from DOM to make decisions */
"let files=[];"          /* string[] — current file list from server */
"const PORTAL_SLOT_COUNT=4;"
"let slots=[{},{},{},{}];" /* slot state objects from server */
"let libPage=1,libTotal=0,libTimer=0,libCatalogRequest=0,libGame='',libElement='',libType='',libSource='',libFavourites=false,libCatalog=null,libCatalogPromise=null;"
/* Portal type state removed — hardcoded Traptanium */

/* ── Render ────────────────────────────────────────────── */
/* Called after every state fetch. Updates DOM to match state.
 * Selects are ONLY populated once (when files change).
 * Never destroys interactive elements — only updates their properties. */
"let lastFileKey='';"

"function greyOutDuplicates(){"
  "if(portalMode!==2)return;" /* Generic mode — allow same character both slots */
  "for(let i=0;i<PORTAL_SLOT_COUNT;i++){"
    "const sel=document.getElementById('sel'+i);"
    "if(!sel)continue;"
    "for(const opt of sel.options){"
      "opt.disabled=slots.some((other,index)=>index!==i&&other&&other.loaded&&other.filename===opt.value);"
      "opt.style.color=opt.disabled?'#555':'';"
    "}"
  "}"
"}"

"function renderFiles(){"
  "const key=files.join('|');"
  "if(key===lastFileKey)return;"
  "lastFileKey=key;"
  "for(let i=0;i<PORTAL_SLOT_COUNT;i++){"
    "const sel=document.getElementById('sel'+i);"
    "if(!sel)continue;"
    "const cur=sel.value;"
    "sel.innerHTML=files.map(f=>'<option>'+f+'</option>').join('');"
    "if(files.includes(cur))sel.value=cur;"
  "}"
"}"

"function renderSlot(i){"
  "const s=slots[i]||{};"
  "const card=document.getElementById('card'+i);"
  "const info=document.getElementById('info'+i);"
  "const extra=document.getElementById('extra'+i);"
  "if(!card||!info||!extra)return;"

  "card.classList.toggle('loaded',!!s.loaded);"

  /* Info panel */
  "if(s.loaded){"
    "const e=s.element||'Magic';"
    "const displayName=s.name||String(s.filename||'').replace(/\\.[^.]+$/,'')||'Loaded Skylander';"
    "info.innerHTML="
      "'<div class=\"char-info\">'+"
      "'<div class=\"char-name\">'+displayName+'</div>'+"
      "'<div class=\"char-elem el-'+e+'\">'+(EL[e]||'')+'  '+e+'</div>'+"
      "'<div class=\"char-file\">'+(s.filename||'')+'</div>'+"
      "'</div>';"
  "}else{"
    "info.innerHTML="
      "'<div class=\"empty-slot\">'+"
      "'<svg width=32 height=32 viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=1.5>'+"
      "'<circle cx=12 cy=12 r=10/><path d=\"M8 13s1.5 2 4 2 4-2 4-2\"/><line x1=9 y1=9 x2=9.01 y2=9/><line x1=15 y1=9 x2=15.01 y2=9/>'+"
      "'</svg>No Skylander</div>';"
  "}"

  /* Extra buttons (download + unload) — only shown when loaded */
  "if(s.loaded){"
    "extra.innerHTML="
      "'<button class=\"btn btn-dl\" onclick=\"doDl('+i+')\">&#8681; Download save</button>'+"
      "'<button class=\"btn btn-unload\" onclick=\"doUnload('+i+')\">Unload</button>';"
  "}else{extra.innerHTML='';}"
"}"

/* renderPortalType / setPortalType removed — Traptanium hardcoded */

"var portalMode=2;" /* 0=Generic, 2=Traptanium (default) */
"function updateModeBtn(){"
  "document.getElementById('btnMode').textContent='Mode: '+(portalMode===2?'Traptanium':'Generic');"
"}"
"async function toggleMode(){"
  "portalMode=(portalMode===2?0:2);"
  "updateModeBtn();"
  "await fetch('/api/portaltype',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({type:portalMode})});"
"}"

/* ── Fetch ─────────────────────────────────────────────── */
"async function poll(){"
  "try{"
    "const r=await fetch('/api/state');"
    "if(!r.ok)throw new Error('bad');"
    "const d=await r.json();"
    "files=d.files||[];"
    "slots=d.slots||[{},{},{},{}];"
    "if(typeof d.portal_type!=='undefined'){portalMode=d.portal_type;updateModeBtn();}"
    "renderFiles();"
    "for(let i=0;i<PORTAL_SLOT_COUNT;i++)renderSlot(i);"
    "greyOutDuplicates();"
    "updateModeBtn();"
    "st('Connected',1);"
  "}catch(e){st('No connection',0)}"
"}"

/* ── Actions ───────────────────────────────────────────── */
"async function doLoad(i){"
  "const sel=document.getElementById('sel'+i);"
  "const file=sel?sel.value:'';"
  "if(!file){st('No file selected',0);return;}"
  "if(portalMode===2){"
    "if(slots.some((other,index)=>index!==i&&other&&other.loaded&&other.filename===file)){st('Already loaded in another slot',0);return;}"
  "}"
  "st('Loading...',1);"
  "try{"
    "const r=await fetch('/api/load',{method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({slot:i,file})});"
    "const d=await r.json();"
    "if(d.ok){await poll();st('Loaded: '+file,1);}else st('Failed: '+(d.error||'?'),0);"
  "}catch(e){st('Error',0)}"
"}"

"function esc(s){return String(s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}"
"const LIB_GAMES=[`Spyros Adventure`,`Giants`,`Swapforce`,`Trap Team`,`Superchargers`,`Imaginators`];"
"const LIB_ELEMENTS=[`Magic`,`Water`,`Fire`,`Life`,`Undead`,`Earth`,`Air`,`Tech`,`Light`,`Dark`,`Kaos`];"
"function libFilters(){return {q:document.getElementById('libSearch').value.trim(),game:libGame,element:libElement,type:libType,category:document.getElementById('libCategory').value.trim(),source:libSource,favourites:libFavourites?1:0};}"
"function editorSupported(e){const c=String(e.category||'').toLowerCase(),t=String(e.type||'').toLowerCase(),g=String(e.game||'').toLowerCase();if(String(e.source||'').toLowerCase()==='user')return true;return c==='skylander'&&g!=='imaginators'&&!['trap','vehicle','creation crystal','sidekick'].includes(c)&&!['trap','vehicle','creation crystal','sidekick'].includes(t);}"
"function selectLibraryMode(mode){const active=(mode==='user'&&libSource==='user')||(mode==='favourites'&&libFavourites);libSource=active?'':(mode==='user'?'user':'');libFavourites=active?false:mode==='favourites';libPage=1;document.getElementById('userUpload').hidden=!libSource;document.getElementById('userNav').classList.toggle('selected',!!libSource);document.getElementById('favNav').classList.toggle('selected',libFavourites);refreshLibrary();}"
"function navValue(v){return encodeURIComponent(v).replace(/'/g,'%27');}"
"function navButton(label,selected,action){return `<button class='filter-btn${selected?' selected':''}' ${action}>${esc(label)}</button>`;}"
"function libEqual(a,b){return !b||String(a||'').trim().toLowerCase()===String(b).trim().toLowerCase();}"
"function libNameMatch(name,q){return !q||String(name||'').toLowerCase().includes(String(q).trim().toLowerCase());}"
"function navEntries(){if(!libCatalog)return [];const category=document.getElementById('libCategory').value.trim();return libCatalog.filter(e=>libEqual(e.game,libGame)&&libEqual(e.category,category)&&libEqual(e.source,libSource)&&(!libFavourites||e.favourite));}"
"function filteredEntries(){const q=document.getElementById('libSearch').value.trim();return navEntries().filter(e=>libEqual(e.element,libElement)&&libEqual(e.type,libType)&&libNameMatch(e.name,q));}"
"function renderNavigation(){const games=document.getElementById('gameButtons');games.innerHTML=LIB_GAMES.map(g=>navButton(g,libGame===g,`data-game='${navValue(g)}' onclick='selectGame(decodeURIComponent(this.dataset.game))'`)).join('');const back=document.getElementById('allGames');back.hidden=!libGame;if(!libGame){document.getElementById('elementPanel').hidden=true;document.getElementById('typePanel').hidden=true;return;}const base=navEntries();const present=new Map(base.filter(e=>e.element).map(e=>[e.element.toLowerCase(),e.element]));const ordered=LIB_ELEMENTS.filter(e=>present.has(e.toLowerCase())).map(e=>present.get(e.toLowerCase()));document.getElementById('elementPanel').hidden=false;document.getElementById('elementButtons').innerHTML=navButton('All Elements',!libElement,`onclick='selectElement()'`)+ordered.map(e=>navButton(e,libElement.toLowerCase()===e.toLowerCase(),`data-value='${navValue(e)}' onclick='selectElement(decodeURIComponent(this.dataset.value))'`)).join('');const q=document.getElementById('libSearch').value.trim();const types=[...new Set(base.filter(e=>libEqual(e.element,libElement)&&libNameMatch(e.name,q)&&e.type).map(e=>e.type))].sort((a,b)=>a.localeCompare(b));document.getElementById('typePanel').hidden=false;document.getElementById('typeButtons').innerHTML=navButton('All Types',!libType,`onclick='selectType()'`)+types.map(t=>navButton(t,libType.toLowerCase()===t.toLowerCase(),`data-value='${navValue(t)}' onclick='selectType(decodeURIComponent(this.dataset.value))'`)).join('');}"
"function selectGame(game=''){libGame=game;libElement='';libType='';libPage=1;refreshLibrary();}"
"function selectElement(element=''){libElement=element;libType='';libPage=1;refreshLibrary();}"
"function selectType(type=''){libType=type;libPage=1;refreshLibrary();}"
"function queueLibrary(){clearTimeout(libTimer);libTimer=setTimeout(()=>{libPage=1;refreshLibrary();},220);}"
"async function loadLibraryCatalog(force=false,keepVisible=false){if(libCatalogPromise&&!force)return libCatalogPromise;if(force&&!keepVisible)libCatalog=null;const request=++libCatalogRequest;if(!libCatalog)document.getElementById('libCount').textContent='Loading library metadata…';const noCache={cache:'no-store'};libCatalogPromise=Promise.all([fetch('/api/library/catalog',noCache).then(r=>r.json()),fetch('/api/library/favourite-keys',noCache).then(r=>r.json())]).then(([entries,keys])=>{if(request!==libCatalogRequest)return;const favourites=new Set(keys);libCatalog=entries;libCatalog.forEach(e=>e.favourite=favourites.has(`${e.source}:${e.path}`));libCatalogPromise=null;refreshLibrary();}).catch(()=>{if(request===libCatalogRequest){libCatalogPromise=null;document.getElementById('libCount').textContent='Library unavailable';}});return libCatalogPromise;}"
"function refreshLibrary(){if(!libCatalog){loadLibraryCatalog();return;}renderNavigation();fetchLibrary();}"
"function fetchLibrary(){if(!libCatalog)return;const entries=filteredEntries();libTotal=entries.length;const start=(libPage-1)*20;const d={entries:entries.slice(start,start+20),user_total:libCatalog.filter(e=>e.source==='user').length};const list=document.getElementById('libList');"
"const card=e=>`<div class='libitem'><div class='libmeta'><div class='libname'>${esc(e.name)}</div><div class='libdetail'>Game: ${esc(e.game||'—')}${e.element?' · Element: '+esc(e.element):''}${e.type&&e.type!=='Normal'?' · Type: '+esc(e.type):''}${e.variant&&e.variant!=='Normal'?' · Variant: '+esc(e.variant):''} · ${esc(e.category)}</div></div><button class='btn fav-btn${e.favourite?' on':''}' title='Toggle favourite' aria-label='Toggle favourite' onclick='toggleFavourite(${e.id},${e.favourite?0:1})'>${e.favourite?'★':'☆'}</button>${editorSupported(e)?`<button class='btn btn-inspect' onclick='inspectLibrary(${e.id})'>Inspect</button>`:''}<button class='btn btn-load' onclick='loadLibrary(0,${e.id})'>Load P1</button><button class='btn btn-load' onclick='loadLibrary(1,${e.id})'>Load P2</button><button class='btn btn-load' onclick='loadLibrary(2,${e.id})'>Load P3</button><button class='btn btn-load' onclick='loadLibrary(3,${e.id})'>Load P4</button><button class='btn btn-dl' onclick='downloadLibrary(${e.id})'>Download</button>${e.source==='user'?`<button class='btn btn-libdel' title='Delete Skylander' aria-label='Delete Skylander' onclick='deleteUserDump(${e.id},${JSON.stringify(e.name)})'>Delete</button>`:''}</div>`;"
"list.innerHTML=(d.entries||[]).map(card).join('')||`<div class='empty-slot'>No matching Skylanders</div>`;document.getElementById('libCount').textContent=libTotal+' entries · User Added: '+(d.user_total||0)+' · page '+libPage;}"
"function libraryPage(delta){const max=Math.max(1,Math.ceil(libTotal/20));libPage=Math.min(max,Math.max(1,libPage+delta));refreshLibrary();}"
"async function loadLibrary(slot,id){const entry=libCatalog&&libCatalog.find(e=>Number(e.id)===Number(id));if(portalMode===2&&entry&&slots.some((other,index)=>index!==slot&&other&&other.loaded&&other.filename===entry.path)){st('Already loaded in another slot',0);return;}st('Loading…',1);try{const r=await fetch('/api/library/load',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slot,id})});const d=await r.json();if(d.ok){await poll();st('Loaded: '+d.name,1)}else st(d.error||'Load failed',0)}catch(e){st('Error',0)}}"
"function downloadLibrary(id){window.location='/api/library/download?id='+id;}"
"function closeEditor(){document.getElementById('editorModal').hidden=true;}"
"function editorArea(label,a,active){return `<div class='editor-area'><b>Save Area ${label}${active?' · Active':''}</b><br>Gold: ${Number(a.gold).toLocaleString()}<br>Counter: ${a.counter}<br><span class='${a.valid?'editor-ok':'editor-bad'}'>${a.valid?'Legacy CRC verified':'Legacy CRC not matched'}</span></div>`;}"
"function editorProgression(p){if(!p)return '';const row=(label,v,active)=>`<div>${label}${active?' · Active':''}: ${Number(v.component1).toLocaleString()} + ${Number(v.component2).toLocaleString()} + ${Number(v.component3).toLocaleString()} = <b>${Number(v.total).toLocaleString()}</b></div>`;const level=Number.isInteger(Number(p.level))?`<div class='editor-level'>Level: ${Number(p.level)}</div>`:'';return `<div class='editor-area'><b>Progression values</b>${level}${row('Save Area A',p.a,p.selected==='a')}${row('Save Area B',p.b,p.selected==='b')}<div class='editor-note'>Level is calculated from the active save area's cumulative progression.</div></div>`;}"
"function editorGoldError(text){const el=document.getElementById('editorStatus');if(el)el.innerHTML=`<div class='editor-error'>${esc(text)}</div>`;}"
"function checkedGoldValue(){const input=document.getElementById('editorGold'),raw=input?input.value:'';if(!raw||raw!==raw.trim()||!/^[0-9]+$/.test(raw))throw new Error('Enter a whole number from 0 to 65,535.');const value=Number(raw);if(!Number.isSafeInteger(value)||value>65535)throw new Error('Gold must be between 0 and 65,535.');return value;}"
"function checkedLevelValue(){const input=document.getElementById('editorLevel'),raw=input?input.value:'';if(!raw||raw!==raw.trim()||!/^[0-9]+$/.test(raw))throw new Error('Enter a whole level from 1 to 20.');const value=Number(raw);if(!Number.isSafeInteger(value)||value<1||value>20)throw new Error('Level must be between 1 and 20.');return value;}"
"async function saveInspectorGold(id){let gold;try{gold=checkedGoldValue();}catch(e){editorGoldError(e.message);return;}const button=document.getElementById('saveGold');button.disabled=true;editorGoldError('Saving and verifying...');try{const r=await fetch('/api/library/editor/gold',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,gold})}),text=await r.text();let d;try{d=JSON.parse(text);}catch(_){throw new Error('The portal returned HTTP '+r.status+' instead of save data.');}if(!r.ok||!d.ok){editorGoldError(d.error||'Gold was not saved.');return;}await inspectLibrary(id);}catch(e){editorGoldError(e.message||'Gold save failed.');}finally{const current=document.getElementById('saveGold');if(current)current.disabled=false;}}"
"async function saveInspectorLevel(id){let level;try{level=checkedLevelValue();}catch(e){editorGoldError(e.message);return;}const button=document.getElementById('saveLevel');button.disabled=true;editorGoldError('Saving and verifying...');try{const r=await fetch('/api/library/editor/level',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,level})}),text=await r.text();let d;try{d=JSON.parse(text);}catch(_){throw new Error('The portal returned HTTP '+r.status+' instead of save data.');}if(!r.ok||!d.ok){editorGoldError(d.error||'Level was not saved.');return;}await inspectLibrary(id);}catch(e){editorGoldError(e.message||'Level save failed.');}finally{const current=document.getElementById('saveLevel');if(current)current.disabled=false;}}"
"async function restoreInspectorBackup(id){if(!confirm('Restore the previous gold backup?'))return;const button=document.getElementById('restoreGold');if(button)button.disabled=true;editorGoldError('Restoring and verifying...');try{const r=await fetch('/api/library/editor/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})}),text=await r.text();let d;try{d=JSON.parse(text);}catch(_){throw new Error('The portal returned HTTP '+r.status+' instead of restore data.');}if(!r.ok||!d.ok){editorGoldError(d.error||'Backup restore failed.');return;}await inspectLibrary(id);}catch(e){editorGoldError(e.message||'Backup restore failed.');}finally{const current=document.getElementById('restoreGold');if(current)current.disabled=false;}}"
"async function inspectLibrary(id){const modal=document.getElementById('editorModal'),body=document.getElementById('editorBody');modal.hidden=false;body.textContent='Inspecting dump...';try{const r=await fetch('/api/library/editor?id='+encodeURIComponent(id),{cache:'no-store'}),text=await r.text();let d;try{d=JSON.parse(text);}catch(_){throw new Error('The portal returned HTTP '+r.status+' instead of inspector data.');}if(!r.ok||!d.ok){body.innerHTML=`<div class='editor-error'>${esc(d.error||'Inspection failed.')}</div>`;return;}const controls=d.goldEditable&&d.levelEditable?`<div class='editor-gold'><label for='editorGold'>Gold</label><input id='editorGold' type='text' inputmode='numeric' autocomplete='off' value='${Number(d.gold)}'><label for='editorLevel'>Level</label><input id='editorLevel' type='text' inputmode='numeric' autocomplete='off' value='${Number(d.progression.level)}'><div class='editor-note'>Saving a level sets progression to that level's starting XP.</div><div class='editor-actions'><button id='saveGold' class='btn btn-load' onclick='saveInspectorGold(${Number(id)})'>Save Gold</button><button id='saveLevel' class='btn btn-load' onclick='saveInspectorLevel(${Number(id)})'>Save Level</button>${d.hasBackup?`<button id='restoreGold' class='btn' onclick='restoreInspectorBackup(${Number(id)})'>Restore Backup</button>`:''}</div></div>`:`<div class='editor-gold'>Gold: ${Number(d.gold).toLocaleString()}</div><div class='editor-note'>Gold and level editing require a checksum-validated active save area.</div>`;body.innerHTML=`<div class='libname'>${esc(d.name)}</div><div class='libdetail'>${esc(d.game||'Unknown')} · ${esc(d.representation)} · ${esc(d.source||'')}</div>${controls}<div id='editorStatus'></div>${d.warning?`<div class='libdetail'>${esc(d.warning)}</div>`:''}${editorProgression(d.progression)}${editorArea('A',d.saveAreas.a,d.saveAreas.selected==='a')}${editorArea('B',d.saveAreas.b,d.saveAreas.selected==='b')}<div class='libdetail'>Active Area: ${String(d.saveAreas.selected).toUpperCase()}</div>`;}catch(e){body.innerHTML=`<div class='editor-error'>${esc(e.message||'Inspector request failed.')}</div>`;}}"
"async function toggleFavourite(id,favourite){try{const r=await fetch('/api/library/favourite',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,favourite})});if((await r.json()).ok){const e=libCatalog&&libCatalog.find(x=>Number(x.id)===Number(id));if(e)e.favourite=Number(favourite)!==0;refreshLibrary();}}catch(e){st('Favourite failed',0)}}"
"async function deleteUserDump(id,name){if(!confirm('Delete “'+name+'.sky” permanently?'))return;try{const r=await fetch('/api/library/delete',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});const d=await r.json();if(d.ok&&libCatalog){libCatalog=libCatalog.filter(e=>Number(e.id)!==Number(id));const max=Math.max(1,Math.ceil(filteredEntries().length/20));libPage=Math.min(libPage,max);refreshLibrary();loadLibraryCatalog(true,true)}else if(!d.ok)st(d.error||'Delete failed',0)}catch(e){st('Delete failed',0)}}"
"async function uploadUserDump(inp){const f=inp.files[0],s=document.getElementById('userStat');if(!f)return;if(f.size!==1024){s.textContent='File must be exactly 1024 bytes';return;}const fd=new FormData();fd.append('file',f);try{const r=await fetch('/api/library/upload',{method:'POST',body:fd});const d=await r.json();s.textContent=d.ok?'Uploaded':(d.error||'Upload failed');if(d.ok)await loadLibraryCatalog(true)}catch(e){s.textContent='Upload failed'}inp.value='';}"
"async function rebuildLibrary(){st('Rebuilding library…',1);try{const r=await fetch('/api/library/rebuild',{method:'POST'});const d=await r.json();if(d.ok){libPage=1;await loadLibraryCatalog(true);st('Library rebuilt',1)}else st(d.error||'Rebuild failed',0)}catch(e){st('Error',0)}}"

"async function doUnload(i){"
  "st('Unloading...',1);"
  "try{"
    "await fetch('/api/unload',{method:'POST',"
      "headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:i})});"
    "await poll();st('Unloaded',1);"
  "}catch(e){st('Error',0)}"
"}"

"async function doDl(i){"
  "window.location='/api/download?slot='+i;"
"}"

"async function doDelSel(i){"
  "const sel=document.getElementById('sel'+i);"
  "const file=sel?sel.value:'';"
  "if(!file||!confirm('Delete '+file+'?'))return;"
  "st('Deleting...',1);"
  "try{"
    "const r=await fetch('/api/delete',{method:'POST',"
      "headers:{'Content-Type':'application/json'},body:JSON.stringify({file})});"
    "const d=await r.json();"
    "if(d.ok){lastFileKey='';await poll();st('Deleted',1);}else st('Delete failed',0);"
  "}catch(e){st('Error',0)}"
"}"

"async function doUpload(inp){"
  "const f=inp.files[0];if(!f)return;"
  "const stat=document.getElementById('upstat');"
  "stat.style.color='#94a3b8';stat.textContent='Uploading...';"
  "const fd=new FormData();fd.append('file',f);"
  "try{"
    "const r=await fetch('/api/upload',{method:'POST',body:fd});"
    "const d=await r.json();"
    "if(d.ok){"
      "stat.style.color='var(--ok)';stat.textContent='✓ '+f.name+' saved';"
      "lastFileKey='';await poll();"
    "}else{"
      "stat.style.color='var(--hot)';stat.textContent='✗ '+(d.error||'Upload failed');"
    "}"
  "}catch(e){stat.style.color='var(--hot)';stat.textContent='✗ Error';}"
  "inp.value='';"
"}"

"async function doSense(){"
  "try{"
    "await fetch('/api/sense',{method:'POST'});"
    "st('Sense triggered',1);"
  "}catch(e){st('Error',0)}"
"}"

"async function setPortalType(v){/* removed - Traptanium hardcoded */}"

"function st(m,ok){"
  "document.getElementById('stxt').textContent=m;"
  "document.getElementById('dot').className='dot '+(ok?'ok':'err');"
"}"

/* ── Boot ──────────────────────────────────────────────── */
"poll();loadLibraryCatalog();setInterval(poll,5000);"
"</script></body></html>";

/* -----------------------------------------------------------------------
 * JSON helpers
 * ----------------------------------------------------------------------- */
static int json_str(char *buf, int max, const char *s) {
    int n = 0;
    buf[n++] = '"';
    while (*s && n < max-2) {
        if (*s == '"' || *s == '\\') buf[n++] = '\\';
        buf[n++] = *s++;
    }
    buf[n++] = '"';
    buf[n]   = '\0';
    return n;
}

/* -----------------------------------------------------------------------
 * GET /
 * ----------------------------------------------------------------------- */
static esp_err_t handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * GET /api/state
 * ----------------------------------------------------------------------- */
static esp_err_t handle_state(httpd_req_t *req) {
    char buf[4096];  /* Increased from 2048 to handle many files */
    int n = 0;

    n += snprintf(buf+n, sizeof(buf)-n, "{\"files\":[");
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);

    for (int i = 0; i < g_file_count && n < (int)sizeof(buf)-64; i++) {
        if (i) buf[n++] = ',';
        n += json_str(buf+n, sizeof(buf)-n-4, g_file_list[i]);
    }

    n += snprintf(buf+n, sizeof(buf)-n, "],\"slots\":[");

    for (int s = 0; s < PORTAL_SLOT_ENABLED_COUNT; s++) {
        skylander_slot_t *sky = &g_skylanders[s];
        if (s) buf[n++] = ',';
        if (sky->loaded) {
            uint16_t char_id = skylander_read_char_id(sky->data);
            const char *elem = skylander_element_from_id(char_id);
            /* filename stored as full path — extract basename */
            const char *base = strrchr(sky->filename, '/');
            base = base ? base+1 : sky->filename;
            char display_name[128], nb[256], fb[80];
            /* File names are reliable library metadata even when the dump's
             * numeric character id has no entry in the local name table. */
            snprintf(display_name, sizeof(display_name), "%.127s", base);
            char *extension = strrchr(display_name, '.');
            if (extension) *extension = '\0';
            json_str(nb, sizeof(nb), display_name);
            json_str(fb, sizeof(fb), base);
            n += snprintf(buf+n, sizeof(buf)-n,
                "{\"loaded\":true,\"name\":%s,\"element\":\"%s\","
                "\"filename\":%s,\"char_id\":%u}",
                nb, elem ? elem : "Magic", fb, char_id);
        } else {
            n += snprintf(buf+n, sizeof(buf)-n, "{\"loaded\":false}");
        }
    }

    n += snprintf(buf+n, sizeof(buf)-n, "],\"portal_type\":%d}", pico_bridge_get_portal_type());
    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Indexed Skylander library
 * ----------------------------------------------------------------------- */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ESP-IDF's httpd_query_key_value() copies the raw query component. The
 * browser encodes "Trap Team" as "Trap+Team", so decode before filtering. */
static void url_decode_in_place(char *value) {
    char *read = value, *write = value;
    while (*read) {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (*read == '%' && read[1] && read[2]) {
            int high = hex_value(read[1]);
            int low = hex_value(read[2]);
            if (high >= 0 && low >= 0) {
                *write++ = (char)((high << 4) | low);
                read += 3;
            } else {
                *write++ = *read++;
            }
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void query_value(const char *query, const char *key, char *out,
                        size_t out_size) {
    out[0] = '\0';
    if (query[0] && httpd_query_key_value(query, key, out, out_size) == ESP_OK)
        url_decode_in_place(out);
}

static esp_err_t handle_library(httpd_req_t *req) {
    char query[256] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char search[96], game[32], element[16], type[32], category[24], source[8], value[12];
    query_value(query, "q", search, sizeof(search));
    query_value(query, "game", game, sizeof(game));
    query_value(query, "element", element, sizeof(element));
    query_value(query, "type", type, sizeof(type));
    query_value(query, "category", category, sizeof(category));
    query_value(query, "source", source, sizeof(source));
    query_value(query, "page", value, sizeof(value));
    int page = value[0] ? atoi(value) : 1;
    query_value(query, "limit", value, sizeof(value));
    int limit = value[0] ? atoi(value) : 20;
    if (limit < 1 || limit > 50) limit = 20;
    query_value(query, "facets", value, sizeof(value));
    bool facets_only = value[0] && atoi(value) != 0;
    query_value(query, "favourites", value, sizeof(value));
    bool favourites_only = value[0] && atoi(value) != 0;

    /* Full-library paths and favourite/source metadata can make a single
     * entry substantially larger than the original test records. */
    size_t response_size = facets_only ? 2048 : 512 + (size_t)limit * 600;
    char *response = malloc(response_size);
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool ok = facets_only
        ? library_facets_json(response, response_size, search, game, element, category,
                              source, favourites_only)
        : library_query_json(response, response_size, search, game, element,
                             type, category, source, favourites_only, page, limit);
    xSemaphoreGive(g_sky_mutex);
    if (!ok) {
        free(response);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Library unavailable");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    free(response);
    return ESP_OK;
}

/* The index is deliberately streamed: it can contain the whole collection,
 * so never allocate a second full copy in ESP32 RAM just to send it. */
static esp_err_t handle_library_catalog(httpd_req_t *req) {
    FILE *file = fopen(LIBRARY_INDEX_PATH, "rb");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!file) return httpd_resp_sendstr(req, "[]");
    char buffer[1024];
    size_t count;
    esp_err_t result = ESP_OK;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        if (httpd_resp_send_chunk(req, buffer, count) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

static esp_err_t handle_library_favourite_keys(httpd_req_t *req) {
    FILE *file = fopen(LIBRARY_FAVOURITES_PATH, "rb");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!file) return httpd_resp_sendstr(req, "[]");
    char buffer[512];
    size_t count;
    esp_err_t result = ESP_OK;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        if (httpd_resp_send_chunk(req, buffer, count) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

static esp_err_t handle_library_rebuild(httpd_req_t *req) {
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool ok = library_rebuild();
    xSemaphoreGive(g_sky_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" :
                              "{\"ok\":false,\"error\":\"Rebuild failed\"}");
    return ESP_OK;
}

static esp_err_t handle_library_editor(httpd_req_t *req) {
    bool diagnostics = strstr(req->uri, "/debug") != NULL;
    char query[64] = {0};
    char id_text[16] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    query_value(query, "id", id_text, sizeof(id_text));
    char *end = NULL;
    unsigned long parsed_id = strtoul(id_text, &end, 10);
    if (!id_text[0] || !end || *end || parsed_id == 0 || parsed_id > UINT32_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"A valid library entry ID is required.\"}");
    }

    library_entry_t entry;
    sky_editor_info_t info;
    ESP_LOGI(TAG, "Inspector request: id=%lu", parsed_id);
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool found = library_find((uint32_t)parsed_id, &entry);
    sky_editor_figure_class_t entry_class = found
        ? sky_editor_entry_class(&entry) : SKY_EDITOR_UNSUPPORTED;
    bool supported = found && sky_editor_entry_supported(&entry);
    esp_err_t inspect_result = supported
        ? sky_editor_inspect_file(&entry, &info)
        : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(g_sky_mutex);
    ESP_LOGI(TAG, "Inspector result: id=%lu found=%d supported=%d result=%s",
             parsed_id, found, supported, esp_err_to_name(inspect_result));

    httpd_resp_set_type(req, "application/json");
    if (!found) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"The library entry no longer exists.\"}");
    }
    if (!supported) {
        httpd_resp_set_status(req, "422 Unprocessable Content");
        char response[300];
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"supported\":false,\"figureClass\":\"%s\","
                 "\"error\":\"Normal-character inspection is not available for %s files yet.\"}",
                 sky_editor_figure_class_name(entry_class),
                 sky_editor_figure_class_name(entry_class));
        return httpd_resp_sendstr(req, response);
    }
    if (inspect_result != ESP_OK) {
        char error[300];
        json_str(error, sizeof(error), info.error[0] ? info.error : "Inspection failed.");
        httpd_resp_set_status(req, "422 Unprocessable Content");
        char response[380];
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"supported\":false,\"figureClass\":\"%s\",\"error\":%s}",
                 sky_editor_figure_class_name(info.figure_class), error);
        return httpd_resp_sendstr(req, response);
    }

    char name[220], game[100], warning[360];
    json_str(name, sizeof(name), entry.name);
    json_str(game, sizeof(game), sky_editor_generation_name(info.generation));
    json_str(warning, sizeof(warning), info.warning);
    bool gold_editable = sky_editor_gold_editable(&info);
    bool level_editable = sky_editor_level_editable(&info);
    bool has_backup = sky_editor_backup_exists(&entry);
    char response[1500];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"supported\":true,\"name\":%s,\"game\":%s,"
             "\"characterId\":%u,\"generation\":%u,\"source\":\"%s\","
             "\"representation\":\"%s\",\"saveAreas\":{"
             "\"a\":{\"valid\":%s,\"counter\":%u,\"gold\":%u},"
             "\"b\":{\"valid\":%s,\"counter\":%u,\"gold\":%u},"
             "\"selected\":\"%c\"},\"progression\":{"
             "\"a\":{\"component1\":%u,\"component2\":%u,\"component3\":%u,\"total\":%u},"
             "\"b\":{\"component1\":%u,\"component2\":%u,\"component3\":%u,\"total\":%u},"
             "\"selected\":\"%c\",\"level\":%u,\"levelProvisional\":%s},"
             "\"gold\":%u,\"goldEditable\":%s,\"levelEditable\":%s,\"hasBackup\":%s,"
             "\"state\":\"%s\",\"warning\":%s%s",
             name, game, info.character_id, (unsigned)info.generation,
             strcasecmp(entry.source, "user") == 0 ? "User Added" : "Built In",
             info.encrypted ? "encrypted" : "plaintext",
             info.area_a_valid ? "true" : "false", info.area_a_counter, info.area_a_gold,
             info.area_b_valid ? "true" : "false", info.area_b_counter, info.area_b_gold,
             info.selected_area == 'A' ? 'a' : 'b',
             info.area_a_progression.component_1, info.area_a_progression.component_2,
             (unsigned)info.area_a_progression.component_3, (unsigned)info.area_a_progression.total,
             info.area_b_progression.component_1, info.area_b_progression.component_2,
             (unsigned)info.area_b_progression.component_3, (unsigned)info.area_b_progression.total,
             info.selected_area == 'A' ? 'a' : 'b', (unsigned)info.derived_level,
             info.derived_level_provisional ? "true" : "false", info.selected_gold,
             gold_editable ? "true" : "false", level_editable ? "true" : "false",
             has_backup ? "true" : "false",
             info.state, warning,
             diagnostics ? ",\"checksums\":{" : "}");
    if (diagnostics) {
        size_t used = strlen(response);
        snprintf(response + used, sizeof(response) - used,
                 "\"header\":{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "\"a\":{\"goldBytes\":[%u,%u],\"checks\":[{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "{\"stored\":%u,\"calculated\":%u,\"valid\":%s}]},"
                 "\"b\":{\"goldBytes\":[%u,%u],\"checks\":[{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "{\"stored\":%u,\"calculated\":%u,\"valid\":%s},"
                 "{\"stored\":%u,\"calculated\":%u,\"valid\":%s}]}}}",
                 info.header_stored, info.header_calculated, info.header_valid ? "true" : "false",
                 info.area_a_gold & 0xff, info.area_a_gold >> 8,
                 info.area_a_checks.stored[0], info.area_a_checks.calculated[0], info.area_a_checks.valid[0] ? "true" : "false",
                 info.area_a_checks.stored[1], info.area_a_checks.calculated[1], info.area_a_checks.valid[1] ? "true" : "false",
                 info.area_a_checks.stored[2], info.area_a_checks.calculated[2], info.area_a_checks.valid[2] ? "true" : "false",
                 info.area_a_checks.stored[3], info.area_a_checks.calculated[3], info.area_a_checks.valid[3] ? "true" : "false",
                 info.area_b_gold & 0xff, info.area_b_gold >> 8,
                 info.area_b_checks.stored[0], info.area_b_checks.calculated[0], info.area_b_checks.valid[0] ? "true" : "false",
                 info.area_b_checks.stored[1], info.area_b_checks.calculated[1], info.area_b_checks.valid[1] ? "true" : "false",
                 info.area_b_checks.stored[2], info.area_b_checks.calculated[2], info.area_b_checks.valid[2] ? "true" : "false",
                 info.area_b_checks.stored[3], info.area_b_checks.calculated[3], info.area_b_checks.valid[3] ? "true" : "false");
    }
    esp_err_t response_result = httpd_resp_sendstr(req, response);
    ESP_LOGI(TAG, "Inspector response: id=%lu bytes=%u result=%s", parsed_id,
             (unsigned)strlen(response), esp_err_to_name(response_result));
    return response_result;
}

static esp_err_t editor_json_error(httpd_req_t *req, const char *status,
                                   const char *message) {
    char escaped[300];
    json_str(escaped, sizeof(escaped), message ? message : "Editor operation failed.");
    char response[340];
    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":%s}", escaped);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static bool read_editor_request(httpd_req_t *req, char *body, size_t body_size) {
    if (req->content_len <= 0 || (size_t)req->content_len >= body_size) return false;
    int received = 0;
    while (received < req->content_len) {
        int result = httpd_req_recv(req, body + received, req->content_len - received);
        if (result <= 0) return false;
        received += result;
    }
    body[received] = '\0';
    return true;
}

/* Parses only an unsigned decimal JSON number. It deliberately rejects
 * signed values, decimal points, exponent notation, hex, quoted values and
 * any number outside the caller's range. */
static bool editor_json_uint(const char *body, const char *field,
                             uint32_t maximum, uint32_t *value) {
    char key[32];
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return false;
    const char *cursor = strstr(body, key);
    if (!cursor) return false;
    cursor = strchr(cursor + strlen(key), ':');
    if (!cursor) return false;
    cursor++;
    while (isspace((unsigned char)*cursor)) cursor++;
    if (!isdigit((unsigned char)*cursor)) return false;
    uint32_t parsed = 0;
    do {
        uint32_t digit = (uint32_t)(*cursor - '0');
        if (parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        cursor++;
    } while (isdigit((unsigned char)*cursor));
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor != ',' && *cursor != '}') return false;
    *value = parsed;
    return true;
}

static void loaded_file_message(int slot, char *out, size_t out_size) {
    snprintf(out, out_size, "Unload this Skylander from P%d before modifying the file.",
             slot + 1);
}

static esp_err_t editor_loaded_file_error(httpd_req_t *req, int slot) {
    char message[96];
    loaded_file_message(slot, message, sizeof(message));
    return editor_json_error(req, "409 Conflict", message);
}

static esp_err_t handle_library_editor_gold(httpd_req_t *req) {
    char body[96] = {0};
    uint32_t id, gold;
    if (!read_editor_request(req, body, sizeof(body)) ||
        !editor_json_uint(body, "id", UINT32_MAX, &id) || id == 0 ||
        !editor_json_uint(body, "gold", UINT16_MAX, &gold))
        return editor_json_error(req, "400 Bad Request",
                                 "Gold must be a whole number from 0 to 65,535.");

    library_entry_t entry;
    sky_editor_info_t info;
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool found = library_find(id, &entry);
    bool supported = found && sky_editor_entry_supported(&entry);
    int loaded_slot = -1;
    bool loaded = found && skylander_path_loaded_in_any_enabled_slot(entry.path, &loaded_slot);
    esp_err_t save_result = supported && !loaded
        ? sky_editor_save_gold(&entry, (uint16_t)gold, &info)
        : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(g_sky_mutex);
    ESP_LOGI(TAG, "Gold save: id=%" PRIu32 " gold=%" PRIu32 " result=%s",
             id, gold, esp_err_to_name(save_result));

    if (!found) return editor_json_error(req, "404 Not Found", "The library entry no longer exists.");
    if (!supported) return editor_json_error(req, "422 Unprocessable Content",
                                             "Gold editing is not available for this figure.");
    if (loaded) return editor_loaded_file_error(req, loaded_slot);
    if (save_result != ESP_OK)
        return editor_json_error(req, "422 Unprocessable Content",
                                 info.error[0] ? info.error : "Gold save failed.");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_library_editor_level(httpd_req_t *req) {
    char body[96] = {0};
    uint32_t id, level;
    if (!read_editor_request(req, body, sizeof(body)) ||
        !editor_json_uint(body, "id", UINT32_MAX, &id) || id == 0 ||
        !editor_json_uint(body, "level", 20, &level))
        return editor_json_error(req, "400 Bad Request",
                                 "Level must be a whole number from 1 to 20.");
    if (level == 0)
        return editor_json_error(req, "400 Bad Request",
                                 "Level must be a whole number from 1 to 20.");

    library_entry_t entry;
    sky_editor_info_t info;
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool found = library_find(id, &entry);
    bool supported = found && sky_editor_entry_supported(&entry);
    int loaded_slot = -1;
    bool loaded = found && skylander_path_loaded_in_any_enabled_slot(entry.path, &loaded_slot);
    esp_err_t save_result = supported && !loaded
        ? sky_editor_save_level(&entry, (uint8_t)level, &info)
        : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(g_sky_mutex);
    ESP_LOGI(TAG, "Level save: id=%" PRIu32 " level=%" PRIu32 " result=%s",
             id, level, esp_err_to_name(save_result));

    if (!found) return editor_json_error(req, "404 Not Found", "The library entry no longer exists.");
    if (!supported) return editor_json_error(req, "422 Unprocessable Content",
                                             "Level editing is not available for this figure.");
    if (loaded) return editor_loaded_file_error(req, loaded_slot);
    if (save_result != ESP_OK)
        return editor_json_error(req, "422 Unprocessable Content",
                                 info.error[0] ? info.error : "Level save failed.");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_library_editor_restore(httpd_req_t *req) {
    char body[64] = {0};
    uint32_t id;
    if (!read_editor_request(req, body, sizeof(body)) ||
        !editor_json_uint(body, "id", UINT32_MAX, &id) || id == 0)
        return editor_json_error(req, "400 Bad Request", "A valid library entry ID is required.");

    library_entry_t entry;
    sky_editor_info_t info;
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool found = library_find(id, &entry);
    bool supported = found && sky_editor_entry_supported(&entry);
    int loaded_slot = -1;
    bool loaded = found && skylander_path_loaded_in_any_enabled_slot(entry.path, &loaded_slot);
    esp_err_t restore_result = supported && !loaded
        ? sky_editor_restore_backup(&entry, &info)
        : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(g_sky_mutex);
    ESP_LOGI(TAG, "Gold restore: id=%" PRIu32 " result=%s", id,
             esp_err_to_name(restore_result));

    if (!found) return editor_json_error(req, "404 Not Found", "The library entry no longer exists.");
    if (!supported) return editor_json_error(req, "422 Unprocessable Content",
                                             "Gold editing is not available for this figure.");
    if (loaded) return editor_loaded_file_error(req, loaded_slot);
    if (restore_result != ESP_OK)
        return editor_json_error(req, "422 Unprocessable Content",
                                 info.error[0] ? info.error : "Backup restore failed.");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_library_favourites(httpd_req_t *req) {
    char *response = malloc(7000);
    if (!response) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory"); return ESP_FAIL; }
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool ok = library_query_json(response, 7000, "", "", "", "", "", "", true, 1, 20);
    xSemaphoreGive(g_sky_mutex);
    if (!ok) { free(response); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Library unavailable"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, response); free(response); return ESP_OK;
}

static esp_err_t handle_library_load(httpd_req_t *req) {
    char body[128] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Bad request\"}");
        return ESP_OK;
    }
    httpd_req_recv(req, body, len);
    char *slot_text = strstr(body, "\"slot\"");
    char *id_text = strstr(body, "\"id\"");
    int slot = slot_text ? atoi(slot_text + 7) : -1;
    uint32_t id = id_text ? (uint32_t)strtoul(id_text + 5, NULL, 10) : 0;
    library_entry_t entry;
    if (slot < 0 || slot >= PORTAL_SLOT_ENABLED_COUNT || id == 0 || !library_find(id, &entry)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Unknown library entry\"}");
        return ESP_OK;
    }

    uint8_t raw[SKYLANDER_DUMP_SIZE];
    FILE *f = fopen(entry.path, "rb");
    bool got_raw = f && fread(raw, 1, sizeof(raw), f) == sizeof(raw);
    if (f) fclose(f);
    if (!got_raw) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Cannot read dump\"}");
        return ESP_OK;
    }

    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    if (g_skylanders[slot].loaded) {
        pico_bridge_unload((uint8_t)slot);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    bool ok = skylander_load((uint8_t)slot, entry.path);
    if (ok) pico_bridge_load((uint8_t)slot, raw);
    xSemaphoreGive(g_sky_mutex);

    char name[128];
    json_str(name, sizeof(name), entry.name);
    char response[180];
    snprintf(response, sizeof(response), ok ? "{\"ok\":true,\"name\":%s}" :
                                    "{\"ok\":false,\"error\":\"Load failed\"}", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t handle_library_download(httpd_req_t *req) {
    char query[32] = {0}, id_text[12] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "id", id_text, sizeof(id_text));
    library_entry_t entry;
    if (!id_text[0] || !library_find((uint32_t)strtoul(id_text, NULL, 10), &entry)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown library entry");
        return ESP_FAIL;
    }
    uint8_t raw[SKYLANDER_DUMP_SIZE];
    FILE *f = fopen(entry.path, "rb");
    size_t nread = f ? fread(raw, 1, sizeof(raw), f) : 0;
    if (f) fclose(f);
    if (nread != sizeof(raw)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Dump is not 1024 bytes");
        return ESP_FAIL;
    }
    const char *base = strrchr(entry.path, '/');
    base = base ? base + 1 : entry.path;
    /* A filename is metadata only; bound it so this HTTP header is always safe. */
    char disposition[160];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.128s\"", base);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return httpd_resp_send(req, (const char *)raw, sizeof(raw));
}

static const uint8_t *find_bytes(const uint8_t *data, size_t data_len,
                                 const char *needle, size_t needle_len) {
    if (!needle_len || needle_len > data_len) return NULL;
    for (size_t i = 0; i + needle_len <= data_len; i++)
        if (memcmp(data + i, needle, needle_len) == 0) return data + i;
    return NULL;
}

static bool user_filename(char *out, size_t out_size, const char *input) {
    if (!input || !*input || strstr(input, "..") || strchr(input, '/') || strchr(input, '\\')) return false;
    const char *dot = strrchr(input, '.');
    if (!dot || (strcasecmp(dot, ".sky") && strcasecmp(dot, ".bin") &&
                 strcasecmp(dot, ".dmp") && strcasecmp(dot, ".dump"))) return false;
    size_t stem = (size_t)(dot - input);
    if (!stem || stem + 5 > out_size) return false;
    for (size_t i = 0; i < stem; i++) {
        unsigned char c = (unsigned char)input[i];
        if (!(isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '.')) return false;
    }
    memcpy(out, input, stem);
    memcpy(out + stem, ".sky", 5);
    return true;
}

static esp_err_t handle_library_upload(httpd_req_t *req) {
    char ct[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        !strstr(ct, "boundary=")) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing multipart boundary"); return ESP_FAIL; }
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "--%s", strstr(ct, "boundary=") + 9);
    int total = req->content_len;
    if (total <= 0 || total > 4096) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid upload size"); return ESP_FAIL; }
    uint8_t *body = malloc((size_t)total);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory"); return ESP_FAIL; }
    int got = 0, read_rc;
    while (got < total && (read_rc = httpd_req_recv(req, (char *)body + got, total - got)) > 0) got += read_rc;
    const uint8_t *header_end = find_bytes(body, got, "\r\n\r\n", 4);
    const uint8_t *filename_at = find_bytes(body, got, "filename=\"", 10);
    if (!header_end || !filename_at || filename_at >= header_end) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed upload"); return ESP_FAIL; }
    filename_at += 10;
    const uint8_t *filename_end = memchr(filename_at, '"', (size_t)(header_end - filename_at));
    char original[80] = {0}, clean[80] = {0};
    if (!filename_end || (size_t)(filename_end - filename_at) >= sizeof(original)) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename"); return ESP_FAIL; }
    memcpy(original, filename_at, (size_t)(filename_end - filename_at));
    if (!user_filename(clean, sizeof(clean), original)) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename or extension"); return ESP_FAIL; }
    const uint8_t *data = header_end + 4;
    char end_marker[88]; snprintf(end_marker, sizeof(end_marker), "\r\n%s--", boundary);
    const uint8_t *end = find_bytes(data, (size_t)(body + got - data), end_marker, strlen(end_marker));
    if (!end || (size_t)(end - data) != SKYLANDER_DUMP_SIZE) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File must be exactly 1024 bytes"); return ESP_FAIL; }
    char path[256]; snprintf(path, sizeof(path), "%s/%s", LIBRARY_USER_ROOT, clean);
    FILE *existing = fopen(path, "rb");
    if (existing) { fclose(existing); free(body); httpd_resp_set_status(req, "409 Conflict"); httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"A user dump with that filename already exists\"}"); return ESP_FAIL; }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, SKYLANDER_DUMP_SIZE, f) == SKYLANDER_DUMP_SIZE && fclose(f) == 0;
    if (!ok && f) fclose(f);
    free(body);
    if (!ok) { remove(path); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed"); return ESP_FAIL; }
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY); ok = library_rebuild(); xSemaphoreGive(g_sky_mutex);
    if (!ok) { remove(path); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Index update failed"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK;
}

static esp_err_t handle_library_favourite(httpd_req_t *req) {
    char body[80] = {0}; int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body) || httpd_req_recv(req, body, len) != len) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request"); return ESP_FAIL; }
    char *id = strstr(body, "\"id\""); char *fav = strstr(body, "\"favourite\"");
    uint32_t value = id ? (uint32_t)strtoul(id + 5, NULL, 10) : 0;
    bool set = fav && (strstr(fav, "true") || strstr(fav, ":1"));
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY); bool ok = value && library_set_favourite(value, set); xSemaphoreGive(g_sky_mutex);
    if (!ok) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown library entry"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK;
}

static esp_err_t handle_library_delete(httpd_req_t *req) {
    char body[48] = {0}; int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body) || httpd_req_recv(req, body, len) != len) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request"); return ESP_FAIL; }
    char *id = strstr(body, "\"id\""); uint32_t value = id ? (uint32_t)strtoul(id + 5, NULL, 10) : 0; int status = 404;
    library_entry_t entry;
    int loaded_slot = -1;
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool found = value && library_find(value, &entry);
    bool loaded = found && skylander_path_loaded_in_any_enabled_slot(entry.path, &loaded_slot);
    bool ok = found && !loaded && library_delete_user(value, &status);
    xSemaphoreGive(g_sky_mutex);
    if (loaded) {
        char message[96];
        loaded_file_message(loaded_slot, message, sizeof(message));
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, message);
        return ESP_FAIL;
    }
    if (!ok) { httpd_resp_send_err(req, status == 403 ? HTTPD_403_FORBIDDEN : HTTPD_404_NOT_FOUND, status == 403 ? "Only user dumps can be deleted" : "Unknown user dump"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/load
 * ----------------------------------------------------------------------- */
static esp_err_t handle_load(httpd_req_t *req) {
    char body[128] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Bad body\"}");
        return ESP_OK;
    }
    httpd_req_recv(req, body, len);

    int slot = -1;
    char file[64] = {0};
    char *ps = strstr(body, "\"slot\"");
    if (ps) slot = atoi(ps + 7);
    char *pf = strstr(body, "\"file\"");
    if (pf) {
        pf = strchr(pf+6, '"');
        if (pf) { pf++; int fi=0; while(*pf&&*pf!='"'&&fi<63) file[fi++]=*pf++; }
    }

    if (slot < 0 || slot >= PORTAL_SLOT_ENABLED_COUNT || !file[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid params\"}");
        return ESP_OK;
    }

    char full[96];
    spiffs_full_path(file, full, sizeof(full));
    ESP_LOGI(TAG, "Load slot %d from %s", slot, full);

    /* Read raw bytes for Pico, then load decrypted for ESP32 metadata */
    static uint8_t raw[SKYLANDER_DUMP_SIZE];
    bool got_raw = false;
    FILE *rf = fopen(full, "rb");
    if (rf) {
        got_raw = (fread(raw, 1, SKYLANDER_DUMP_SIZE, rf) == SKYLANDER_DUMP_SIZE);
        fclose(rf);
    }

    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    /* If slot is already loaded, unload it first so the game sees a clean
     * removal before the new figure arrives — prevents figure index confusion */
    if (g_skylanders[slot].loaded) {
        pico_bridge_unload((uint8_t)slot);
        vTaskDelay(pdMS_TO_TICKS(200)); /* give game time to process removal */
    }
    bool ok = skylander_load((uint8_t)slot, full);
    if (ok && got_raw) {
        ESP_LOGI(TAG, "Pushing slot %d to Pico", slot);
        pico_bridge_load((uint8_t)slot, raw);
    }
    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Load failed\"}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/unload
 * ----------------------------------------------------------------------- */
static esp_err_t handle_unload(httpd_req_t *req) {
    char body[64] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) httpd_req_recv(req, body, len);
    int slot = -1;
    char *ps = strstr(body, "\"slot\"");
    if (ps) slot = atoi(ps + 7);
    if (slot < 0 || slot >= PORTAL_SLOT_ENABLED_COUNT) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid slot\"}");
        return ESP_OK;
    }
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    skylander_unload((uint8_t)slot);
    xSemaphoreGive(g_sky_mutex);
    pico_bridge_unload((uint8_t)slot);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/sense
 * ----------------------------------------------------------------------- */
static esp_err_t handle_sense(httpd_req_t *req) {
    ESP_LOGI(TAG, "Sense requested");
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    int loaded = 0;
    for (int s = 0; s < PORTAL_SLOT_ENABLED_COUNT; s++) {
        if (!g_skylanders[s].loaded) continue;
        loaded++;
        static uint8_t raw[SKYLANDER_DUMP_SIZE];
        FILE *f = fopen(g_skylanders[s].filename, "rb");
        if (f) {
            size_t n = fread(raw, 1, SKYLANDER_DUMP_SIZE, f);
            fclose(f);
            ESP_LOGI(TAG, "  Slot %d: re-sending %u bytes", s, (unsigned)n);
            pico_bridge_unload((uint8_t)s);
            vTaskDelay(pdMS_TO_TICKS(80));
            pico_bridge_load((uint8_t)s, raw);
        } else {
            ESP_LOGE(TAG, "  Slot %d: cannot open %s", s, g_skylanders[s].filename);
        }
    }
    if (!loaded) {
        ESP_LOGW(TAG, "  No slots loaded — sending empty unloads");
        for (uint8_t slot = 0; slot < PORTAL_SLOT_ENABLED_COUNT; slot++)
            pico_bridge_unload(slot);
    }
    xSemaphoreGive(g_sky_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/portaltype
 * ----------------------------------------------------------------------- */
static esp_err_t handle_portaltype(httpd_req_t *req) {
    char body[64] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) httpd_req_recv(req, body, len);
    char *pt = strstr(body, "\"type\"");
    if (pt) {
        int t = atoi(pt + 7);
        if (t >= 0 && t <= 3) {
            /* Unload all enabled slots first — Pico will reboot and lose state */
            xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
            for (uint8_t slot = 0; slot < PORTAL_SLOT_ENABLED_COUNT; slot++)
                skylander_unload(slot);
            xSemaphoreGive(g_sky_mutex);
            for (uint8_t slot = 0; slot < PORTAL_SLOT_ENABLED_COUNT; slot++)
                pico_bridge_unload(slot);
            vTaskDelay(pdMS_TO_TICKS(100));
            pico_bridge_set_portal_type((uint8_t)t);
            ESP_LOGI(TAG, "Portal type → %d", t);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/upload  — multipart form upload → SPIFFS
 * ----------------------------------------------------------------------- */
static esp_err_t handle_upload(httpd_req_t *req) {
    /* Parse multipart boundary from Content-Type header */
    char ct[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No Content-Type\"}");
        return ESP_OK;
    }

    char *bnd_ptr = strstr(ct, "boundary=");
    if (!bnd_ptr) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No boundary\"}");
        return ESP_OK;
    }
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "--%s", bnd_ptr + 9);

    /* Read entire multipart body into a heap buffer */
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 16384) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Bad size\"}");
        return ESP_OK;
    }

    char *body = malloc(total_len + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"OOM\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) break;
        received += r;
    }
    body[received] = '\0';

    /* Extract filename from Content-Disposition header inside multipart */
    char filename[64] = "upload.bin";
    char *cd = strstr(body, "Content-Disposition:");
    if (cd) {
        char *fn = strstr(cd, "filename=\"");
        if (fn) {
            fn += 10;
            int fi = 0;
            while (*fn && *fn != '"' && fi < 63) filename[fi++] = *fn++;
            filename[fi] = '\0';
        }
    }

    /* Find start of binary data (after \r\n\r\n following headers) */
    char *data_start = strstr(body, "\r\n\r\n");
    if (!data_start) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Malformed multipart\"}");
        return ESP_OK;
    }
    data_start += 4;

    /* Find end boundary */
    char end_boundary[84];
    snprintf(end_boundary, sizeof(end_boundary), "\r\n%s--", boundary);
    char *data_end = strstr(data_start, end_boundary);
    if (!data_end) data_end = body + received;

    int data_len = (int)(data_end - data_start);
    ESP_LOGI(TAG, "Upload: '%s' %d bytes", filename, data_len);

    /* Skylander dumps must be exactly 1024 bytes.
     * Some formats (e.g. Flipper Zero .nfc) have headers — find the 1024-byte
     * block by scanning for the last 1024-byte aligned chunk, or just take
     * the last 1024 bytes if the file is larger. */
    char *dump_start = data_start;
    int   dump_len   = data_len;

    if (data_len > SKYLANDER_DUMP_SIZE) {
        /* Take the first 1024 bytes — headers are prepended in some formats,
         * but the actual MIFARE dump starts right at the beginning.
         * 1070 bytes = 1024 dump + 46 byte footer/padding, so just truncate. */
        dump_len = SKYLANDER_DUMP_SIZE;
        ESP_LOGW(TAG, "File is %d bytes, saving first %d bytes",
                 data_len, SKYLANDER_DUMP_SIZE);
    } else if (data_len < SKYLANDER_DUMP_SIZE) {
        ESP_LOGE(TAG, "File too small: %d bytes (need %d)", data_len, SKYLANDER_DUMP_SIZE);
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"File too small — need 1024 bytes\"}");
        return ESP_OK;
    }

    /* Write to SPIFFS */
    char full[96];
    spiffs_full_path(filename, full, sizeof(full));
    bool ok = false;
    int loaded_slot = -1;
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    bool loaded = skylander_path_loaded_in_any_enabled_slot(full, &loaded_slot);
    FILE *f = loaded ? NULL : fopen(full, "wb");
    if (f) {
        /* Close the file even when fwrite fails, otherwise a failed upload
         * can leak a SPIFFS handle and make later library work unreliable. */
        size_t written = fwrite(dump_start, 1, dump_len, f);
        int close_rc = fclose(f);
        ok = written == (size_t)dump_len && close_rc == 0;
        if (ok) {
            ESP_LOGI(TAG, "Saved to %s", full);
            scan_files();
        } else {
            ESP_LOGE(TAG, "Write failed for %s", full);
        }
    } else if (!loaded) {
        ESP_LOGE(TAG, "Cannot create %s", full);
    }
    xSemaphoreGive(g_sky_mutex);

    free(body);
    httpd_resp_set_type(req, "application/json");
    if (loaded) {
        char message[96], response[128];
        loaded_file_message(loaded_slot, message, sizeof(message));
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", message);
        httpd_resp_sendstr(req, response);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Write failed\"}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * GET /api/download?slot=N
 * Returns the exact raw 1024-byte dump currently stored in SPIFFS.
 * ----------------------------------------------------------------------- */
static esp_err_t handle_download(httpd_req_t *req) {
    /* Parse slot from query string */
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    int slot = 0;
    char val[8] = {0};
    if (httpd_query_key_value(query, "slot", val, sizeof(val)) == ESP_OK)
        slot = atoi(val);

    if (slot < 0 || slot >= PORTAL_SLOT_ENABLED_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad slot");
        return ESP_OK;
    }

    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);

    if (!g_skylanders[slot].loaded) {
        xSemaphoreGive(g_sky_mutex);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Slot empty");
        return ESP_OK;
    }

    /* The SPIFFS file is the authoritative raw tag image.  Do not use the
     * decrypted metadata cache and do not encrypt/decrypt on export. */
    static uint8_t raw[SKYLANDER_DUMP_SIZE];
    FILE *f = fopen(g_skylanders[slot].filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Download cannot open %s", g_skylanders[slot].filename);
        xSemaphoreGive(g_sky_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot read dump");
        return ESP_FAIL;
    }
    size_t nread = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (nread != sizeof(raw)) {
        ESP_LOGE(TAG, "Download read %u/%u bytes from %s", (unsigned)nread,
                 SKYLANDER_DUMP_SIZE, g_skylanders[slot].filename);
        xSemaphoreGive(g_sky_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Dump is not 1024 bytes");
        return ESP_FAIL;
    }
    log_download_dump("Download SPIFFS read", raw);

    /* Build a download filename */
    const char *base = strrchr(g_skylanders[slot].filename, '/');
    base = base ? base+1 : g_skylanders[slot].filename;
    char cdispo[320];
    snprintf(cdispo, sizeof(cdispo), "attachment; filename=\"%s\"", base);

    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", cdispo);
    log_download_dump("Download HTTP body", raw);
    esp_err_t send_rc = httpd_resp_send(req, (const char *)raw, sizeof(raw));
    ESP_LOGI(TAG, "Download HTTP send: bytes=%u result=%s", SKYLANDER_DUMP_SIZE,
             esp_err_to_name(send_rc));
    return send_rc;
}

/* -----------------------------------------------------------------------
 * POST /api/delete  — body: {"file":"Spyro.bin"}
 * ----------------------------------------------------------------------- */
static esp_err_t handle_delete(httpd_req_t *req) {
    char body[128] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) httpd_req_recv(req, body, len);

    char file[64] = {0};
    char *pf = strstr(body, "\"file\"");
    if (pf) {
        pf = strchr(pf+6, '"');
        if (pf) { pf++; int fi=0; while(*pf&&*pf!='"'&&fi<63) file[fi++]=*pf++; }
    }

    bool ok = false;
    if (file[0]) {
        char full[96];
        spiffs_full_path(file, full, sizeof(full));
        int loaded_slot = -1;
        xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
        bool loaded = skylander_path_loaded_in_any_enabled_slot(full, &loaded_slot);
        if (!loaded) ok = (remove(full) == 0);
        if (ok) scan_files();
        xSemaphoreGive(g_sky_mutex);
        if (loaded) {
            char message[96];
            loaded_file_message(loaded_slot, message, sizeof(message));
            httpd_resp_set_type(req, "application/json");
            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", message);
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
        if (ok) {
            ESP_LOGI(TAG, "Deleted %s", full);
        } else {
            ESP_LOGE(TAG, "Failed to delete %s", full);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Delete failed\"}");
    return ESP_OK;
}
httpd_handle_t web_ui_start(void) {
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 26;
    config.stack_size       = 12288;
    /* Increase recv buffer for file uploads */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    static const httpd_uri_t uris[] = {
        { .uri="/",               .method=HTTP_GET,  .handler=handle_root       },
        { .uri="/api/state",      .method=HTTP_GET,  .handler=handle_state      },
        { .uri="/api/library",    .method=HTTP_GET,  .handler=handle_library    },
        { .uri="/api/library/catalog", .method=HTTP_GET, .handler=handle_library_catalog },
        { .uri="/api/library/favourite-keys", .method=HTTP_GET, .handler=handle_library_favourite_keys },
        { .uri="/api/library/favourites", .method=HTTP_GET, .handler=handle_library_favourites },
        { .uri="/api/library/editor", .method=HTTP_GET, .handler=handle_library_editor },
        { .uri="/api/library/editor/debug", .method=HTTP_GET, .handler=handle_library_editor },
        { .uri="/api/library/editor/gold", .method=HTTP_POST, .handler=handle_library_editor_gold },
        { .uri="/api/library/editor/level", .method=HTTP_POST, .handler=handle_library_editor_level },
        { .uri="/api/library/editor/restore", .method=HTTP_POST, .handler=handle_library_editor_restore },
        { .uri="/api/library/rebuild", .method=HTTP_POST, .handler=handle_library_rebuild },
        { .uri="/api/library/load", .method=HTTP_POST, .handler=handle_library_load },
        { .uri="/api/library/download", .method=HTTP_GET, .handler=handle_library_download },
        { .uri="/api/library/upload", .method=HTTP_POST, .handler=handle_library_upload },
        { .uri="/api/library/favourite", .method=HTTP_POST, .handler=handle_library_favourite },
        { .uri="/api/library/delete", .method=HTTP_DELETE, .handler=handle_library_delete },
        { .uri="/api/download",   .method=HTTP_GET,  .handler=handle_download   },
        { .uri="/api/load",       .method=HTTP_POST, .handler=handle_load       },
        { .uri="/api/unload",     .method=HTTP_POST, .handler=handle_unload     },
        { .uri="/api/sense",      .method=HTTP_POST, .handler=handle_sense      },
        { .uri="/api/portaltype", .method=HTTP_POST, .handler=handle_portaltype },
        { .uri="/api/upload",     .method=HTTP_POST, .handler=handle_upload     },
        { .uri="/api/delete",     .method=HTTP_POST, .handler=handle_delete     },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t register_result = httpd_register_uri_handler(server, &uris[i]);
        if (register_result != ESP_OK)
            ESP_LOGE(TAG, "Failed to register %s: %s", uris[i].uri,
                     esp_err_to_name(register_result));
    }

    ESP_LOGI(TAG, "HTTP server ready");
    return server;
}

void web_ui_stop(httpd_handle_t server) {
    if (server) httpd_stop(server);
}
