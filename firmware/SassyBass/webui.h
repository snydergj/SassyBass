// =====================================================================
//  Billy's web page (on the home network): http://billybass.local
//  Personas (traits, backstory, voice, style), memory, volume, logs, backup.
// =====================================================================
#pragma once
#include <WebServer.h>
#include <ArduinoJson.h>
#include "persona.h"
#include "logtee.h"

extern WebServer web;
extern int volume;
void setVolume(int v);
void testMotor(const String &which);

static const char WEB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SassyBass</title>
<style>
:root{--bg:#0d2117;--card:#143223;--ink:#eaf5ee;--mute:#9fc2ad;--acc:#45d17a;--line:#2b5a40;--warn:#f2c14e}
*{box-sizing:border-box}body{margin:0;font-family:-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--ink)}
header{padding:16px 18px 6px;display:flex;align-items:center;gap:10px}header h1{margin:0;font-size:1.5em}
#status{color:var(--mute);font-size:.85em;margin-left:auto;text-align:right}
nav{display:flex;gap:6px;padding:8px 14px;position:sticky;top:0;background:var(--bg);z-index:2;border-bottom:1px solid var(--line)}
nav button{flex:1;background:none;border:0;color:var(--mute);padding:10px;font-size:1em;border-radius:8px;cursor:pointer}
nav button.on{background:var(--card);color:var(--ink);font-weight:600}
main{max-width:720px;margin:0 auto;padding:10px 14px 60px}
.card{background:var(--card);border-radius:12px;padding:14px 16px;margin:12px 0}
h2{font-size:1.05em;margin:0 0 10px;color:var(--acc)}
label{display:block;font-size:.85em;color:var(--mute);margin:10px 0 4px}
input,select,textarea{width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:#0b1c13;color:var(--ink);font-size:1em}
textarea{min-height:110px;resize:vertical}
.row{display:flex;gap:8px;align-items:center}.row>*{flex:1}
button.b{background:var(--acc);color:#062b14;border:0;border-radius:9px;padding:11px 14px;font-weight:700;font-size:.95em;cursor:pointer}
button.s{background:#23463a;color:var(--ink);border:0;border-radius:9px;padding:10px 12px;font-size:.9em;cursor:pointer}
button.d{background:#5a2323;color:#fdd;border:0;border-radius:9px;padding:10px 12px;cursor:pointer}
.trait{display:grid;grid-template-columns:96px 1fr 38px;gap:10px;align-items:center;margin:8px 0}
.trait span{font-size:.9em}.trait b{text-align:right;font-variant-numeric:tabular-nums}
input[type=range]{padding:0;accent-color:var(--acc)}
.pill{display:inline-block;font-size:.75em;background:var(--acc);color:#062b14;border-radius:20px;padding:2px 8px;margin-left:6px;vertical-align:middle}
.fact{display:flex;gap:8px;align-items:center;border-bottom:1px solid var(--line);padding:8px 0}.fact div{flex:1}
pre{background:#071510;padding:10px;border-radius:8px;white-space:pre-wrap;font-size:.78em;max-height:60vh;overflow:auto}
.hint{color:var(--mute);font-size:.8em;margin-top:4px}
#toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--acc);color:#062b14;padding:10px 16px;border-radius:9px;font-weight:600;display:none}
.tab{display:none}.tab.on{display:block}
</style></head><body>
<header><h1>&#x1F41F; SassyBass</h1><div id="status"></div></header>
<nav><button data-t="persona" class="on">Persona</button><button data-t="memory">Memory</button><button data-t="billy">Billy</button><button data-t="logs">Logs</button></nav>
<main>
<section id="persona" class="tab on">
 <div class="card"><div class="row"><select id="psel"></select></div>
  <div class="row" style="margin-top:8px"><button class="b" id="mkactive">Use this persona</button><button class="s" id="pnew">New</button><button class="s" id="pdup">Duplicate</button><button class="d" id="pdel">Delete</button></div>
  <div class="hint">Changes apply at Billy's next chat (or right away if he's chatting).</div></div>
 <div class="card"><h2>Identity</h2>
  <label>Name</label><input id="pname">
  <label>Voice</label><select id="pvoice"></select>
  <div class="hint">Listen to every voice in Google AI Studio's Stream page.</div>
  <label>Voice, accent &amp; style</label><textarea id="pinstr" placeholder="e.g. Thick Cajun accent, slow and sly. Loves crawfish."></textarea></div>
 <div class="card"><h2>Personality</h2><div id="traits"></div></div>
 <div class="card"><h2>Backstory</h2>
  <label>Where he's from</label><input id="b_origin">
  <label>Species</label><input id="b_species">
  <label>How he was caught</label><input id="b_caught">
  <label>Original purpose</label><input id="b_purpose">
  <label>How he became self-aware</label><input id="b_awakening"></div>
 <button class="b" id="psave" style="width:100%">Save persona</button>
</section>
<section id="memory" class="tab">
 <div class="card"><h2>What Billy remembers</h2>
  <div class="hint">Billy adds to this when you tell him things ("Billy, remember my dog is named Rex"). Used in every chat.</div>
  <div id="facts"></div>
  <div class="row" style="margin-top:10px"><input id="newfact" placeholder="Add a fact..."><button class="s" id="addfact" style="flex:0 0 auto">Add</button></div></div>
</section>
<section id="billy" class="tab">
 <div class="card"><h2>Volume</h2><div class="trait"><span>Volume</span><input type="range" id="vol" min="0" max="100"><b id="volv"></b></div></div>
 <div class="card"><h2>Motor test</h2><div class="row"><button class="s" data-m="mouth">Mouth</button><button class="s" data-m="head">Head</button><button class="s" data-m="tail">Tail</button></div></div>
 <div class="card"><h2>Backup</h2><div class="row"><button class="s" id="export">Download backup</button><button class="s" id="importb">Restore backup</button></div>
  <input type="file" id="importf" accept=".json" style="display:none">
  <div class="row" style="margin-top:8px"><button class="d" id="presets">Reset personas to presets</button></div></div>
 <div class="card"><h2>System</h2><div class="hint" id="sysinfo"></div>
  <div class="row" style="margin-top:8px"><button class="s" id="restart">Restart Billy</button><a href="/diag" class="s" style="text-align:center;padding:10px;border-radius:9px;background:#23463a;color:var(--ink);text-decoration:none">Audio diagnostics</a></div></div>
</section>
<section id="logs" class="tab"><div class="card"><h2>Live log</h2><pre id="log">Loading...</pre></div></section>
</main><div id="toast"></div>
<script>
const VOICES=[["Algenib","male, gravelly"],["Charon","male, informative"],["Puck","male, upbeat"],["Fenrir","male, excitable"],["Orus","male, firm"],["Enceladus","male, breathy"],["Iapetus","male, clear"],["Umbriel","male, easy-going"],["Algieba","male, smooth"],["Rasalgethi","male, informative"],["Alnilam","male, firm"],["Schedar","male, even"],["Achird","male, friendly"],["Zubenelgenubi","male, casual"],["Sadachbia","male, lively"],["Sadaltager","male, knowledgeable"],["Zephyr","female, bright"],["Kore","female, firm"],["Leda","female, youthful"],["Aoede","female, breezy"],["Callirrhoe","female, easy-going"],["Autonoe","female, bright"],["Despina","female, smooth"],["Erinome","female, clear"],["Laomedeia","female, upbeat"],["Achernar","female, soft"],["Gacrux","female, mature"],["Pulcherrima","female, forward"],["Vindemiatrix","female, gentle"],["Sulafat","female, warm"]];
const TRAITS=["humor","sarcasm","honesty","confidence","warmth","curiosity","verbosity","formality"];
const $=s=>document.querySelector(s);let S=null,cur=null;
function toast(t){const e=$("#toast");e.textContent=t;e.style.display="block";setTimeout(()=>e.style.display="none",1800)}
async function api(p,b){const r=await fetch(p,b===undefined?{}:{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(b)});if(!r.ok)throw new Error(await r.text());const ct=r.headers.get("content-type")||"";return ct.includes("json")?r.json():r.text()}
document.querySelectorAll("nav button").forEach(b=>b.onclick=()=>{document.querySelectorAll("nav button,.tab").forEach(e=>e.classList.remove("on"));b.classList.add("on");$("#"+b.dataset.t).classList.add("on");if(b.dataset.t=="logs")loadLog()});
$("#pvoice").innerHTML=VOICES.map(v=>`<option value="${v[0]}">${v[0]} (${v[1]})</option>`).join("");
$("#traits").innerHTML=TRAITS.map(t=>`<div class="trait"><span>${t[0].toUpperCase()+t.slice(1)}</span><input type="range" min="0" max="100" id="t_${t}" oninput="this.nextElementSibling.textContent=this.value"><b></b></div>`).join("");
async function load(){S=await api("/api/state");$("#status").innerHTML=`v${S.version}<br>${S.state}`;renderSel(cur||S.active);renderFacts();$("#vol").value=S.volume;$("#volv").textContent=S.volume;$("#sysinfo").textContent=`Firmware ${S.version} · ${S.ip} · free RAM ${S.heap} · ${S.personas.length} personas · ${S.facts.length} memories`}
function renderSel(id){const ps=S.personas;$("#psel").innerHTML=ps.map(p=>`<option value="${p.id}">${p.name}${p.id==S.active?" ★ active":""}</option>`).join("");if(!ps.find(p=>p.id==id))id=S.active;$("#psel").value=id;show(id)}
function show(id){cur=id;const p=S.personas.find(x=>x.id==id);if(!p)return;$("#pname").value=p.name||"";$("#pvoice").value=p.voice||"Algenib";$("#pinstr").value=p.instructions||"";const t=p.traits||{};TRAITS.forEach(k=>{const e=$("#t_"+k);e.value=t[k]??50;e.nextElementSibling.textContent=e.value});const b=p.backstory||{};["origin","species","caught","purpose","awakening"].forEach(k=>$("#b_"+k).value=b[k]||"")}
function collect(){const t={};TRAITS.forEach(k=>t[k]=+$("#t_"+k).value);const b={};["origin","species","caught","purpose","awakening"].forEach(k=>b[k]=$("#b_"+k).value);return{id:cur,name:$("#pname").value.trim()||"Billy",voice:$("#pvoice").value,instructions:$("#pinstr").value,traits:t,backstory:b}}
$("#psel").onchange=e=>show(e.target.value);
$("#psave").onclick=async()=>{const r=await api("/api/persona",collect());cur=r.id;await load();toast("Saved")};
$("#mkactive").onclick=async()=>{await api("/api/persona",collect());await api("/api/active",{id:cur});await load();toast("Billy will use this persona")};
$("#pnew").onclick=async()=>{const r=await api("/api/persona",{id:"",name:"New Fish",voice:"Puck",instructions:"",traits:Object.fromEntries(TRAITS.map(t=>[t,50])),backstory:{}});cur=r.id;await load();toast("Created")};
$("#pdup").onclick=async()=>{const p=collect();p.id="";p.name+=" copy";const r=await api("/api/persona",p);cur=r.id;await load();toast("Duplicated")};
$("#pdel").onclick=async()=>{if(S.personas.length<2)return toast("Keep at least one");if(!confirm("Delete this persona?"))return;await api("/api/persona/delete",{id:cur});cur=null;await load();toast("Deleted")};
function renderFacts(){$("#facts").innerHTML=S.facts.length?S.facts.map((f,i)=>`<div class="fact"><div>${f.replace(/</g,"&lt;")}</div><button class="d" onclick="delFact(${i})">&#x2715;</button></div>`).join(""):'<div class="hint" style="padding:10px 0">Nothing yet.</div>'}
async function saveFacts(){await api("/api/memory",{facts:S.facts});await load()}
window.delFact=async i=>{S.facts.splice(i,1);await saveFacts();toast("Forgotten")};
$("#addfact").onclick=async()=>{const v=$("#newfact").value.trim();if(!v)return;S.facts.push(v);$("#newfact").value="";await saveFacts();toast("Remembered")};
$("#vol").oninput=e=>$("#volv").textContent=e.target.value;$("#vol").onchange=e=>api("/api/volume",{v:+e.target.value}).then(()=>toast("Volume set"));
document.querySelectorAll("[data-m]").forEach(b=>b.onclick=()=>fetch("/api/test?m="+b.dataset.m));
$("#export").onclick=()=>location.href="/api/export";
$("#importb").onclick=()=>$("#importf").click();
$("#importf").onchange=async e=>{const f=e.target.files[0];if(!f)return;const j=JSON.parse(await f.text());await api("/api/import",j);cur=null;await load();toast("Restored")};
$("#presets").onclick=async()=>{if(!confirm("Replace all personas with the built-in presets?"))return;await api("/api/presets",{});cur=null;await load();toast("Presets restored")};
$("#restart").onclick=async()=>{if(!confirm("Restart Billy?"))return;await api("/api/restart",{});toast("Restarting...")};
async function loadLog(){if(!$("#logs").classList.contains("on"))return;const t=await api("/api/logs");const e=$("#log");const atEnd=e.scrollTop+e.clientHeight>=e.scrollHeight-20;e.textContent=t;if(atEnd)e.scrollTop=e.scrollHeight;setTimeout(loadLog,2000)}
load();setInterval(()=>api("/api/state").then(s=>{S.state=s.state;$("#status").innerHTML=`v${s.version}<br>${s.state}`}).catch(()=>{}),5000);
</script></body></html>)HTML";

// ---------------------------------------------------------------- API
const char *billyStateText();   // in the main sketch

static bool readBody(JsonDocument &doc) {
  if (!web.hasArg("plain")) { web.send(400, "text/plain", "no body"); return false; }
  if (deserializeJson(doc, web.arg("plain"))) { web.send(400, "text/plain", "bad json"); return false; }
  return true;
}

static void sendJson(JsonDocument &doc) {
  String out; serializeJson(doc, out);
  web.send(200, "application/json", out);
}

static String slugify(String n) {
  String s; n.toLowerCase();
  for (char c : n) if (isalnum((unsigned char)c)) s += c;
  if (s.isEmpty()) s = "fish";
  String base = s; int i = 2;
  while (!findPersona(s).isNull()) s = base + String(i++);
  return s;
}

void registerWebApi() {
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html", WEB_PAGE); });

  web.on("/api/state", HTTP_GET, [] {
    JsonDocument d;
    d["version"] = FW_VERSION;
    d["ip"] = WiFi.localIP().toString();
    d["state"] = billyStateText();
    d["volume"] = volume;
    d["heap"] = (uint32_t)ESP.getFreeHeap();
    d["active"] = activePersona()["id"].as<String>();
    d["personas"] = personasDoc["personas"];
    d["facts"] = memoryDoc["facts"];
    sendJson(d);
  });

  web.on("/api/persona", HTTP_POST, [] {
    JsonDocument in; if (!readBody(in)) return;
    String id = in["id"] | "";
    JsonObject p = id.length() ? findPersona(id) : JsonObject();
    if (p.isNull()) {
      p = personasDoc["personas"].as<JsonArray>().add<JsonObject>();
      id = slugify(in["name"] | "fish");
    }
    p.clear();
    p["id"] = id;
    p["name"] = (in["name"] | "Billy");
    p["voice"] = (in["voice"] | "Algenib");
    p["instructions"] = (in["instructions"] | "");
    JsonObject t = p["traits"].to<JsonObject>();
    for (int i = 0; i < NUM_TRAITS; i++) t[TRAITS[i]] = constrain((int)(in["traits"][TRAITS[i]] | 50), 0, 100);
    JsonObject b = p["backstory"].to<JsonObject>();
    for (const char *k : {"origin", "species", "caught", "purpose", "awakening"}) b[k] = (in["backstory"][k] | "");
    savePersonas();
    if (id == activePersona()["id"].as<String>()) personaSwitchPending = true;
    Log.println("[web] saved persona " + p["name"].as<String>());
    JsonDocument r; r["id"] = id; sendJson(r);
  });

  web.on("/api/persona/delete", HTTP_POST, [] {
    JsonDocument in; if (!readBody(in)) return;
    String id = in["id"] | "";
    JsonArray arr = personasDoc["personas"];
    if (arr.size() < 2) { web.send(400, "text/plain", "keep one"); return; }
    for (size_t i = 0; i < arr.size(); i++)
      if (id == arr[i]["id"].as<String>()) { arr.remove(i); break; }
    if (findPersona(personasDoc["active"] | "").isNull()) personasDoc["active"] = arr[0]["id"].as<String>();
    savePersonas();
    web.send(200, "text/plain", "ok");
  });

  web.on("/api/active", HTTP_POST, [] {
    JsonDocument in; if (!readBody(in)) return;
    String id = in["id"] | "";
    if (findPersona(id).isNull()) { web.send(404, "text/plain", "no such persona"); return; }
    personasDoc["active"] = id;
    savePersonas();
    personaSwitchPending = true;
    Log.println("[web] active persona: " + activeName());
    web.send(200, "text/plain", "ok");
  });

  web.on("/api/memory", HTTP_POST, [] {
    JsonDocument in; if (!readBody(in)) return;
    JsonArray facts = memoryDoc["facts"].to<JsonArray>();
    for (JsonVariant v : in["facts"].as<JsonArray>()) {
      String f = v.as<String>(); f.trim();
      if (f.length() && facts.size() < MAX_FACTS) facts.add(f.substring(0, 200));
    }
    saveMemory();
    web.send(200, "text/plain", "ok");
  });

  web.on("/api/volume", HTTP_POST, [] {
    JsonDocument in; if (!readBody(in)) return;
    setVolume(in["v"] | volume);
    web.send(200, "text/plain", "ok");
  });

  web.on("/api/test", HTTP_GET, [] { testMotor(web.arg("m")); web.send(200, "text/plain", "ok"); });
  web.on("/api/logs", HTTP_GET, [] { web.send(200, "text/plain", Log.text()); });

  web.on("/api/export", HTTP_GET, [] {
    JsonDocument d;
    d["sassybass_backup"] = 1;
    d["personas"] = personasDoc;
    d["memory"] = memoryDoc;
    String out; serializeJsonPretty(d, out);
    web.sendHeader("Content-Disposition", "attachment; filename=\"sassybass-backup.json\"");
    web.send(200, "application/json", out);
  });

  web.on("/api/import", HTTP_POST, [] {
    JsonDocument in; if (!readBody(in)) return;
    if (!in["personas"]["personas"].is<JsonArray>() || in["personas"]["personas"].size() == 0) {
      web.send(400, "text/plain", "not a SassyBass backup"); return;
    }
    personasDoc.set(in["personas"]);
    if (in["memory"]["facts"].is<JsonArray>()) memoryDoc.set(in["memory"]);
    savePersonas(); saveMemory();
    personaSwitchPending = true;
    Log.println("[web] backup restored");
    web.send(200, "text/plain", "ok");
  });

  web.on("/api/presets", HTTP_POST, [] {
    resetPersonasToPresets();
    personaSwitchPending = true;
    web.send(200, "text/plain", "ok");
  });

  web.on("/api/restart", HTTP_POST, [] {
    web.send(200, "text/plain", "ok");
    delay(300);
    ESP.restart();
  });
}
