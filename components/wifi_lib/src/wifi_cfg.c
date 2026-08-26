#include "web_cfg.h"
#include "wifi_db.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "WEB_CFG";
static httpd_handle_t s_srv = NULL;

static char g_sta_ssid[33] = {0};
static char g_sta_ip[16] = "0.0.0.0";
static char g_ap_ip[16] = "192.168.4.1"; // default, but we’ll also read it
static volatile int g_sta_state = 0;     // 0=DISCONNECTED,1=CONNECTING,2=CONNECTED

static void sta_status_set(int state, const char *ssid, const char *ip)
{
    g_sta_state = state;
    if (ssid)
        strlcpy(g_sta_ssid, ssid, sizeof(g_sta_ssid));
    if (ip)
        strlcpy(g_sta_ip, ip, sizeof(g_sta_ip));
}

// static const char *INDEX_HTML =
//     "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
//     "<title>ESP WiFi Config</title>"
//     "<style>"
//     "body{font-family:Arial;margin:16px} .card{border:1px solid #ddd;border-radius:12px;padding:12px;margin:10px 0}"
//     "button{padding:10px 14px;border-radius:10px;border:1px solid #222;background:#111;color:#fff}"
//     "input{padding:10px;border-radius:10px;border:1px solid #bbb;width:100%;box-sizing:border-box}"
//     "select{padding:10px;border-radius:10px;border:1px solid #bbb;width:100%}"
//     ".row{display:flex;gap:10px} .row>div{flex:1}"
//     ".smallbtn{padding:8px 10px;border-radius:10px;border:1px solid #222;background:#fff;color:#111}"
//     "</style></head><body>"
//     "<h2>Wi-Fi Configurator</h2>"
//     "<div class='card'>"
//     "<div class='row'><div><button onclick='scan()'>Scan</button></div><div><button onclick='loadKnown()'>Known</button></div></div>"
//     "<p id='status'></p>"
//     "</div>"
//     "<div class='card'>"
//     "<h3>Add / Update</h3>"

//     "<div class='row'>"
//     "<div><button class='smallbtn' onclick='useScanned()'>Use Scanned</button></div>"
//     "<div><button class='smallbtn' onclick='useManual()'>Manual SSID</button></div>"
//     "</div>"

//     "<div id='ssid_scanned_box' style='margin-top:10px'>"
//     "<label>SSID (Scanned)</label>"
//     "<select id='ssid'></select>"
//     "</div>"

//     "<div id='ssid_manual_box' style='margin-top:10px;display:none'>"
//     "<label>SSID (Manual)</label>"
//     "<input id='ssid_manual' type='text' placeholder='Enter SSID manually'/>"
//     "</div>"

//     "<label style='margin-top:10px;display:block'>Password</label>"
//     "<input id='pass' type='password' placeholder='Enter password'/>"

//     "<div style='margin-top:10px'><button onclick='save()'>Save</button></div>"
//     "</div>"
//     "<div class='card'>"
//     "<h3>Known Networks</h3>"
//     "<div id='known'></div>"
//     "</div>"
//     "<script>"
//     "const el=(id)=>document.getElementById(id);"
//     "function msg(t){el('status').innerText=t;}"

//     "let ssidMode='scanned';"
//     "function useScanned(){ssidMode='scanned'; el('ssid_scanned_box').style.display='block'; el('ssid_manual_box').style.display='none';}"
//     "function useManual(){ssidMode='manual'; el('ssid_scanned_box').style.display='none'; el('ssid_manual_box').style.display='block'; el('ssid_manual').focus();}"

//     "async function scan(){msg('Scanning...');"
//     "let r=await fetch('/api/scan'); let j=await r.json();"
//     "let s=el('ssid'); s.innerHTML='';"
//     "j.aps.forEach(a=>{let o=document.createElement('option');"
//     "o.value=a.ssid; o.text=a.ssid+'  ('+a.rssi+' dBm)'; s.appendChild(o);});"
//     "msg('Found '+j.aps.length+' APs');}"

//     // "async function loadKnown(){msg('Loading known...');"
//     // "let r=await fetch('/api/known'); let j=await r.json();"
//     // "let k=el('known'); k.innerHTML='';"
//     // "j.known.forEach(n=>{"
//     // "let d=document.createElement('div'); d.style.margin='8px 0';"
//     // "d.innerHTML='<b>'+n.ssid+'</b> <button style=\"margin-left:10px\" onclick=\"del(\\''+n.ssid+'\\')\">Delete</button>';"
//     // "k.appendChild(d);});"
//     // "msg('Known: '+j.known.length);}"

