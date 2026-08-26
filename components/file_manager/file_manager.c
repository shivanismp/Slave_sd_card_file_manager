#include "file_manager.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#define FM_PATH_MAX 384
#define FM_QUERY_MAX 768
#define FM_IO_CHUNK 1024
#define FM_MAX_DIRECTORY_DEPTH 8
#define FM_TEMP_NAME ".fm_upload.tmp"
#define FM_BUILD_ID "FM_SD_CLASS_V3"

static const char *TAG = "FILE_MANAGER";

static httpd_handle_t s_server;
static bool s_owns_server;
static bool s_allow_mutation;
static size_t s_registered_handlers;
static file_manager_fs_t s_filesystem = FILE_MANAGER_FS_AUTO;
static char s_mount_point[64] = "/spiffs";
static char s_partition_label[64];
static sd_card_file_manager_t s_sd_manager;

volatile bool html_file_manager_enabled = false;

typedef enum {
    FM_STORAGE_INTERNAL = 0,
    FM_STORAGE_SD,
} fm_storage_t;

static const char INDEX_HTML_HEAD[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 File Manager</title><style>"
    ":root{color-scheme:dark}body{font:14px system-ui,sans-serif;max-width:1000px;"
    "margin:auto;padding:24px;background:#10151b;color:#edf3f8}h1{font-size:22px}"
    ".card{background:#18212b;border:1px solid #2c3a47;border-radius:10px;padding:16px;"
    "margin:14px 0}.muted{color:#a9b7c4}.bar{height:8px;background:#34424f;border-radius:8px;"
    "overflow:hidden;margin-top:8px}.bar>div{height:100%;background:#35b8ff;width:0}"
    "table{width:100%;border-collapse:collapse}th,td{padding:10px 8px;border-bottom:1px solid #2c3a47;"
    "text-align:left}th:nth-child(2),td:nth-child(2){text-align:right;white-space:nowrap}"
    "a,button{color:#55c4ff}button,input{background:#111820;color:#edf3f8;border:1px solid #3c5265;"
    "border-radius:6px;padding:8px}button{cursor:pointer}.danger{color:#ff7b86}"
    ".storage-tabs{display:flex;gap:8px;margin:14px 0}.storage-tabs button.selected{background:#17658e;color:white}"
    "#status{min-height:1.4em}.actions{white-space:nowrap}.actions>*{margin-left:10px}"
    "@media(max-width:620px){body{padding:12px}.actions{white-space:normal}.actions>*{display:block;margin:4px 0}}"
    "</style></head><body><h1>ESP32 File Manager</h1>"
    "<div class='storage-tabs'><button id='internalBtn' class='selected'>INTERNAL</button>"
    "<button id='sdBtn' hidden>SD CARD</button><button id='refreshBtn'>Refresh</button></div>"
    "<div class='card'><strong id='storageTitle'>Internal storage</strong>"
    "<div id='storage'>Loading storage information...</div>"
    "<div class='bar'><div id='used'></div></div></div>"
    "<div class='card' id='writebox' hidden><form id='uploadForm'>"
    "<input id='folder' placeholder='Existing folder (optional)'> "
    "<input id='file' type='file' required> <button>Upload</button></form>"
    "<div class='bar' id='uploadBar' hidden><div id='uploadUsed'></div></div></div>"
    "<p id='status' class='muted'></p><div class='card'><table><thead><tr>"
    "<th>Path</th><th>Size</th><th>Actions</th></tr></thead><tbody id='files'></tbody>"
    "</table></div><script>";

static const char INDEX_HTML_SCRIPT[] =
    "const api='/filemanager/api/';let writable=false,storageName='internal',sdAvailable=false;"
    "const fmt=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KiB':(n/1048576).toFixed(1)+' MiB';"
    "const status=m=>document.getElementById('status').textContent=m||'';"
    "const apiUrl=(action,path)=>api+action+'?storage='+encodeURIComponent(storageName)+(path===undefined?'':'&path='+encodeURIComponent(path));"
    "async function json(url,opt){const r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text()||r.statusText);return r.json()}"
    "function updateStorageButtons(){document.getElementById('internalBtn').classList.toggle('selected',storageName==='internal');"
    "document.getElementById('sdBtn').classList.toggle('selected',storageName==='sd')}"
    "async function selectStorage(next){storageName=next;updateStorageButtons();await refresh()}"
    "function showSd(show){sdAvailable=show;document.getElementById('sdBtn').hidden=!show}"
    "async function refreshInfo(){const d=await json(apiUrl('info'));writable=!!d.writable;"
    "showSd(!!d.sd_available);document.getElementById('writebox').hidden=!writable;"
    "document.getElementById('storageTitle').textContent=d.label;"
    "if(d.capacity_known){const pct=d.total?100*d.used/d.total:0;"
    "document.getElementById('storage').textContent=fmt(d.used)+' used of '+fmt(d.total)+' ('+fmt(d.free)+' free)';"
    "document.getElementById('used').style.width=Math.min(100,pct)+'%'}else{"
    "document.getElementById('storage').textContent=d.label+' is mounted';document.getElementById('used').style.width='0%'}}"
    "function action(text,href,cls){const a=document.createElement('a');a.textContent=text;a.href=href;if(cls)a.className=cls;return a}"
    "async function removeFile(name){if(!confirm('Delete '+name+'?'))return;"
    "try{const r=await fetch(apiUrl('delete',name),{method:'DELETE'});"
    "if(!r.ok)throw new Error(await r.text());await refresh()}catch(e){status('Delete failed: '+e.message)}}"
    "async function refresh(){status('Loading...');try{await refreshInfo();const d=await json(apiUrl('list'));"
    "const body=document.getElementById('files');body.replaceChildren();for(const f of d.files){"
    "const tr=document.createElement('tr'),name=document.createElement('td'),size=document.createElement('td'),acts=document.createElement('td');"
    "name.textContent=f.name;size.textContent=fmt(f.size);acts.className='actions';"
    "acts.append(action('View',apiUrl('view',f.name)));"
    "acts.append(action('Download',apiUrl('download',f.name)));"
    "if(writable){const del=action('Delete','#','danger');del.onclick=e=>{e.preventDefault();removeFile(f.name)};acts.append(del)}"
    "tr.append(name,size,acts);body.append(tr)}status(d.files.length+' file(s)')}catch(e){"
    "if(storageName==='sd'&&e.message.includes('not mounted')){storageName='internal';updateStorageButtons();"
    "showSd(false);status('SD card removed; returning to internal storage');setTimeout(refresh,0)}"
    "else status('Unable to load files: '+e.message)}}"
    "async function pollSd(){try{const d=await json(api+'info?storage=internal'),now=!!d.sd_available;"
    "if(now!==sdAvailable)showSd(now);if(!now&&storageName==='sd'){storageName='internal';updateStorageButtons();refresh()}}catch(e){}}"
    "document.getElementById('uploadForm').onsubmit=e=>{e.preventDefault();const file=document.getElementById('file').files[0];"
    "let folder=document.getElementById('folder').value.trim().replace(/^\\/+|\\/+$/g,'');"
    "const path=folder?folder+'/'+file.name:file.name,x=new XMLHttpRequest(),bar=document.getElementById('uploadBar');"
    "x.open('POST',apiUrl('upload',path));bar.hidden=false;"
    "x.upload.onprogress=p=>{if(p.lengthComputable)document.getElementById('uploadUsed').style.width=(100*p.loaded/p.total)+'%'};"
    "x.onload=()=>{bar.hidden=true;document.getElementById('uploadUsed').style.width='0';"
    "if(x.status>=400)status('Upload failed: '+x.responseText);else refresh()};x.onerror=()=>status('Upload connection failed');x.send(file)};"
    "document.getElementById('internalBtn').onclick=()=>selectStorage('internal');"
    "document.getElementById('sdBtn').onclick=()=>selectStorage('sd');"
    "document.getElementById('refreshBtn').onclick=()=>refresh();"
    "refresh();setInterval(pollSd,2000);</script></body></html>";

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

static esp_err_t send_text_error(httpd_req_t *req, const char *status,
                                 const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    set_common_headers(req);
    return httpd_resp_sendstr(req, message);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    while (*src) {
        unsigned char value;
        if (*src == '%') {
            if (src[1] == '\0' || src[2] == '\0') {
                return false;
            }
            int high = hex_value(src[1]);
            int low = hex_value(src[2]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)((high << 4) | low);
            src += 3;
        } else {
            value = (unsigned char)(*src == '+' ? ' ' : *src);
            src++;
        }
        if (value == 0 || out + 1 >= dst_size) {
            return false;
        }
        dst[out++] = (char)value;
    }
    dst[out] = '\0';
    return true;
}

static bool valid_relative_path(const char *path)
{
    if (!path || path[0] == '\0' || path[0] == '/' ||
        strcmp(path, FM_TEMP_NAME) == 0) {
        return false;
    }

    const char *segment = path;
    for (const unsigned char *p = (const unsigned char *)path;; p++) {
        if (*p == '\\' || (*p != '\0' && (*p < 0x20 || *p == 0x7f))) {
            return false;
        }
        if (*p == '/' || *p == '\0') {
            size_t length = (const char *)p - segment;
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (*p == '\0') {
                break;
            }
            segment = (const char *)p + 1;
        }
    }
    return true;
}

static bool get_request_path(httpd_req_t *req, char *relative,
                             size_t relative_size)
{
    size_t query_length = httpd_req_get_url_query_len(req);
    if (query_length == 0 || query_length + 1 > FM_QUERY_MAX) {
        return false;
    }

    char query[FM_QUERY_MAX];
    char encoded[FM_QUERY_MAX];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", encoded, sizeof(encoded)) != ESP_OK ||
        !url_decode(encoded, relative, relative_size)) {
        return false;
    }
    return valid_relative_path(relative);
}

