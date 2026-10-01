#include "web.h"
#include "ske02.h"
#include "debug.h"
#include "storage.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

static ESP8266WebServer sServer(80);
static bool sPortal;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static String jsonEsc(const char *s)
{
    String o;
    o.reserve(strlen(s) + 2);
    for (const char *p = s; *p; p++)
    {
        uint8_t c = (uint8_t)*p;
        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += (char)c;
        }
        else if (c < 0x20)
            o += ' '; /* control chars shouldn't appear, stay valid JSON */
        else
            o += (char)c; /* UTF-8 passes through unchanged */
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* WiFi setup portal                                                   */

static const char PAGE_WIFI[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SKE-02 Gateway - Wi-Fi</title>
<style>
body{font:14px/1.45 system-ui,sans-serif;background:#14171c;color:#d8dee9;max-width:520px;margin:24px auto;padding:0 14px}
h1{font-size:18px}label{display:block;margin:12px 0 4px;color:#8b94a7}
input{width:100%;box-sizing:border-box;background:#0e1116;color:#d8dee9;border:1px solid #39414f;border-radius:6px;padding:8px}
button{margin-top:14px;background:#2a5bdb;color:#fff;border:0;border-radius:6px;padding:9px 22px;cursor:pointer;font-size:15px}
table{width:100%;border-collapse:collapse;margin-top:6px}
td{padding:6px 8px;border-top:1px solid #2a3140}
.pick{background:#233047;color:#d8dee9;border:1px solid #39414f;border-radius:6px;padding:3px 10px;cursor:pointer}
.dbm{color:#8b94a7}
</style></head><body>
<h1>Настройка Wi-Fi шлюза SKE-02</h1>
<form method="POST" action="/save">
<label>SSID сети</label><input name="ssid" id="ssid" maxlength="32" autocapitalize="off" autocorrect="off" spellcheck="false" required>
<label>Пароль (для открытой сети оставьте пустым)</label><input name="pass" type="password" maxlength="64" autocapitalize="off" autocorrect="off">
<button type="submit">Подключиться</button>
</form>
<h3>Найденные сети</h3><div id="nets">сканирование…</div>
<script>
let NETS=[];
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function load(restart){
 if(restart)await fetch('/scan?restart=1');
 const r=await (await fetch('/scan')).json();
 if(r.running){setTimeout(()=>load(0),1500);return}
 NETS=r.nets;
 let h='<table>';
 NETS.forEach((n,i)=>{
  h+='<tr><td>'+esc(n.s)+'</td><td class="dbm">'+n.r+' dBm</td><td>'+
     (n.e?'🔒':'')+'</td><td><button class="pick" onclick="pick('+i+')">выбрать</button></td></tr>';
 });
 document.getElementById('nets').innerHTML=h+'</table>';
}
function pick(i){document.getElementById('ssid').value=NETS[i].s}
load(1);
</script></body></html>)HTML";

static const char PAGE_CSS[] PROGMEM =
    "body{font:14px system-ui,sans-serif;background:#14171c;color:#d8dee9;"
    "max-width:520px;margin:24px auto;padding:0 14px}a{color:#7aa2f7}";

static String htmlEsc(const char *s)
{
    String o;
    for (; *s; s++)
    {
        if (*s == '&') o += F("&amp;");
        else if (*s == '<') o += F("&lt;");
        else if (*s == '>') o += F("&gt;");
        else if (*s == '"') o += F("&quot;");
        else o += *s;
    }
    return o;
}

static void handleCaptive(void)
{
    sServer.sendHeader("Location", "http://10.0.0.1/", true);
    sServer.send(302, "text/plain", "");
}

static void handleWifiPage(void)
{
    sServer.send_P(200, "text/html", PAGE_WIFI);
}

static void handleScan(void)
{
    if (sServer.hasArg("restart"))
    {
        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING)
            WiFi.scanDelete(); /* never delete a scan that is still running */
        WiFi.scanNetworks(true);
        sServer.send(202, "application/json", F("{\"running\":true}"));
        return;
    }
    int8_t r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING)
    {
        sServer.send(202, "application/json", F("{\"running\":true}"));
        return;
    }
    String out = F("{\"nets\":[");
    for (int8_t i = 0; i < r; i++)
    {
        if (i)
            out += ',';
        out += F("{\"s\":\"");
        out += jsonEsc(WiFi.SSID(i).c_str());
        out += F("\",\"r\":");
        out += String(WiFi.RSSI(i));
        out += F(",\"e\":");
        out += (WiFi.encryptionType(i) != ENC_TYPE_NONE) ? '1' : '0';
        out += '}';
    }
    out += F("]}");
    sServer.send(200, "application/json", out);
}

static void handleSave(void)
{
    String ssid = sServer.arg("ssid");
    String pass = sServer.arg("pass");
    ssid.trim();
    if (ssid.length() < 1 || ssid.length() > 32 || pass.length() > 64)
    {
        sServer.send(400, "text/plain", F("bad ssid/pass"));
        return;
    }
    storageSetWifi(ssid.c_str(), pass.c_str());
    storageSave();
    DBG("web: creds for \"%s\" saved, joining\n", ssid.c_str());

    /* Try the network while the portal AP is still up (WIFI_AP_STA), so
       the user sees the real result instead of a blind reboot. */
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000)
        delay(100);

    String head = F("<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">");
    if (WiFi.status() == WL_CONNECTED)
    {
        String ip = WiFi.localIP().toString();
        String page = head;
        page += F("<title>OK</title><meta http-equiv=\"refresh\" content=\"12;url=http://");
        page += ip;
        page += F("/\"><style>");
        page += PAGE_CSS;
        page += F("</style></head><body><h2>Подключено к &laquo;");
        page += htmlEsc(ssid.c_str());
        page += F("&raquo;</h2><p>Шлюз перезагружается. Через ~10 секунд он будет доступен "
                  "по адресу <b>http://");
        page += ip;
        page += F("/</b> (или http://ske02-gw.local/).</p></body></html>");
        sServer.send(200, "text/html", page);
        delay(1500); /* let the response reach the browser */
        ESP.restart();
    }

    /* Failed: drop the STA attempt, keep the portal running */
    WiFi.mode(WIFI_AP);
    String page = head;
    page += F("<title>Ошибка</title><style>");
    page += PAGE_CSS;
    page += F("</style></head><body><h2>Не удалось подключиться</h2>"
              "<p>Сеть &laquo;");
    page += htmlEsc(ssid.c_str());
    page += F("&raquo; не ответила за 15 секунд. Проверьте:</p>"
              "<ul><li>имя сети и пароль — <b>регистр букв важен</b>;</li>"
              "<li>сеть должна быть 2.4 ГГц (ESP8266 не видит 5 ГГц);</li>"
              "<li>роутер разрешает новые подключения (нет фильтра MAC).</li></ul>"
              "<p><a href=\"/wifi\">Вернуться к настройке</a></p></body></html>");
    sServer.send(200, "text/html", page);
    DBG("web: join failed, portal keeps running\n");
}

/* ------------------------------------------------------------------ */
/* dashboard + JSON API                                                */

static const char PAGE_INDEX[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SKE-02 Gateway</title>
<style>
body{font:14px/1.45 system-ui,sans-serif;background:#14171c;color:#d8dee9;margin:0;padding:12px}
header{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
h1{font-size:18px;margin:0 8px 0 0}
.tag{background:#2a3140;border-radius:6px;padding:2px 8px;font-size:12px;white-space:nowrap}
.ok{background:#1d5c2f}.bad{background:#6c2431}
button{background:#2a5bdb;color:#fff;border:0;border-radius:6px;padding:5px 12px;cursor:pointer;font-size:13px}
button.warn{background:#a23b2e}
input,select{background:#0e1116;color:#d8dee9;border:1px solid #39414f;border-radius:6px;padding:4px 8px;box-sizing:border-box}
#filter{width:200px}
#bar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:10px}
#nav{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}
#nav button{background:#2a3140;font-size:14px;padding:8px 18px}
#nav button.act{background:#2a5bdb}
.tabs{display:flex;flex-wrap:wrap;gap:4px;border-bottom:2px solid #2a3140;margin-bottom:8px}
.tabs button{background:transparent;color:#8b94a7;border:0;border-bottom:2px solid transparent;border-radius:6px 6px 0 0;padding:7px 14px;cursor:pointer;font-size:14px}
.tabs button.act{color:#d8dee9;background:#1b202a;border-bottom-color:#2a5bdb}
.subtabs{display:flex;flex-wrap:wrap;gap:4px;margin:0 0 8px 8px}
.subtabs button{background:#232a38;color:#8b94a7;border:0;border-radius:10px;padding:3px 12px;cursor:pointer;font-size:12px}
.subtabs button.act{background:#1d5c2f;color:#d8dee9}
details{background:#1b202a;border:1px solid #2a3140;border-radius:8px;margin-bottom:8px}
summary{padding:8px 12px;cursor:pointer;font-weight:600}
table{width:100%;border-collapse:collapse}
td,th{padding:5px 10px;border-top:1px solid #2a3140;text-align:left;vertical-align:middle}
th{color:#8b94a7;font-weight:500;font-size:12px}
.val{font-family:ui-monospace,Consolas,monospace;white-space:nowrap}
.stale{color:#8b94a7}
.ctl{text-align:right;vertical-align:middle}
.where{color:#8b94a7;font-size:11px;margin-top:1px}
input.fnum{width:130px;text-align:right}
.set{display:flex;gap:6px;align-items:center;flex-wrap:wrap;justify-content:flex-end}
.set input[type=number],.set input[type=text]{width:110px}
.set input.fnum{width:130px}
.set select{max-width:170px}
.t2{width:56px!important;text-align:center}
.dv{color:#8b94a7}
</style></head><body>
<header>
 <h1>SKE-02 Gateway</h1>
 <span id="link" class="tag">…</span><span id="fw" class="tag"></span>
 <button onclick="rescan()">Пересканировать</button>
 <button class="warn" onclick="rebootDev()">Перезагрузить прибор</button>
 <a href="/wifi"><button>Wi-Fi</button></a>
</header>
<div id="nav"></div>
<div id="tabs"></div>
<div id="subtabs"></div>
<div id="bar">
 <input id="filter" placeholder="фильтр по имени или id" oninput="applyFilter()">
 <span class="tag" id="ip"></span><span class="tag" id="rss"></span>
 <span class="tag" id="upt"></span><span class="tag" id="heap"></span>
 <span class="tag">опрос <input id="poll" type="number" min="1" max="255" style="width:64px">×100 мс</span>
 <button onclick="setPoll()">OK</button>
</div>
<div id="sections"><p>Опрос прибора…</p></div>
<script>
const COMMON='\x01';              /* pseudo tab for params directly in the section */
let P=[],SECT=[],built='',cur=0,curT={},curS={};
const $=i=>document.getElementById(i);
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const p2=v=>String(v).padStart(2,'0');
function hms(t){const s=t%60,m=(t/60|0)%60,h=t/3600|0;return h?h+'ч '+m+'м':m?m+'м '+s+'с':s+'с'}
async function tick(){
 try{
  const r=await (await fetch('/api/info')).json();
  $('link').textContent=r.link?'UART: связь есть':'UART: нет связи';
  $('link').className='tag '+(r.link?'ok':'bad');
  $('fw').textContent=r.fw+' · параметров: '+r.count;
  $('ip').textContent=r.ip;
  $('rss').textContent=r.ap?r.rssi+' dBm':r.ssid+': '+r.rssi+' dBm';
  $('upt').textContent='uptime '+hms(r.uptime);$('heap').textContent='heap '+(r.heap/1024|0)+' KiB';
  if(document.activeElement!==$('poll'))$('poll').value=Math.round(r.poll/100);
  const d=await (await fetch('/api/params')).json();
  const fp=d.count+'|'+d.params.length+'|'+d.sections.length+'|'+d.rc;
  if(fp!==built){P=d.params;SECT=d.sections;render(fp);applyFilter()}
  else for(const q of d.params){updVal(q)}
 }catch(e){}
}
/* value refresh of the editor (never steals focus) */
function updVal(q){
 const e=$('v'+q.i);if(e){e.textContent=q.v;e.className='val'+(q.a>90?' stale':'')}
 if(!q.w)return; /* read-only: the span above is the whole value */
 const t=q.t,g=x=>$(x+q.i);
 if((t===10||t===11)&&g('w')&&g('w')!==document.activeElement)g('w').value=q.r;
 else if(t!==2&&t!==3&&g('w')&&g('w')!==document.activeElement)g('w').value=q.v;
 else if(t===2&&g('wh')){const m=q.v.split(':');if(g('wh')!==document.activeElement)g('wh').value=m[0];if(g('wm')!==document.activeElement)g('wm').value=m[1]}
 else if(t===3&&g('wd')){const m=q.v.split('.');if(g('wd')!==document.activeElement)g('wd').value=m[0];if(g('wm2')!==document.activeElement)g('wm2').value=m[1];if(g('wy')!==document.activeElement)g('wy').value=m[2]}
}
/* edit widget html for the parameter type */
function widget(q){
 if(!q.w)return '';
 const i=q.i,ok='<button onclick="setParam('+i+')">✓</button>';
 if((q.t===10||q.t===11)&&q.o&&q.o.length){
  let h='<select id="w'+i+'">';
  q.o.forEach((s,k)=>h+='<option value="'+k+'"'+(String(k)===String(q.r)?' selected':'')+'>'+esc(s)+'</option>');
  return '<div class="set">'+h+'</select>'+ok+'</div>';
 }
 if(q.t===0)return '<div class="set"><input id="w'+i+'" type="text" class="fnum" inputmode="decimal" maxlength="12" title="'+esc((q.lo||'?')+' .. '+(q.hi||'?'))+'" value="'+esc(q.v)+'">'+ok+'</div>';
 if(q.t===1)return '<div class="set"><input id="w'+i+'" type="text" maxlength="6" inputmode="numeric" title="'+esc((q.lo||'?')+' .. '+(q.hi||'?'))+'" value="'+esc(q.v)+'">'+ok+'</div>';
 if(q.t===2||q.t===3){
  const m=q.v.split(/[:.]/);
  if(q.t===2)return '<div class="set"><input class="t2" id="wh'+i+'" type="number" min="0" max="23" step="1" value="'+esc(m[0])+'"><span class="dv">:</span><input class="t2" id="wm'+i+'" type="number" min="0" max="59" step="1" value="'+esc(m[1])+'">'+ok+'</div>';
  return '<div class="set"><input class="t2" id="wd'+i+'" type="number" min="1" max="31" step="1" value="'+esc(m[0])+'"><span class="dv">.</span><input class="t2" id="wm2'+i+'" type="number" min="1" max="12" step="1" value="'+esc(m[1])+'"><span class="dv">.</span><input class="t2" id="wy'+i+'" type="number" min="0" max="99" step="1" value="'+esc(m[2])+'">'+ok+'</div>';
 }
 const st=' min="'+(q.lo!==''?esc(q.lo):'0')+'" max="'+(q.hi!==''?esc(q.hi):'65535')+'"';
 return '<div class="set"><input id="w'+i+'" type="number" step="1"'+st+' value="'+esc(q.v)+'">'+ok+'</div>';
}
/* one table row: name | editor (read-only values render as plain text) */
function row(q,where){
 const ctrl=q.w?widget(q):'<span class="val'+(q.a>90?' stale':'')+'" id="v'+q.i+'">'+esc(q.v)+'</span>';
 return '<tr data-n="'+esc((q.n+' '+q.i).toLowerCase())+'"><td>'+esc(q.n||('P'+q.i))+
  (where?'<div class="where">'+esc(where)+'</div>':'')+'</td><td class="ctl">'+ctrl+'</td></tr>';
}
const THEAD='<tr><th>Параметр</th><th></th></tr>';
/*
 * The server names the owning L2 menu explicitly ("tb"), because the
 * firmware prints subtrees before the parent's own params and the id
 * order alone cannot recover the hierarchy. L3 = the nearest submenu
 * when its level is exactly 3.
 */
function decorate(arr){
 const rows=[];
 for(const q of arr){
  rows.push({q,l2:q.tb||COMMON,l3:(q.gl===3&&q.g)?q.g:''});
 }
 return rows;
}
let curTab=COMMON;                 /* active L2 tab name of the section */
function render(fp){
 if(!P.length){$('tabs').innerHTML='';$('subtabs').innerHTML='';$('sections').innerHTML='<p>Прибор не найден: проверьте подключение UART и питание, затем нажмите «Пересканировать».</p>';built=fp;return}
 let h='';
 SECT.forEach((s,k)=>h+='<button class="'+(k===cur?'act':'')+'" onclick="go('+k+')">'+esc(s)+'</button>');
 $('nav').innerHTML=h;
 if(cur>=SECT.length)cur=0;
 const rows=decorate(P.filter(q=>q.s===cur||SECT.length<=1));
 const tabs=[...new Set(rows.map(r=>r.l2))];
 if(!(cur in curT)||curT[cur]>=tabs.length)curT[cur]=0;
 const t=tabs[curT[cur]];
 curTab=t;
 const tr=rows.filter(r=>r.l2===t);
 const subs=[...new Set(tr.map(r=>r.l3))];
 const sk=cur+'|'+t;
 if(!(sk in curS)||curS[sk]>=subs.length)curS[sk]=0;
 const s=subs[curS[sk]];
 h='';
 tabs.forEach((n,k)=>h+='<button class="'+(n===t?'act':'')+'" onclick="go2('+k+')">'+esc(n===COMMON?'Общие':n)+'</button>');
 $('tabs').innerHTML='<div class="tabs">'+h+'</div>';
 h='';
 if(subs.length>1||subs[0]!=='')
  subs.forEach((n,k)=>h+='<button class="'+(n===s?'act':'')+'" onclick="go3('+k+')">'+esc(n||'Общие')+'</button>');
 $('subtabs').innerHTML=h?'<div class="subtabs">'+h+'</div>':'';
 h='<table>'+THEAD;
 for(const r of tr.filter(r=>r.l3===s))
  h+=row(r.q);
 h+='</table>';
 $('sections').innerHTML=h;built=fp;
}
/* search view: filter matches anywhere, across all sections and tabs */
function renderSearch(f){
 let h='<table>'+THEAD;
 for(const q of P){
  if(!(q.n+' '+q.i).toLowerCase().includes(f))continue;
  const sec=q.s>=0&&q.s<SECT.length?SECT[q.s]:'';
  h+=row(q,sec?(sec+(q.g?' · '+q.g:'')):'');
 }
 h+='</table>';
 $('sections').innerHTML=h||'<p>Ничего не найдено.</p>';
 $('tabs').innerHTML='';$('subtabs').innerHTML='';
}
function go(k){cur=k;applyFilter()}
function go2(k){curT[cur]=k;applyFilter()}
function go3(k){curS[cur+'|'+curTab]=k;applyFilter()}
function applyFilter(){
 const f=$('filter').value.trim().toLowerCase();
 if(!f){render(built);return}
 renderSearch(f);
}
function curVal(q){
 const i=q.i,g=x=>$(x+i);
 if(q.t===10||q.t===11)return g('w').value;
 if(q.t===2)return p2(g('wh').value)+':'+p2(g('wm').value);
 if(q.t===3)return p2(g('wd').value)+'.'+p2(g('wm2').value)+'.'+p2(g('wy').value);
 return g('w').value;
}
async function setParam(i){
 const q=P.find(x=>x.i===i);if(!q)return;
 try{
  const r=await (await fetch('/api/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:'id='+i+'&v='+encodeURIComponent(curVal(q))})).json();
  if(!r.ok){alert('Ошибка: '+r.error);return}
  updVal(Object.assign({},q,{v:r.value,a:0}));
  if(q.t===10||q.t===11){const s=$('w'+i);if(s)[...s.options].forEach(o=>{if(o.text===r.value)s.value=o.value})}
 }catch(e){alert('нет ответа от шлюза')}
}
async function rescan(){await fetch('/api/rescan',{method:'POST'});built='';tick()}
function rebootDev(){if(confirm('Перезагрузить прибор?'))fetch('/api/reboot',{method:'POST'})}
async function setPoll(){await fetch('/api/setpoll?poll='+$('poll').value,{method:'POST'});tick()}
setInterval(tick,2000);tick();
</script></body></html>)HTML";

static void handleRoot(void)
{
    if (sPortal)
        handleWifiPage();
    else
        sServer.send_P(200, "text/html", PAGE_INDEX);
}

static void handleApiInfo(void)
{
    bool sta = (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED;
    String out = F("{\"fw\":\"");
    out += jsonEsc(skeVersion());
    out += F("\",\"count\":");
    out += String(skeCount());
    out += F(",\"link\":");
    out += skeLinkUp() ? '1' : '0';
    out += F(",\"ready\":");
    out += skeReady() ? '1' : '0';
    out += F(",\"ap\":");
    out += sPortal ? '1' : '0';
    out += F(",\"ssid\":\"");
    out += jsonEsc(sta ? WiFi.SSID().c_str() : "");
    out += F("\",\"ip\":\"");
    out += sPortal ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    out += F("\",\"rssi\":");
    out += String(sPortal ? 0 : WiFi.RSSI());
    out += F(",\"uptime\":");
    out += String(millis() / 1000UL);
    out += F(",\"heap\":");
    out += String(ESP.getFreeHeap());
    out += F(",\"poll\":");
    out += String((uint32_t)storagePoll100ms() * 100UL);
    out += '}';
    sServer.send(200, "application/json", out);
}

static void handleApiParams(void)
{
    sServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    sServer.send(200, "application/json");
    String out;
    out.reserve(1600);
    out = F("{\"count\":");
    out += String(skeCount());
    out += F(",\"sections\":[");
    for (uint16_t s = 0; s < skeSectionCount(); s++)
    {
        if (s)
            out += ',';
        out += F("\"");
        out += jsonEsc(skeSectionName((uint8_t)s));
        out += F("\"");
    }
    out += F("],\"params\":[");
    bool first = true;
    uint16_t rc = 0;
    for (uint16_t id = 0; id < skeCount(); id++)
    {
        SkeParam *p = skeGet(id);
        if (!p || (!p->present && !p->name[0]))
            continue;
        uint8_t sz = skeTypeSize(p->type);
        if ((p->minv != 0 || p->maxv != 0) || p->optCnt > 0)
            rc++;
        uint32_t age = p->updated ? (millis() - p->updated) / 1000UL : 0xFFFF;
        if (age > 0xFFFF)
            age = 0xFFFF;
        if (!first)
            out += ',';
        first = false;
        out += F("{\"i\":");
        out += String(id);
        out += F(",\"t\":");
        out += String(p->type);
        out += F(",\"r\":");
        out += String(p->value); /* raw u32: enum/bool index, spin value */
        out += F(",\"n\":\"");
        out += jsonEsc(p->name);
        out += F("\",\"s\":");
        out += String((int)skeSectionIndexOf(p->section)); /* -1 = none */
        out += F(",\"g\":\"");
        out += jsonEsc(skeGroupName(p->group));
        out += F("\",\"gl\":");
        out += String(p->groupLvl);
        out += F(",\"tb\":\"");
        out += jsonEsc(p->tab2 ? skeGroupName(p->tab2) : "");
        out += F("\",\"v\":\"");
        out += jsonEsc(skeValueText(id).c_str());
        out += F("\",\"w\":");
        out += (p->present && !p->readOnly && sz > 0) ? '1' : '0';
        out += F(",\"lo\":\"");
        out += jsonEsc(skeBoundText(p, false).c_str());
        out += F("\",\"hi\":\"");
        out += jsonEsc(skeBoundText(p, true).c_str());
        out += F("\",\"o\":[");
        for (uint8_t k = 0; k < skeOptCount(p); k++)
        {
            if (k)
                out += ',';
            out += F("\"");
            out += jsonEsc(skeOptText(p, k) ? skeOptText(p, k) : "");
            out += F("\"");
        }
        out += F("],\"a\":");
        out += String(age);
        out += '}';
        if (out.length() > 1400)
        {
            sServer.sendContent(out);
            out = "";
        }
    }
    out += F("],\"rc\":");
    out += String(rc);
    out += '}';
    sServer.sendContent(out);
    sServer.sendContent(""); /* end of chunked body */
}

static void handleApiSet(void)
{
    String ids = sServer.arg("id");
    String v = sServer.arg("v");
    long id = strtol(ids.c_str(), NULL, 10);
    if (id < 0 || id >= SKE_MAX_PARAMS)
    {
        sServer.send(400, "application/json", F("{\"ok\":false,\"error\":\"bad id\"}"));
        return;
    }
    String err;
    if (skeSetFromText((uint16_t)id, v.c_str(), err))
    {
        String out = F("{\"ok\":true,\"value\":\"");
        out += jsonEsc(skeValueText((uint16_t)id).c_str());
        out += F("\"}");
        sServer.send(200, "application/json", out);
    }
    else
    {
        String out = F("{\"ok\":false,\"error\":\"");
        out += jsonEsc(err.c_str());
        out += F("\"}");
        sServer.send(400, "application/json", out);
    }
}

static void handleApiRescan(void)
{
    skeRescan();
    sServer.send(200, "application/json", F("{\"ok\":true}"));
}

static void handleApiReboot(void)
{
    skeRebootDevice();
    sServer.send(200, "application/json", F("{\"ok\":true}"));
}

static void handleApiSetPoll(void)
{
    long v = sServer.arg("poll").toInt();
    if (v < 1 || v > 255)
    {
        sServer.send(400, "application/json", F("{\"ok\":false,\"error\":\"1..255\"}"));
        return;
    }
    storageSetPoll100ms((uint8_t)v);
    storageSave();
    sServer.send(200, "application/json", F("{\"ok\":true}"));
}

/* ------------------------------------------------------------------ */

void webSetup(bool portalMode)
{
    sPortal = portalMode;

    sServer.on("/", HTTP_GET, handleRoot);
    sServer.on("/wifi", HTTP_GET, handleWifiPage);
    sServer.on("/scan", HTTP_GET, handleScan);
    sServer.on("/save", HTTP_POST, handleSave);

    /* the API works in both modes (portal included) for bench testing */
    sServer.on("/api/info", HTTP_GET, handleApiInfo);
    sServer.on("/api/params", HTTP_GET, handleApiParams);
    sServer.on("/api/set", HTTP_POST, handleApiSet);
    sServer.on("/api/rescan", HTTP_POST, handleApiRescan);
    sServer.on("/api/reboot", HTTP_POST, handleApiReboot);
    sServer.on("/api/setpoll", HTTP_POST, handleApiSetPoll);

    if (!portalMode)
    {
        sServer.onNotFound([]() {
            sServer.send(404, "text/plain", F("not found"));
        });
    }
    else
    {
        /* captive portal: everything unknown redirects to the setup page */
        sServer.onNotFound(handleCaptive);
        WiFi.scanNetworks(true);
    }

    sServer.begin();
    DBG("web: http server started (%s mode)\n", portalMode ? "portal" : "sta");
}

void webLoop(void)
{
    sServer.handleClient();
}