//     "async function loadKnown(){msg('Loading known...'); "
//     "let r=await fetch('/api/known'); let j=await r.json();"
//     "let k=el('known'); k.innerHTML='';"
//     "j.known.forEach(n=>{"
//     "let d=document.createElement('div'); d.style.margin='8px 0';"
//     "let btn = '';"
//     "if (n.avail) {"
//     "btn = ` <button style=\"margin-left:10px\" onclick=\"connectNow('${n.ssid}')\">Connect</button>`;"
//     "} else {"
//     "btn = ` <button style=\"margin-left:10px\" disabled>Not in range</button>`;"
//     "}"
//     "d.innerHTML = `<b>${n.ssid}</b>  <span style=\"color:#666\">(${n.rssi} dBm)</span>`"
//     "+ btn"
//     "+ ` <button style=\"margin-left:10px\" onclick=\"del('${n.ssid}')\">Delete</button>`;"
//     "k.appendChild(d);"
//     "});"
//     "msg('Known: '+j.known.length);"
//     "}"

//     "async function connectNow(ssid){"
//     "msg('Connecting to '+ssid+' ...');"
//     "let r=await fetch('/api/connect',{"
//     "method:'POST',"
//     "headers:{'Content-Type':'application/json'},"
//     "body:JSON.stringify({ssid})"
//     "});"
//     "let j=await r.json();"
//     "msg(j.ok ? ('Connect started: '+ssid) : ('Connect failed: '+(j.err||'unknown')));"
//     "}"

//     "async function save(){"
//     "let pass=el('pass').value;"
//     "let ssid='';"
//     "if(ssidMode==='manual'){ssid=(el('ssid_manual').value||'').trim();}"
//     "else{ssid=el('ssid').value;}"
//     "if(!ssid){msg('SSID is empty');return;}"
//     "msg('Saving...');"
//     "let r=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});"
//     "let j=await r.json(); msg(j.ok?'Saved':'Save failed'); loadKnown();}"

//     "async function del(ssid){"
//     "let r=await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid})});"
//     "let j=await r.json(); msg(j.ok?'Deleted':'Delete failed'); loadKnown();}"

//     "useScanned();"
//     "scan(); loadKnown();"
//     "</script></body></html>";