static bool get_request_storage(httpd_req_t *req, fm_storage_t *storage)
{
    if (!req || !storage) {
        return false;
    }

    *storage = FM_STORAGE_INTERNAL;
    size_t query_length = httpd_req_get_url_query_len(req);
    if (query_length == 0) {
        return true; /* Backward-compatible default for existing API clients. */
    }
    if (query_length + 1 > FM_QUERY_MAX) {
        return false;
    }

    char query[FM_QUERY_MAX];
    char encoded[32];
    char decoded[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "storage", encoded, sizeof(encoded)) != ESP_OK) {
        return true; /* Requests containing only path= still use internal. */
    }
    if (!url_decode(encoded, decoded, sizeof(decoded))) {
        return false;
    }
    if (strcmp(decoded, "internal") == 0) {
        *storage = FM_STORAGE_INTERNAL;
        return true;
    }
    if (strcmp(decoded, "sd") == 0) {
        *storage = FM_STORAGE_SD;
        return true;
    }
    return false;
}

static bool sd_storage_available(void)
{
    return sd_card_file_manager_is_available(&s_sd_manager);
}

static const char *storage_mount_point(fm_storage_t storage)
{
    return storage == FM_STORAGE_SD
               ? sd_card_file_manager_mount_point(&s_sd_manager)
               : s_mount_point;
}

