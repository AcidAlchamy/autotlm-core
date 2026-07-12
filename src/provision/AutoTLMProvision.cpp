/*
 * AutoTLMProvision.cpp — captive-portal provisioning implementation.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMProvision.h"

#if defined(ESP32)

#include <WiFi.h>

// The whole setup page, self-contained (no external assets — the client has
// no internet while joined to the provisioning AP).
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AutoTLM One setup</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#e6edf3;--dim:#8b949e;--acc:#2f81f7}
*{box-sizing:border-box;font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}
body{margin:0;background:var(--bg);color:var(--fg)}
.wrap{max-width:440px;margin:0 auto;padding:24px 16px}
h1{font-size:22px;margin:8px 0 2px}
h1 b{color:var(--acc)}
.sub{color:var(--dim);font-size:13px;margin-bottom:20px}
fieldset{border:1px solid var(--line);border-radius:10px;background:var(--card);margin:0 0 14px;padding:14px}
legend{padding:0 6px;font-size:13px;color:var(--dim)}
label{display:block;font-size:13px;margin:10px 0 4px}
input,select{width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:7px;background:var(--bg);color:var(--fg);font-size:15px}
input:focus,select:focus{outline:none;border-color:var(--acc)}
.row{display:flex;gap:10px}.row>div{flex:1}
button{width:100%;padding:12px;border:0;border-radius:8px;background:var(--acc);color:#fff;font-size:16px;font-weight:600;cursor:pointer;margin-top:6px}
.ghost{background:transparent;border:1px solid var(--line);color:var(--dim);font-size:13px;font-weight:400;padding:8px;margin:8px 0 0}
.hint{color:var(--dim);font-size:12px;margin-top:4px}
#nets{margin-top:8px}
.net{padding:8px 10px;border:1px solid var(--line);border-radius:7px;margin-top:6px;cursor:pointer;font-size:14px;display:flex;justify-content:space-between}
.net:hover{border-color:var(--acc)}
.net span{color:var(--dim);font-size:12px}
#done{display:none;text-align:center;padding:40px 0}
#done h2{color:var(--acc)}
</style></head><body><div class="wrap">
<form id="f" method="POST" action="/save">
<h1>Auto<b>TLM</b> One</h1>
<div class="sub">First-boot setup — saved to the device, survives reflash.</div>
<fieldset><legend>WiFi</legend>
<label>Network (your driving hotspot)</label>
<input name="ssid" id="ssid" maxlength="32" required placeholder="MyHotspot">
<button type="button" class="ghost" onclick="scan()">Scan for networks</button>
<div id="nets"></div>
<label>Password</label>
<input name="pass" type="password" maxlength="64" placeholder="(open network: leave blank)">
</fieldset>
<fieldset><legend>Cloud (optional)</legend>
<label>Ingest URL</label>
<input name="url" maxlength="200" placeholder="http://yourserver.com/api/ingest">
<div class="hint">Plain http:// — TLS stalls on weak cellular; the token authenticates.</div>
<label>Bearer token</label>
<input name="token" maxlength="255">
<label>Push interval</label>
<select name="interval"><option value="1000">every second</option>
<option value="2000">every 2 s</option><option value="5000">every 5 s</option>
<option value="10000">every 10 s</option></select>
</fieldset>
<fieldset><legend>Device</legend>
<div class="row"><div>
<label>GPS</label>
<select name="gps"><option value="1">on</option><option value="0">off</option></select>
</div><div>
<label>Display units</label>
<select name="units"><option value="metric">metric (km/h, &deg;C)</option>
<option value="imperial">imperial (mph, &deg;F)</option></select>
</div></div>
</fieldset>
<button type="submit">Save &amp; start driving</button>
</form>
<div id="done"><h2>Saved!</h2><p>The unit is rebooting into your settings.<br>
You can close this page and turn WiFi back to normal.</p></div>
<script>
function scan(){
 var d=document.getElementById('nets');d.textContent='scanning…';
 fetch('/scan').then(r=>r.json()).then(list=>{
  d.textContent='';
  list.forEach(n=>{var e=document.createElement('div');e.className='net';
   e.innerHTML='<b>'+n.ssid.replace(/</g,'&lt;')+'</b><span>'+n.rssi+' dBm'+(n.open?' · open':'')+'</span>';
   e.onclick=()=>{document.getElementById('ssid').value=n.ssid;};d.appendChild(e);});
  if(!list.length)d.textContent='nothing found — try again';
 }).catch(()=>{d.textContent='scan failed — try again';});
}
document.getElementById('f').addEventListener('submit',function(ev){
 ev.preventDefault();
 fetch('/save',{method:'POST',body:new URLSearchParams(new FormData(this))})
  .then(()=>{document.getElementById('f').style.display='none';
             document.getElementById('done').style.display='block';});
});
</script></div></body></html>)HTML";

bool AutoTLMProvision::start(const char* apName, const char* apPass) {
  if (m_active) return true;

  if (apName && apName[0]) {
    strncpy(m_apName, apName, sizeof(m_apName) - 1);
    m_apName[sizeof(m_apName) - 1] = 0;
  } else {
    snprintf(m_apName, sizeof(m_apName), "AutoTLM-%04X",
             (unsigned)(ESP.getEfuseMac() >> 16) & 0xFFFF);
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(m_apName, (apPass && apPass[0]) ? apPass : nullptr)) {
    if (m_log) m_log->println("PROVISION:softAP failed");
    return false;
  }

  // Catch-all DNS: every hostname resolves to us, so phones pop their
  // "sign in to network" sheet straight onto the setup page.
  m_dns.setErrorReplyCode(DNSReplyCode::NoError);
  m_dns.start(53, "*", WiFi.softAPIP());

  m_http.on("/", HTTP_GET, [this]() { handleRoot(); });
  m_http.on("/scan", HTTP_GET, [this]() { handleScan(); });
  m_http.on("/save", HTTP_POST, [this]() { handleSave(); });
  m_http.onNotFound([this]() { handleNotFound(); });
  m_http.begin();

  m_active = true;
  if (m_log)
    m_log->printf("PROVISION:portal up — join WiFi \"%s\", open http://%s/\n",
                  m_apName, WiFi.softAPIP().toString().c_str());
  return true;
}

void AutoTLMProvision::tick() {
  if (!m_active) return;
  m_dns.processNextRequest();
  m_http.handleClient();

  // Give the browser a moment to render the success page before rebooting.
  if (m_restartAt && (int32_t)(millis() - m_restartAt) > 0) {
    if (m_log) m_log->println("PROVISION:rebooting into saved settings");
    delay(100);
    ESP.restart();
  }
}

void AutoTLMProvision::stop() {
  if (!m_active) return;
  m_http.stop();
  m_dns.stop();
  WiFi.softAPdisconnect(true);
  m_active = false;
  if (m_log) m_log->println("PROVISION:portal stopped");
}

void AutoTLMProvision::handleRoot() {
  m_http.send_P(200, "text/html", PORTAL_HTML);
}

void AutoTLMProvision::handleScan() {
  // Synchronous scan; the AP stays up throughout. A handful of seconds is
  // fine here — the user just tapped "Scan".
  const int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  m_http.send(200, "application/json", json);
}

void AutoTLMProvision::handleSave() {
  if (!m_config) {
    m_http.send(500, "text/plain", "no config store");
    return;
  }
  const String ssid = m_http.arg("ssid");
  if (ssid.length() == 0) {
    m_http.send(400, "text/plain", "ssid required");
    return;
  }

  m_config->saveWifi(ssid.c_str(), m_http.arg("pass").c_str());
  const String url = m_http.arg("url");
  const uint32_t interval = (uint32_t)m_http.arg("interval").toInt();
  m_config->saveCloud(url.c_str(), m_http.arg("token").c_str(),
                      interval ? interval : 1000);
  m_config->saveGpsEnabled(m_http.arg("gps") != "0");
  const String units = m_http.arg("units");
  m_config->saveUnits(units == "imperial" ? "imperial" : "metric");

  m_saved = true;
  if (m_log)
    m_log->printf("PROVISION:saved wifi=\"%s\" cloud=%s gps=%s units=%s\n",
                  ssid.c_str(), url.length() ? "yes" : "no",
                  m_http.arg("gps") != "0" ? "on" : "off",
                  units == "imperial" ? "imperial" : "metric");

  m_http.send(200, "text/plain", "OK");
  if (m_restartOnSave) m_restartAt = millis() + 1500;
}

void AutoTLMProvision::handleNotFound() {
  // Captive-portal probes (generate_204, hotspot-detect.html, ncsi.txt...)
  // and any stray URL all bounce to the setup page.
  m_http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  m_http.send(302, "text/plain", "");
}

#else  // !ESP32 — no radio: the portal politely reports "not available".

bool AutoTLMProvision::start(const char*, const char*) {
  if (m_log) m_log->println("PROVISION:not available on this platform");
  return false;
}
void AutoTLMProvision::tick() {}
void AutoTLMProvision::stop() {}

#endif