static const char *INDEX_HTML =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP WiFi Config</title>"
    "<style>"
    "body{font-family:Arial;margin:16px} .card{border:1px solid #ddd;border-radius:12px;padding:12px;margin:10px 0}"
    "button{padding:10px 14px;border-radius:10px;border:1px solid #222;background:#111;color:#fff;cursor:pointer}"
    "button:disabled{opacity:.5;cursor:not-allowed}"
    "input{padding:10px;border-radius:10px;border:1px solid #bbb;width:100%;box-sizing:border-box}"
    "select{padding:10px;border-radius:10px;border:1px solid #bbb;width:100%}"
    ".row{display:flex;gap:10px} .row>div{flex:1}"
    ".smallbtn{padding:8px 10px;border-radius:10px;border:1px solid #222;background:#fff;color:#111}"
    ".muted{color:#666}"
    "</style></head><body>"

    "<h2>Wi-Fi Configurator</h2>"

    "<div class='card'>"
    "  <div class='row'>"
    "    <div><button id='btnScan'>Scan</button></div>"
    // "    <div><button id='btnKnown'>Known</button></div>"
    "<div><button id='btnKnown'>Refresh</button></div>"

    "  </div>"
    "  <p id='status'></p>"
    // "  <p><b>Wi-Fi:</b> <span id='wstat'>-</span></p>"
    "<p><b>Wi-Fi:</b> <span id='wstat'>-</span></p>"
    "<p class=\"muted\"><b>AP IP:</b> <span id=\"apip\">-</span> &nbsp;&nbsp; <b>STA IP:</b> <span id=\"staip\">-</span></p>"

    "</div>"

    "<div class='card'>"
    "  <h3>Add / Update</h3>"
    "  <div class='row'>"
    "    <div><button class='smallbtn' id='btnUseScanned'>Use Scanned</button></div>"
    "    <div><button class='smallbtn' id='btnUseManual'>Manual SSID</button></div>"
    "  </div>"

    "  <div id='ssid_scanned_box' style='margin-top:10px'>"
    "    <label>SSID (Scanned)</label>"
    "    <select id='ssid'></select>"
    "  </div>"

    "  <div id='ssid_manual_box' style='margin-top:10px;display:none'>"
    "    <label>SSID (Manual)</label>"
    "    <input id='ssid_manual' type='text' placeholder='Enter SSID manually'/>"
    "  </div>"

    "  <label style='margin-top:10px;display:block'>Password</label>"
    "  <input id='pass' type='password' placeholder='Enter password'/>"

    "  <div style='margin-top:10px'><button id='btnSave'>Save</button></div>"
    "</div>"

    "<div class='card'>"
    "  <h3>Known Networks</h3>"
    "  <div id='known'></div>"
    "</div>"

    "<script>"
    "function el(id){return document.getElementById(id);} "
    "function msg(t){el('status').innerText=t;} "

    "window.onerror = function(m, src, line, col){"
    "  msg('JS error: '+m+' @'+line+':'+col);"
    "};"

    "var ssidMode='scanned';"
    "function useScanned(){"
    "  ssidMode='scanned';"
    "  el('ssid_scanned_box').style.display='block';"
    "  el('ssid_manual_box').style.display='none';"
    "}"
    "function useManual(){"
    "  ssidMode='manual';"
    "  el('ssid_scanned_box').style.display='none';"
    "  el('ssid_manual_box').style.display='block';"
    "  el('ssid_manual').focus();"
    "}"

    "async function scan(){"
    "  msg('Scanning...');"
    "  try{"
    "    var r = await fetch('/api/scan');"
    "    var j = await r.json();"
    "    var s = el('ssid');"
    "    s.innerHTML='';"
    "    var aps = j.aps || [];"
    "    for(var i=0;i<aps.length;i++){"
    "      var o=document.createElement('option');"
    "      o.value=aps[i].ssid;"
    "      o.text=aps[i].ssid+'  ('+aps[i].rssi+' dBm)';"
    "      s.appendChild(o);"
    "    }"
    "    msg('Found '+aps.length+' APs');"
    "  }catch(e){"
    "    msg('Scan failed');"
    "  }"
    "}"

    "function addKnownRow(container, ssid, avail, rssi){"
    "  var row=document.createElement('div');"
    "  row.style.margin='8px 0';"

    "  var title=document.createElement('b');"
    "  title.innerText=ssid;"
    "  row.appendChild(title);"

    "  var r=document.createElement('span');"
    "  r.className='muted';"
    "  r.style.marginLeft='10px';"
    "  r.innerText='('+(avail ? (rssi+' dBm') : '---')+')';"
    "  row.appendChild(r);"

    "  var btnConnect=document.createElement('button');"
    "  btnConnect.style.marginLeft='10px';"
    "  if(avail){"
    "    btnConnect.innerText='Connect';"
    "    btnConnect.onclick=function(){connectNow(ssid);};"
    "  }else{"
    "    btnConnect.innerText='Not in range';"
    "    btnConnect.disabled=true;"
    "  }"
    "  row.appendChild(btnConnect);"

    "  var btnDel=document.createElement('button');"
    "  btnDel.style.marginLeft='10px';"
    "  btnDel.innerText='Delete';"
    "  btnDel.onclick=function(){delNet(ssid);};"
    "  row.appendChild(btnDel);"

    "  container.appendChild(row);"
    "}"

    "async function loadKnown(){"
    "  msg('Loading known...');"
    "  try{"
    "    var r = await fetch('/api/known');"
    "    var j = await r.json();"
    "    var k = el('known');"
    "    k.innerHTML='';"
    "    var known = j.known || [];"
    "    for(var i=0;i<known.length;i++){"
    "      addKnownRow(k, known[i].ssid, !!known[i].avail, known[i].rssi);"
    "    }"
    "    msg('Known: '+known.length);"
    "  }catch(e){"
    "    msg('Known load failed');"
    "  }"
    "}"

    "async function connectNow(ssid){"
    "  msg('Connecting to '+ssid+' ...');"
    "  try{"
    "    var r = await fetch('/api/connect',{"
    "      method:'POST',"
    "      headers:{'Content-Type':'application/json'},"
    "      body:JSON.stringify({ssid:ssid})"
    "    });"
    "    var j = await r.json();"
    "    msg(j.ok ? ('Connect started: '+ssid) : ('Connect failed: '+(j.err||'unknown')));"
    "  }catch(e){"
    "    msg('Connect request failed');"
    "  }"
    "}"

    "async function save(){"
    "  var pass = el('pass').value;"
    "  var ssid='';"
    "  if(ssidMode==='manual'){"
    "    ssid = (el('ssid_manual').value || '').trim();"
    "  }else{"
    "    ssid = el('ssid').value;"
    "  }"
    "  if(!ssid){msg('SSID is empty');return;}"
    "  msg('Saving...');"
    "  try{"
    "    var r = await fetch('/api/save',{"
    "      method:'POST',"
    "      headers:{'Content-Type':'application/json'},"
    "      body:JSON.stringify({ssid:ssid, pass:pass})"
    "    });"
    "    var j = await r.json();"
    "    msg(j.ok ? 'Saved' : 'Save failed');"
    "    loadKnown();"
    "  }catch(e){"
    "    msg('Save request failed');"
    "  }"
    "}"

    "async function delNet(ssid){"
    "  try{"
    "    var r = await fetch('/api/delete',{"
    "      method:'POST',"
    "      headers:{'Content-Type':'application/json'},"
    "      body:JSON.stringify({ssid:ssid})"
    "    });"
    "    var j = await r.json();"
    "    msg(j.ok ? 'Deleted' : 'Delete failed');"
    "    loadKnown();"
    "  }catch(e){"
    "    msg('Delete request failed');"
    "  }"
    "}"

    // "async function pollStatus(){"
    // "  try{"
    // "    var r = await fetch('/api/status');"
    // "    var j = await r.json();"
    // "    var t = j.state || '-';"
    // "    if(t==='CONNECTING'){t += ' ('+(j.ssid||'')+')';}"
    // "    if(t==='CONNECTED'){t += ' ('+(j.ssid||'')+')  IP: '+(j.ip||'');}"
    // "    el('wstat').innerText=t;"
    // "  }catch(e){}"
    // "}"

    "async function pollStatus(){"
    "try{"
    "var r = await fetch('/api/status');"
    "var j = await r.json();"

    "var t = j.state || '-';"
    "if(t==='CONNECTING'){t += ' ('+(j.ssid||'')+')';}"
    "if(t==='CONNECTED'){t += ' ('+(j.ssid||'')+')';}"
    "el('wstat').innerText = t;"

    // Always show IPs
    "el('apip').innerText  = j.ap_ip  || '-';"
    "el('staip').innerText = j.sta_ip || '-';"
    "}catch(e){}"
    "}"

    "el('btnScan').onclick = scan;"
    // "el('btnKnown').onclick = loadKnown;"
    "el('btnKnown').onclick = async function(){"
    "await loadKnown();"
    "await scan();"
    "await pollStatus();"
    "};"

    "el('btnUseScanned').onclick = useScanned;"
    "el('btnUseManual').onclick = useManual;"
    "el('btnSave').onclick = save;"

    "useScanned();" 
    
    "scan();"
    "loadKnown();"
    "setInterval(pollStatus, 1000);"
    "pollStatus();"
    "</script></body></html>";