static bool make_full_path(fm_storage_t storage, const char *relative,
                           char *full, size_t full_size)
{
    if (storage == FM_STORAGE_SD) {
        return sd_card_file_manager_resolve_path(&s_sd_manager, relative,
                                                 full, full_size) == ESP_OK;
    }
    int written = snprintf(full, full_size, "%s/%s",
                           s_mount_point, relative);
    return written > 0 && (size_t)written < full_size;
}

// static const char *partition_label(void)
// {
//     return s_partition_label[0] ? s_partition_label : NULL;
// }

// static esp_err_t filesystem_info(fm_storage_t storage, uint64_t *total,
//                                  uint64_t *used)
// {
//     if (storage == FM_STORAGE_SD) {
//         uint64_t free_bytes = 0;
//         return sd_card_file_manager_get_space(
//             &s_sd_manager, total, used, &free_bytes);
//     }

//     size_t littlefs_total = 0;
//     size_t littlefs_used = 0;
//     esp_err_t err = esp_littlefs_info("storage", &littlefs_total, &littlefs_used);

//     if (s_filesystem == FILE_MANAGER_FS_LITTLEFS) {
//         err = esp_littlefs_info(partition_label(), &littlefs_total,
//                                 &littlefs_used);
//     } else if (s_filesystem == FILE_MANAGER_FS_SPIFFS) {
//         err = esp_spiffs_info(partition_label(), &littlefs_total,
//                               &littlefs_used);
//     } else {
//         err = esp_littlefs_info(partition_label(), &littlefs_total,
//                                 &littlefs_used);
//         if (err != ESP_OK) {
//             err = esp_spiffs_info(partition_label(), &littlefs_total,
//                                   &littlefs_used);
//         }
//     }
//     if (err == ESP_OK) {
//         *total = littlefs_total;
//         *used = littlefs_used;
//     }
//     return err;
// }



