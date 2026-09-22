#include "tp_web.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_log.h>

#include "tp_app.h"
#include "tp_config.h"
#include "tp_link.h"

static const char *TAG = "tp_web";

static WebServer s_server(80);

static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Teleprompter</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:#111417;color:#e8eaed;
     font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:760px;margin:0 auto}
h1{font-size:19px;margin:0 0 14px;font-weight:600}
textarea{width:100%;min-height:230px;padding:12px;border-radius:10px;
         border:1px solid #333a41;background:#181c20;color:#e8eaed;
         font:15px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;resize:vertical}
.row{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:14px 0}
button{flex:1 1 120px;padding:11px 14px;border-radius:9px;border:1px solid #333a41;
       background:#222a31;color:#e8eaed;font-size:15px;cursor:pointer}
button:hover{background:#2c363f}
button.primary{background:#2f6fdb;border-color:#2f6fdb}
button.primary:hover{background:#3b7de8}
label{display:block;margin:16px 0 6px;font-size:13px;color:#9aa4ad}
input[type=range]{width:100%}
.chk{display:flex;align-items:center;gap:9px;margin:16px 0;font-size:14px}
.stat{margin-top:18px;padding-top:14px;border-top:1px solid #262c32;
      font-size:12.5px;color:#8b949e}
.pace{margin:16px 0;padding:14px;border-radius:10px;background:#181c20;
      border:1px solid #262c32}
.pace .line{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.pace input[type=text]{width:88px;padding:9px 10px;border-radius:8px;
      border:1px solid #333a41;background:#111417;color:#e8eaed;
      font:15px ui-monospace,SFMono-Regular,Menlo,monospace;text-align:center}
.pace button{flex:0 0 auto}
.big{margin-top:12px;font-size:22px;font-weight:600;letter-spacing:.2px}
.sub{margin-top:4px;font-size:13px;color:#8b949e}
.ahead{color:#5ac27a}
.behind{color:#e58a5a}
.bar{margin-top:12px;height:5px;border-radius:3px;background:#262c32;overflow:hidden}
.bar i{display:block;height:100%;background:#2f6fdb;width:0}
.warn{margin-top:12px;padding:10px 12px;border-radius:8px;
      background:#3a2a12;color:#f0c36d;font-size:13px}
</style></head><body><div class="wrap">
<h1>Teleprompter</h1>

<textarea id="script" spellcheck="false"></textarea>
<div class="row">
  <button class="primary" onclick="saveScript(this)">Save script</button>
  <button onclick="ctl('rewind=1')">Back to top</button>
  <button id="pp" onclick="togglePause()">Pause</button>
</div>

<div class="pace">
  <div class="line">
    <span>Target length</span>
    <input type="text" id="dur" placeholder="2:00" inputmode="numeric">
    <button onclick="setDur()">Set</button>
    <button onclick="ctl('duration=0')">Off</button>
    <button onclick="ctl('repace=1')">Re-pace</button>
  </div>
  <div class="big" id="clock">0:00</div>
  <div class="sub" id="pacing"></div>
  <div class="bar"><i id="prog"></i></div>
</div>

<label>Scroll speed - <span id="spdv"></span> px/s</label>
<input type="range" id="spd" min="2" max="400" step="1" oninput="spdv.textContent=this.value"
       onchange="ctl('speed='+this.value)">

<label>Brightness - <span id="brtv"></span>%</label>
<input type="range" id="brt" min="10" max="100" step="5" oninput="brtv.textContent=this.value"
       onchange="ctl('brightness='+this.value)">

<div class="chk">
  <input type="checkbox" id="mir" onchange="ctl('mirror='+(this.checked?1:0))">
  <label for="mir" style="margin:0">Mirror text (for beam-splitter glass)</label>
</div>

<div id="warn" class="warn" hidden></div>
<div id="stat" class="stat"></div>
</div><script>
const $ = id => document.getElementById(id);
let paused = false;

async function ctl(q){ await fetch('/api/control?'+q, {method:'POST'}); refresh(); }

function togglePause(){ ctl('paused='+(paused?0:1)); }

const fmt = s => {
  s = Math.max(0, Math.round(s));
  return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');
};

function parseDur(v){
  v = (v||'').trim();
  if(!v) return 0;
  const m = v.match(/^(\d+):([0-5]?\d)$/);
  if(m) return (+m[1])*60 + (+m[2]);
  const n = parseInt(v,10);
  return isNaN(n) ? -1 : n;
}

function setDur(){
  const s = parseDur($('dur').value);
  if(s < 0){ $('pacing').textContent = 'Enter a length like 2:00, or seconds.'; return; }
  ctl('duration='+s);
}

async function saveScript(b){
  const old = b.textContent;
  b.textContent = 'Saving...'; b.disabled = true;
  await fetch('/api/script', {method:'POST', body:$('script').value});
  b.textContent = old; b.disabled = false;
  refresh();
}

async function refresh(){
  try{
    const s = await (await fetch('/api/state')).json();
    paused = s.paused;
    $('pp').textContent = s.finished ? 'Restart' : (paused ? 'Play' : 'Pause');
    if(document.activeElement !== $('spd')){ $('spd').value = s.speed; }
    $('spdv').textContent = s.speed < 10 ? s.speed.toFixed(1) : Math.round(s.speed);
    if(document.activeElement !== $('brt')){ $('brt').value = s.brightness; }
    $('brtv').textContent = s.brightness;
    $('mir').checked = s.mirror;
    if(document.activeElement !== $('dur')){
      $('dur').value = s.duration_s ? fmt(s.duration_s) : '';
    }

    const eff = s.speed * (1 + (s.trim || 0));
    const travel = Math.max(1, s.end_row - s.start_row);
    const elapsed = s.elapsed_ms / 1000;
    const covered = Math.min(travel, Math.max(0, s.view_row - s.start_row));
    $('prog').style.width = (covered / travel * 100).toFixed(1) + '%';
    $('clock').textContent = fmt(elapsed) + (s.duration_s ? ' / ' + fmt(s.duration_s) : '');

    let txt = '';
    if(s.finished){
      txt = 'Finished.';
    } else if(eff > 0){
      txt = 'at this speed: ' + fmt(travel / eff) + ' total, ' +
            fmt((travel - covered) / eff) + ' left';
    }
    if(Math.abs(s.trim || 0) > 0.01){
      txt += ' | <span class="' + (s.trim > 0 ? 'ahead' : 'behind') + '">remote trim ' +
             (s.trim > 0 ? '+' : '') + Math.round(s.trim * 100) + '%</span>';
    }
    if(s.duration_s > 0 && !s.finished && eff > 0 && elapsed > 2){
      const drift = (covered - travel * Math.min(1, elapsed / s.duration_s)) / eff;
      if(Math.abs(drift) >= 1){
        txt += ' | <span class="' + (drift > 0 ? 'ahead' : 'behind') + '">' +
               fmt(Math.abs(drift)) + (drift > 0 ? ' ahead' : ' behind') + '</span>';
      }
    }
    if(s.duration_s > 0 && s.words > 0){
      const wpm = Math.round(s.words * 60 / s.duration_s);
      txt += ' | <span class="' + (wpm > 200 ? 'behind' : '') + '">' + wpm + ' wpm</span>';
    }
    $('pacing').innerHTML = txt;

    $('stat').textContent =
      s.lines+' lines, '+s.words+' words | '+s.fps+' fps | heap '+
      Math.round(s.heap/1024)+'K | psram '+Math.round(s.psram/1024)+'K';
    const w = $('warn');
    if(s.missing > 0){
      w.hidden = false;
      w.textContent = s.missing+' character(s) in this script are not in the font and '+
        'were substituted. Regenerate Roboto_96 with the full character set.';
    } else { w.hidden = true; }
  }catch(e){}
}

(async function(){
  $('script').value = await (await fetch('/api/script')).text();
  refresh();
  setInterval(refresh, 500);   // the run clock needs to look like a clock
})();
</script></body></html>)HTML";

static void handle_root()
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send_P(200, "text/html", PAGE_HTML);
}

static void handle_get_script()
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/plain", tp_app_script());
}

static void handle_post_script()
{
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "text/plain", "empty body");
        return;
    }
    tp_app_set_script(s_server.arg("plain"));
    s_server.send(200, "text/plain", "ok");
}

static void handle_control()
{
    if (s_server.hasArg("speed"))      tp_app_set_speed(s_server.arg("speed").toFloat());
    if (s_server.hasArg("paused"))     tp_app_set_paused(s_server.arg("paused").toInt() != 0);
    if (s_server.hasArg("brightness")) tp_app_set_brightness(s_server.arg("brightness").toInt());
    if (s_server.hasArg("mirror"))     tp_app_set_mirror(s_server.arg("mirror").toInt() != 0);
    if (s_server.hasArg("duration"))   tp_app_set_duration(s_server.arg("duration").toInt());
    if (s_server.hasArg("repace"))     tp_app_repace();
    if (s_server.hasArg("rewind"))     tp_app_rewind();
    s_server.send(200, "text/plain", "ok");
}

static void handle_state()
{
    tp_status_t st;
    tp_app_status(&st);

    String j;
    j.reserve(256);
    j += "{\"speed\":";      j += String(st.speed, 1);
    j += ",\"trim\":";       j += String(st.trim, 3);
    j += ",\"paused\":";     j += st.paused ? "true" : "false";
    j += ",\"mirror\":";     j += st.mirror ? "true" : "false";
    j += ",\"finished\":";   j += st.finished ? "true" : "false";
    j += ",\"brightness\":"; j += st.brightness;
    j += ",\"lines\":";      j += st.lines;
    j += ",\"words\":";      j += st.words;
    j += ",\"missing\":";    j += st.missing_glyphs;
    j += ",\"duration_s\":"; j += st.duration_s;
    j += ",\"elapsed_ms\":"; j += st.elapsed_ms;
    j += ",\"start_row\":";  j += st.start_row;
    j += ",\"end_row\":";    j += st.end_row;
    j += ",\"view_row\":";   j += st.view_row;
    j += ",\"loop_rows\":";  j += st.loop_rows;
    j += ",\"fps\":";        j += st.fps;
    j += ",\"heap\":";       j += st.free_heap;
    j += ",\"psram\":";      j += st.free_psram;
    j += "}";

    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "application/json", j);
}

static void web_task(void *)
{
    WiFi.mode(WIFI_AP);
    WiFi.softAP(TP_AP_SSID, TP_AP_PASSWORD, TP_AP_CHANNEL);
    ESP_LOGI(TAG, "AP \"%s\" up - open http://%s/",
             TP_AP_SSID, WiFi.softAPIP().toString().c_str());

    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/script", HTTP_GET, handle_get_script);
    s_server.on("/api/script", HTTP_POST, handle_post_script);
    s_server.on("/api/control", HTTP_POST, handle_control);
    s_server.on("/api/state", HTTP_GET, handle_state);
    s_server.begin();

    tp_link_start();        // ESP-NOW needs the AP up and the channel settled

    for (;;) {
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void tp_web_start(void)
{
    // Core 0, alongside the Wi-Fi stack. The renderer owns core 1.
    xTaskCreatePinnedToCore(web_task, "tp_web", 8192, nullptr, 2, nullptr, 0);
}