// static void webcfg_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
// {
//     if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
//     {
//         sta_status_set(0, g_sta_ssid, "0.0.0.0");
//     }
//     else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
//     {
//         ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
//         char ipbuf[16];
//         snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ev->ip_info.ip));
//         sta_status_set(2, g_sta_ssid, ipbuf);
//     }
// }

static void webcfg_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        g_sta_state = 0;
        strlcpy(g_sta_ip, "0.0.0.0", sizeof(g_sta_ip));
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        g_sta_state = 2;
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
    {
        g_sta_state = 0;
        strlcpy(g_sta_ip, "0.0.0.0", sizeof(g_sta_ip));
    }
}

typedef struct
{
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
} wifi_connect_req_t;

static void wifi_connect_worker(void *p)
{
    wifi_connect_req_t *r = (wifi_connect_req_t *)p;

    ESP_LOGI(TAG, "CONNECT worker: %s", r->ssid);

    // Ensure APSTA mode (keep AP alive while connecting)
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, r->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, r->pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Set STA config then connect
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    ESP_LOGI(TAG, "set_config: %s", esp_err_to_name(err));

    sta_status_set(1, r->ssid, "0.0.0.0"); // CONNECTING...

    esp_wifi_disconnect(); // ignore
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "connect: %s", esp_err_to_name(err));

    free(r);
    vTaskDelete(NULL);
}