/*
           TO DISPLAY THE SIZE USED AND TOTAL SIZE OF INTERNAL ESP32        */
static esp_err_t filesystem_info(fm_storage_t storage,
                                 uint64_t *total,
                                 uint64_t *used)
{
    if (storage == FM_STORAGE_SD) {
        uint64_t free_bytes = 0;
        return sd_card_file_manager_get_space(
            &s_sd_manager, total, used, &free_bytes);
    }

    size_t littlefs_total = 0;
    size_t littlefs_used = 0;

    esp_err_t err = esp_littlefs_info("storage",
                                      &littlefs_total,
                                      &littlefs_used);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS info failed for storage: %s",
                 esp_err_to_name(err));
        return err;
    }

    *total = (uint64_t)littlefs_total;
    *used = (uint64_t)littlefs_used;
    return ESP_OK;
}


static esp_err_t send_json_string(httpd_req_t *req, const char *value)
{
    char out[128];
    size_t used = 0;
    out[used++] = '"';

    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        char escaped[7];
        size_t escaped_length = 0;
        switch (*p) {
        case '"': escaped[0] = '\\'; escaped[1] = '"'; escaped_length = 2; break;
        case '\\': escaped[0] = '\\'; escaped[1] = '\\'; escaped_length = 2; break;
        case '\b': escaped[0] = '\\'; escaped[1] = 'b'; escaped_length = 2; break;
        case '\f': escaped[0] = '\\'; escaped[1] = 'f'; escaped_length = 2; break;
        case '\n': escaped[0] = '\\'; escaped[1] = 'n'; escaped_length = 2; break;
        case '\r': escaped[0] = '\\'; escaped[1] = 'r'; escaped_length = 2; break;
        case '\t': escaped[0] = '\\'; escaped[1] = 't'; escaped_length = 2; break;
        default:
            if (*p < 0x20) {
                escaped_length = (size_t)snprintf(escaped, sizeof(escaped),
                                                  "\\u%04x", *p);
            } else {
                escaped[0] = (char)*p;
                escaped_length = 1;
            }
            break;
        }

        if (used + escaped_length >= sizeof(out)) {
            if (httpd_resp_send_chunk(req, out, used) != ESP_OK) {
                return ESP_FAIL;
            }
            used = 0;
        }
        memcpy(out + used, escaped, escaped_length);
        used += escaped_length;
    }

    out[used++] = '"';
    return httpd_resp_send_chunk(req, out, used);
}