static bool read_body(httpd_req_t *req, char *buf, int buflen)
{
    int total = req->content_len;
    if (total <= 0 || total >= buflen)
        return false;

    int r = httpd_req_recv(req, buf, total);
    if (r <= 0)
        return false;
    buf[r] = 0;
    return true;
}

static void json_send(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_connect_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body)))
    {
        json_send(req, "{\"ok\":false,\"err\":\"bad_body\"}");
        return ESP_OK;
    }

    char ssid[WIFI_SSID_MAX + 1] = {0};
    char *s = strstr(body, "\"ssid\"");
    if (!s)
    {
        json_send(req, "{\"ok\":false,\"err\":\"no_ssid\"}");
        return ESP_OK;
    }
    sscanf(s, "\"ssid\":\"%32[^\"]\"", ssid);

    // Load DB and find password
    wifi_cred_t *list = calloc(WIFI_DB_MAX_RECORDS, sizeof(wifi_cred_t));
    if (!list)
    {
        json_send(req, "{\"ok\":false,\"err\":\"no_mem\"}");
        return ESP_OK;
    }

    int count = 0;
    wifi_db_load(list, &count);

    const char *pass = NULL;
    for (int i = 0; i < count; i++)
    {
        if (strncmp(list[i].ssid, ssid, WIFI_SSID_MAX) == 0)
        {
            pass = list[i].pass;
            break;
        }
    }

    if (!pass)
    {
        free(list);
        json_send(req, "{\"ok\":false,\"err\":\"not_known\"}");
        return ESP_OK;
    }

    // Optional: verify SSID is currently available (scan + match)
    // If you want strict checking, uncomment this block.
    /*
    wifi_scan_config_t scan_cfg = {.ssid=0,.bssid=0,.channel=0,.show_hidden=true,.scan_type=WIFI_SCAN_TYPE_ACTIVE};
    if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
        uint16_t ap_num = 0; esp_wifi_scan_get_ap_num(&ap_num);
        wifi_ap_record_t *aps = ap_num ? calloc(ap_num, sizeof(wifi_ap_record_t)) : NULL;
        if (aps) esp_wifi_scan_get_ap_records(&ap_num, aps);
        bool avail=false;
        for (int a=0;a<ap_num;a++) if (!strncmp((char*)aps[a].ssid, ssid, WIFI_SSID_MAX)) { avail=true; break; }
        free(aps);
        if (!avail) { free(list); json_send(req,"{\"ok\":false,\"err\":\"not_in_range\"}"); return ESP_OK; }
    }
    */

    wifi_connect_req_t *r = calloc(1, sizeof(*r));
    if (!r)
    {
        free(list);
        json_send(req, "{\"ok\":false,\"err\":\"no_mem\"}");
        return ESP_OK;
    }

    strlcpy(r->ssid, ssid, sizeof(r->ssid));
    strlcpy(r->pass, pass, sizeof(r->pass));

    free(list);

    // Start connect task (don’t do connect inside HTTP handler)
    xTaskCreate(wifi_connect_worker, "wifi_connect_worker", 8192, r, 5, NULL);

    json_send(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_scan_get(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = 0, .bssid = 0, .channel = 0, .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE};

    // esp_wifi_scan_start(&scan_cfg, true);
    esp_err_t e = esp_wifi_scan_start(&scan_cfg, true);
    if (e != ESP_OK)
    {
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(e));
        json_send(req, "{\"aps\":[]}");
        return ESP_OK;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0)
    {
        json_send(req, "{\"aps\":[]}");
        return ESP_OK;
    }

    wifi_ap_record_t *aps = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!aps)
    {
        json_send(req, "{\"aps\":[]}");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_num, aps);

    // Build JSON (simple, no escaping handling for weird SSIDs)
    // Limit output to first 30 for UI sanity
    int limit = ap_num > 30 ? 30 : ap_num;

    // Sort by RSSI desc (so UI shows strongest first)
    for (int i = 0; i < limit; i++)
    {
        for (int j = i + 1; j < limit; j++)
        {
            if (aps[j].rssi > aps[i].rssi)
            {
                wifi_ap_record_t t = aps[i];
                aps[i] = aps[j];
                aps[j] = t;
            }
        }
    }

    char *buf = malloc(2048);
    if (!buf)
    {
        free(aps);
        json_send(req, "{\"aps\":[]}");
        return ESP_OK;
    }

    strcpy(buf, "{\"aps\":[");
    for (int i = 0; i < limit; i++)
    {
        char line[160];
        snprintf(line, sizeof(line),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"auth\":%d}",
                 (i ? "," : ""),
                 (char *)aps[i].ssid,
                 aps[i].rssi,
                 aps[i].primary,
                 aps[i].authmode);
        strlcat(buf, line, 2048);
    }
    strlcat(buf, "]}", 2048);

    json_send(req, buf);
    free(buf);
    free(aps);
    return ESP_OK;
}

// static esp_err_t api_known_get(httpd_req_t *req)
// {
//     wifi_cred_t list[WIFI_DB_MAX_RECORDS] = {0};
//     int count = 0;
//     wifi_db_load(list, &count);

//     char *buf = malloc(1024);
//     if (!buf)
//     {
//         json_send(req, "{\"known\":[]}");
//         return ESP_OK;
//     }

//     strcpy(buf, "{\"known\":[");
//     for (int i = 0; i < count; i++)
//     {
//         char line[96];
//         snprintf(line, sizeof(line), "%s{\"ssid\":\"%s\"}", (i ? "," : ""), list[i].ssid);
//         strlcat(buf, line, 1024);
//     }
//     strlcat(buf, "]}", 1024);

//     json_send(req, buf);
//     free(buf);
//     return ESP_OK;
// }

static esp_err_t api_known_get(httpd_req_t *req)
{
    // Load known from DB using heap (avoid stack overflow in HTTP task)
    wifi_cred_t *list = calloc(WIFI_DB_MAX_RECORDS, sizeof(wifi_cred_t));
    if (!list)
    {
        json_send(req, "{\"known\":[]}");
        return ESP_OK;
    }

    int count = 0;
    wifi_db_load(list, &count);

    // Scan current APs
    wifi_scan_config_t scan_cfg = {
        .ssid = 0, .bssid = 0, .channel = 0, .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE};

    esp_err_t e = esp_wifi_scan_start(&scan_cfg, true);
    if (e != ESP_OK)
    {
        free(list);
        json_send(req, "{\"known\":[]}");
        return ESP_OK;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    wifi_ap_record_t *aps = NULL;
    if (ap_num)
    {
        aps = calloc(ap_num, sizeof(wifi_ap_record_t));
        if (aps)
            esp_wifi_scan_get_ap_records(&ap_num, aps);
    }

    char *buf = malloc(2048);
    if (!buf)
    {
        free(aps);
        free(list);
        json_send(req, "{\"known\":[]}");
        return ESP_OK;
    }

    strcpy(buf, "{\"known\":[");
    for (int i = 0; i < count; i++)
    {
        int best_rssi = -999;
        bool avail = false;

        if (aps)
        {
            for (int a = 0; a < ap_num; a++)
            {
                if (strncmp((char *)aps[a].ssid, list[i].ssid, WIFI_SSID_MAX) == 0)
                {
                    avail = true;
                    if (aps[a].rssi > best_rssi)
                        best_rssi = aps[a].rssi;
                }
            }
        }

        char line[160];
        snprintf(line, sizeof(line),
                 "%s{\"ssid\":\"%s\",\"avail\":%s,\"rssi\":%d}",
                 (i ? "," : ""),
                 list[i].ssid,
                 (avail ? "true" : "false"),
                 avail ? best_rssi : -999);
        strlcat(buf, line, 2048);
    }
    strlcat(buf, "]}", 2048);

    json_send(req, buf);

    free(buf);
    free(aps);
    free(list);
    return ESP_OK;
}

// static esp_err_t api_status_get(httpd_req_t *req)
// {
//     char buf[128];

//     const char *state_str = "DISCONNECTED";
//     if (g_sta_state == 1)
//         state_str = "CONNECTING";
//     else if (g_sta_state == 2)
//         state_str = "CONNECTED";

//     snprintf(buf, sizeof(buf),
//              "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}",
//              state_str, g_sta_ssid, g_sta_ip);

//     httpd_resp_set_type(req, "application/json");
//     httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
//     return ESP_OK;
// }

static esp_err_t api_status_get(httpd_req_t *req)
{
    // --- Read AP IP live ---
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap)
    {
        esp_netif_ip_info_t ipinfo;
        if (esp_netif_get_ip_info(ap, &ipinfo) == ESP_OK)
        {
            snprintf(g_ap_ip, sizeof(g_ap_ip), IPSTR, IP2STR(&ipinfo.ip));
        }
    }

    // --- Read STA IP live (THIS is the important part) ---
    char sta_ip_now[16] = "0.0.0.0";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta)
    {
        esp_netif_ip_info_t ipinfo;
        if (esp_netif_get_ip_info(sta, &ipinfo) == ESP_OK)
        {
            snprintf(sta_ip_now, sizeof(sta_ip_now), IPSTR, IP2STR(&ipinfo.ip));
        }
    }

    // Optional: get current connected SSID (more reliable than cached)
    wifi_ap_record_t apinfo;
    if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK)
    {
        strlcpy(g_sta_ssid, (const char *)apinfo.ssid, sizeof(g_sta_ssid));
        // If IP is non-zero, we are effectively connected
        if (strcmp(sta_ip_now, "0.0.0.0") != 0)
            g_sta_state = 2;
    }

    const char *state_str = "DISCONNECTED";
    if (g_sta_state == 1)
        state_str = "CONNECTING";
    else if (g_sta_state == 2)
        state_str = "CONNECTED";

    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"ssid\":\"%s\",\"ap_ip\":\"%s\",\"sta_ip\":\"%s\"}",
             state_str, g_sta_ssid, g_ap_ip, sta_ip_now);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_save_post(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body)))
    {
        json_send(req, "{\"ok\":false}");
        return ESP_OK;
    }

    // Tiny JSON parse (ssid/pass). For production: use cJSON.
    char ssid[WIFI_SSID_MAX + 1] = {0};
    char pass[WIFI_PASS_MAX + 1] = {0};

    // naive extraction: "ssid":"...."
    char *s = strstr(body, "\"ssid\"");
    char *p = strstr(body, "\"pass\"");
    if (!s)
    {
        json_send(req, "{\"ok\":false}");
        return ESP_OK;
    }

    sscanf(s, "\"ssid\":\"%32[^\"]\"", ssid);
    if (p)
        sscanf(p, "\"pass\":\"%64[^\"]\"", pass);

    bool ok = wifi_db_add_or_update(ssid, pass);
    json_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t api_delete_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body)))
    {
        json_send(req, "{\"ok\":false}");
        return ESP_OK;
    }

    char ssid[WIFI_SSID_MAX + 1] = {0};
    char *s = strstr(body, "\"ssid\"");
    if (!s)
    {
        json_send(req, "{\"ok\":false}");
        return ESP_OK;
    }
    sscanf(s, "\"ssid\":\"%32[^\"]\"", ssid);

    bool ok = wifi_db_delete(ssid);
    json_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