static esp_err_t emit_file_json(httpd_req_t *req, const char *relative,
                                uint64_t size, bool *first)
{
    char prefix[48];
    int length = snprintf(prefix, sizeof(prefix), "%s{\"name\":",
                          *first ? "" : ",");
    if (length <= 0 || (size_t)length >= sizeof(prefix) ||
        httpd_resp_send_chunk(req, prefix, length) != ESP_OK ||
        send_json_string(req, relative) != ESP_OK) {
        return ESP_FAIL;
    }

    length = snprintf(prefix, sizeof(prefix), ",\"size\":%" PRIu64 "}", size);
    if (length <= 0 || (size_t)length >= sizeof(prefix) ||
        httpd_resp_send_chunk(req, prefix, length) != ESP_OK) {
        return ESP_FAIL;
    }
    *first = false;
    return ESP_OK;
}

static esp_err_t list_directory(httpd_req_t *req, const char *absolute_dir,
                                const char *relative_dir, unsigned depth,
                                bool *first)
{
    if (depth > FM_MAX_DIRECTORY_DEPTH) {
        return ESP_OK;
    }

    DIR *dir = opendir(absolute_dir);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open directory %s (errno=%d)", absolute_dir, errno);
        return depth == 0 ? ESP_FAIL : ESP_OK;
    }

    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, FM_TEMP_NAME) == 0) {
            continue;
        }

        char absolute[FM_PATH_MAX];
        char relative[FM_PATH_MAX];
        int abs_length = snprintf(absolute, sizeof(absolute), "%s/%s",
                                  absolute_dir, entry->d_name);
        int rel_length = relative_dir[0]
            ? snprintf(relative, sizeof(relative), "%s/%s", relative_dir,
                       entry->d_name)
            : snprintf(relative, sizeof(relative), "%s", entry->d_name);
        if (abs_length <= 0 || (size_t)abs_length >= sizeof(absolute) ||
            rel_length <= 0 || (size_t)rel_length >= sizeof(relative)) {
            ESP_LOGW(TAG, "Skipping overlong path under %s", absolute_dir);
            continue;
        }

        struct stat st;
        if (stat(absolute, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            result = list_directory(req, absolute, relative, depth + 1, first);
        } else if (S_ISREG(st.st_mode)) {
            result = emit_file_json(req, relative, (uint64_t)st.st_size, first);
        }
        if (result != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    return result;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (httpd_resp_send_chunk(req, INDEX_HTML_HEAD, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, INDEX_HTML_SCRIPT, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    fm_storage_t storage;
    if (!get_request_storage(req, &storage)) {
        return send_text_error(req, "400 Bad Request", "Invalid storage");
    }

    bool sd_available = sd_storage_available();
    if (storage == FM_STORAGE_SD && !sd_available) {
        return send_text_error(req, "503 Service Unavailable",
                               "SD card is not mounted");
    }

    uint64_t total = 0;
    uint64_t used = 0;
    bool capacity_known = filesystem_info(storage, &total, &used) == ESP_OK;
    if (!capacity_known && storage == FM_STORAGE_INTERNAL) {
        ESP_LOGE(TAG, "Unable to read filesystem information");
    }
    uint64_t free_bytes = total > used ? total - used : 0;

    char json[320];
    int length = snprintf(json, sizeof(json),
                          "{\"build\":\"" FM_BUILD_ID "\",\"storage\":\"%s\","
                          "\"label\":\"%s\","
                          "\"available\":true,\"sd_available\":%s,"
                          "\"capacity_known\":%s,\"total\":%" PRIu64 ","
                          "\"used\":%" PRIu64 ",\"free\":%" PRIu64 ","
                          "\"writable\":%s}",
                          storage == FM_STORAGE_SD ? "sd" : "internal",
                          storage == FM_STORAGE_SD ? "SD CARD" : "Internal storage",
                          sd_available ? "true" : "false",
                          capacity_known ? "true" : "false",
                          total, used, free_bytes,
                          s_allow_mutation ? "true" : "false");
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, length);
}

static esp_err_t list_get_handler(httpd_req_t *req)
{
    fm_storage_t storage;
    if (!get_request_storage(req, &storage)) {
        return send_text_error(req, "400 Bad Request", "Invalid storage");
    }
    if (storage == FM_STORAGE_SD && !sd_storage_available()) {
        return send_text_error(req, "503 Service Unavailable",
                               "SD card is not mounted");
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send_chunk(req, "{\"files\":[", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }

    bool first = true;
    esp_err_t result = list_directory(req, storage_mount_point(storage),
                                      "", 0, &first);
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(req, "]}", 2);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

static void make_download_header(const char *relative, char *header,
                                 size_t header_size)
{
    const char *base = strrchr(relative, '/');
    base = base ? base + 1 : relative;

    char safe_name[96];
    size_t i = 0;
    while (base[i] && i + 1 < sizeof(safe_name)) {
        unsigned char c = (unsigned char)base[i];
        safe_name[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
                           ? (char)c
                           : '_';
        i++;
    }
    safe_name[i] = '\0';
    snprintf(header, header_size, "attachment; filename=\"%s\"", safe_name);
}

static esp_err_t send_file(httpd_req_t *req, bool inline_view)
{
    fm_storage_t storage;
    char relative[FM_PATH_MAX];
    char full_path[FM_PATH_MAX];
    if (!get_request_storage(req, &storage)) {
        return send_text_error(req, "400 Bad Request", "Invalid storage");
    }
    if (storage == FM_STORAGE_SD && !sd_storage_available()) {
        return send_text_error(req, "503 Service Unavailable",
                               "SD card is not mounted");
    }
    if (!get_request_path(req, relative, sizeof(relative)) ||
        !make_full_path(storage, relative, full_path, sizeof(full_path))) {
        return send_text_error(req, "400 Bad Request", "Invalid path");
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_text_error(req, "404 Not Found", "File not found");
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        return send_text_error(req, "404 Not Found", "File not found");
    }

    set_common_headers(req);
    if (inline_view) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
    } else {
        char disposition[144];
        make_download_header(relative, disposition, sizeof(disposition));
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    }

    char chunk[FM_IO_CHUNK];
    esp_err_t result = ESP_OK;
    size_t bytes;
    while ((bytes = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, bytes) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

static esp_err_t view_get_handler(httpd_req_t *req)
{
    return send_file(req, true);
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    return send_file(req, false);
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    fm_storage_t storage;
    if (!get_request_storage(req, &storage)) {
        return send_text_error(req, "400 Bad Request", "Invalid storage");
    }
    if (storage == FM_STORAGE_SD && !sd_storage_available()) {
        return send_text_error(req, "503 Service Unavailable",
                               "SD card is not mounted");
    }
    if (!s_allow_mutation) {
        return send_text_error(req, "403 Forbidden", "File changes are disabled");
    }

    char relative[FM_PATH_MAX];
    char destination[FM_PATH_MAX];
    if (!get_request_path(req, relative, sizeof(relative)) ||
        !make_full_path(storage, relative, destination, sizeof(destination))) {
        return send_text_error(req, "400 Bad Request", "Invalid path");
    }

    uint64_t total = 0;
    uint64_t used = 0;
    if (filesystem_info(storage, &total, &used) == ESP_OK) {
        uint64_t free_bytes = total > used ? total - used : 0;
        if ((uint64_t)req->content_len > free_bytes) {
            return send_text_error(req, "507 Insufficient Storage",
                                   "Not enough free storage");
        }
    }

    char temporary[FM_PATH_MAX];
    if (!make_full_path(storage, FM_TEMP_NAME, temporary, sizeof(temporary))) {
        return send_text_error(req, "500 Internal Server Error", "Invalid mount path");
    }
    unlink(temporary);

    FILE *file = fopen(temporary, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Cannot open temporary upload file (errno=%d)", errno);
        return send_text_error(req, "500 Internal Server Error",
                               "Cannot create upload file");
    }

    char buffer[FM_IO_CHUNK];
    size_t remaining = req->content_len;
    bool failed = false;
    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int received = httpd_req_recv(req, buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0 || fwrite(buffer, 1, (size_t)received, file) !=
                                 (size_t)received) {
            failed = true;
            break;
        }
        remaining -= (size_t)received;
    }
    if (fflush(file) != 0) {
        failed = true;
    }
    fclose(file);

    if (failed) {
        unlink(temporary);
        ESP_LOGE(TAG, "Upload failed for %s (errno=%d)", relative, errno);
        return send_text_error(req, "500 Internal Server Error", "Upload failed");
    }

    if (rename(temporary, destination) != 0) {
        int saved_errno = errno;
        unlink(temporary);
        ESP_LOGE(TAG, "Cannot install upload %s (errno=%d)", relative, saved_errno);
        return send_text_error(req, "409 Conflict",
                               "Destination folder is missing or file cannot be replaced");
    }

    ESP_LOGI(TAG, "Uploaded %s (%u bytes)", relative, (unsigned)req->content_len);
    set_common_headers(req);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    fm_storage_t storage;
    if (!get_request_storage(req, &storage)) {
        return send_text_error(req, "400 Bad Request", "Invalid storage");
    }
    if (storage == FM_STORAGE_SD && !sd_storage_available()) {
        return send_text_error(req, "503 Service Unavailable",
                               "SD card is not mounted");
    }
    if (!s_allow_mutation) {
        return send_text_error(req, "403 Forbidden", "File changes are disabled");
    }

    char relative[FM_PATH_MAX];
    char full_path[FM_PATH_MAX];
    if (!get_request_path(req, relative, sizeof(relative)) ||
        !make_full_path(storage, relative, full_path, sizeof(full_path))) {
        return send_text_error(req, "400 Bad Request", "Invalid path");
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        unlink(full_path) != 0) {
        return send_text_error(req, "404 Not Found", "File not found");
    }

    ESP_LOGI(TAG, "Deleted %s", relative);
    set_common_headers(req);
    return httpd_resp_sendstr(req, "OK");
}

static const httpd_uri_t FILE_MANAGER_URIS[] = {
    {.uri = "/filemanager", .method = HTTP_GET, .handler = index_get_handler},
    {.uri = "/filemanager/api/info", .method = HTTP_GET, .handler = info_get_handler},
    {.uri = "/filemanager/api/list", .method = HTTP_GET, .handler = list_get_handler},
    {.uri = "/filemanager/api/view", .method = HTTP_GET, .handler = view_get_handler},
    {.uri = "/filemanager/api/download", .method = HTTP_GET, .handler = download_get_handler},
    {.uri = "/filemanager/api/upload", .method = HTTP_POST, .handler = upload_post_handler},
    {.uri = "/filemanager/api/delete", .method = HTTP_DELETE, .handler = delete_handler},
};

static void unregister_handlers(void)
{
    if (!s_server) {
        s_registered_handlers = 0;
        return;
    }
    while (s_registered_handlers > 0) {
        s_registered_handlers--;
        httpd_unregister_uri_handler(s_server,
                                     FILE_MANAGER_URIS[s_registered_handlers].uri,
                                     FILE_MANAGER_URIS[s_registered_handlers].method);
    }
}

static esp_err_t mount_spiffs(const file_manager_config_t *config)
{
    if (esp_spiffs_mounted(config->partition_label)) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = config->mount_point,
        .partition_label = config->partition_label,
        .max_files = 10,
        .format_if_mount_failed = config->format_if_mount_failed,
    };
    return esp_vfs_spiffs_register(&conf);
}

esp_err_t file_manager_init(const file_manager_config_t *config)
{
    if (s_server) {
        return ESP_ERR_INVALID_STATE;
    }

    file_manager_config_t cfg = config ? *config
                                       : (file_manager_config_t)FILE_MANAGER_DEFAULT_CONFIG();
    if (!cfg.mount_point) {
        cfg.mount_point = "/spiffs";
    }
    if (cfg.port == 0) {
        cfg.port = 8080;
    }
    if (cfg.control_port == 0) {
        cfg.control_port = 32769;
    }
    if (cfg.mount_point[0] != '/' || strcmp(cfg.mount_point, "/") == 0 ||
        strlen(cfg.mount_point) >= sizeof(s_mount_point) ||
        (cfg.partition_label &&
         strlen(cfg.partition_label) >= sizeof(s_partition_label))) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_mount_point, sizeof(s_mount_point), "%s", cfg.mount_point);
    size_t mount_length = strlen(s_mount_point);
    while (mount_length > 1 && s_mount_point[mount_length - 1] == '/') {
        s_mount_point[--mount_length] = '\0';
    }
    snprintf(s_partition_label, sizeof(s_partition_label), "%s",
             cfg.partition_label ? cfg.partition_label : "");
    s_filesystem = cfg.filesystem;
    s_allow_mutation = cfg.allow_mutation;

    if (cfg.mount_spiffs) {
        esp_err_t err = mount_spiffs(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        struct stat st;
        if (stat(s_mount_point, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGE(TAG, "Filesystem is not mounted at %s", s_mount_point);
            return ESP_ERR_INVALID_STATE;
        }
    }

    sd_card_file_manager_deinit(&s_sd_manager);
    if (cfg.sd_mount_point) {
        sd_card_file_manager_config_t sd_config =
            SD_CARD_FILE_MANAGER_DEFAULT_CONFIG();
        sd_config.mount_point = cfg.sd_mount_point;
        sd_config.available = cfg.sd_available;
        esp_err_t err = sd_card_file_manager_init(&s_sd_manager, &sd_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Invalid SD-card configuration: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    if (html_file_manager_enabled)
    {

   
        if (cfg.server_handle) {
            s_server = cfg.server_handle;
            s_owns_server = false;
        } else {
            httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
            http_config.server_port = cfg.port;
            http_config.ctrl_port = cfg.control_port;
            http_config.stack_size = 12288;
            http_config.max_uri_handlers = 10;
            http_config.lru_purge_enable = true;
            esp_err_t err = httpd_start(&s_server, &http_config);
            if (err != ESP_OK) {
                s_server = NULL;
                sd_card_file_manager_deinit(&s_sd_manager);
                ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
                return err;
            }
            s_owns_server = true;
        }

        for (size_t i = 0; i < sizeof(FILE_MANAGER_URIS) / sizeof(FILE_MANAGER_URIS[0]); i++) {
            esp_err_t err = httpd_register_uri_handler(s_server, &FILE_MANAGER_URIS[i]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Cannot register %s: %s", FILE_MANAGER_URIS[i].uri,
                        esp_err_to_name(err));
                unregister_handlers();
                if (s_owns_server) {
                    httpd_stop(s_server);
                }
                s_server = NULL;
                s_owns_server = false;
                sd_card_file_manager_deinit(&s_sd_manager);
                return err;
            }
            s_registered_handlers++;
        }
        ESP_LOGI(TAG,
                 "File manager %s ready at http://<device-ip>:%u/filemanager (%s)",
                 FM_BUILD_ID,
                 (unsigned)cfg.port,
                 s_allow_mutation ? "read/write" : "read-only");
    }
    
    const char *sd_mount_point =
        sd_card_file_manager_mount_point(&s_sd_manager);

        if(html_file_manager_enabled)
        {
            if (sd_mount_point) 
                ESP_LOGI(TAG, "SD browser enabled at %s (%s)",
                            sd_mount_point,
                            sd_storage_available() ? "mounted" : "not mounted");
        }
    
    return ESP_OK;
}

void file_manager_stop(void)
{
    if (!s_server) {
        return;
    }
    if (s_owns_server) {
        httpd_stop(s_server);
        s_registered_handlers = 0;
    } else {
        unregister_handlers();
    }
    s_server = NULL;
    s_owns_server = false;
    sd_card_file_manager_deinit(&s_sd_manager);
}

bool file_manager_is_running(void)
{
    return s_server != NULL &&
           s_registered_handlers == sizeof(FILE_MANAGER_URIS) / sizeof(FILE_MANAGER_URIS[0]);
}