void web_cfg_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;

    static bool s_evt_reg_done = false;
    if (!s_evt_reg_done)
    {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &webcfg_wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &webcfg_wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &webcfg_wifi_event_handler, NULL);

        s_evt_reg_done = true;
    }

    ESP_LOGI(TAG, "Starting web server...");
    if (httpd_start(&s_srv, &cfg) != ESP_OK)
        return;

    httpd_uri_t u0 = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    httpd_uri_t u1 = {.uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_get};
    httpd_uri_t u2 = {.uri = "/api/known", .method = HTTP_GET, .handler = api_known_get};
    httpd_uri_t u3 = {.uri = "/api/save", .method = HTTP_POST, .handler = api_save_post};
    httpd_uri_t u4 = {.uri = "/api/delete", .method = HTTP_POST, .handler = api_delete_post};
    httpd_uri_t u5 = {.uri = "/api/connect", .method = HTTP_POST, .handler = api_connect_post};
    httpd_uri_t u6 = {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_get};

    httpd_register_uri_handler(s_srv, &u0);
    httpd_register_uri_handler(s_srv, &u1);
    httpd_register_uri_handler(s_srv, &u2);
    httpd_register_uri_handler(s_srv, &u3);
    httpd_register_uri_handler(s_srv, &u4);
    httpd_register_uri_handler(s_srv, &u5);
    httpd_register_uri_handler(s_srv, &u6);

    ESP_LOGI(TAG, "Web UI ready: http://192.168.4.1/");
}
