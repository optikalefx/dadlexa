#include "web_admin.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_music_library.h"

#define MAX_UPLOAD_BYTES (8 * 1024 * 1024)
#define REBOOT_DELAY_MS 3000

static const char *TAG = "web_admin";
static httpd_handle_t server;

static const char index_html[] =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Jacob Speaker</title><style>"
    "body{font-family:system-ui,-apple-system,sans-serif;margin:24px;max-width:760px}"
    "label{display:block;margin:14px 0 6px}input,button{font:inherit;padding:8px;width:100%;box-sizing:border-box}"
    "button{margin-top:16px;width:auto}.grid{display:grid;grid-template-columns:1fr 1fr;gap:24px}"
    "progress{display:none;width:100%;height:18px;margin-top:14px}"
    "progress.active{display:block}"
    "pre{background:#f3f3f3;padding:12px;overflow:auto}@media(max-width:640px){.grid{grid-template-columns:1fr}}"
    "</style></head><body><h1>Jacob Speaker</h1>"
    "<form id='upload' enctype='multipart/form-data'>"
    "<label>Phrase</label><input name='phrase' required placeholder='play milkshake'>"
    "<label>Song MP3</label><input name='song' type='file' accept='.mp3,audio/mpeg' required>"
    "<button id='submit'>Upload and reboot</button><progress id='progress' max='100' value='0'></progress></form><p id='status'></p>"
    "<div class='grid'><section><h2>Manifest</h2><pre id='manifest'>Loading...</pre></section>"
    "<section><h2>Songs</h2><pre id='songs'>Loading...</pre></section></div>"
    "<script>"
    "async function load(){const r=await fetch('/api/list',{cache:'no-store'});const j=await r.json();"
    "manifest.textContent=(j.manifest||[]).map(x=>x.phrase+' | '+x.song).join('\\n')||'(empty)';"
    "songs.textContent=(j.songs||[]).join('\\n')||'(empty)'}"
    "upload.onsubmit=e=>{e.preventDefault();submit.disabled=true;progress.className='active';progress.removeAttribute('value');"
    "status.textContent='Starting upload...';const x=new XMLHttpRequest();"
    "x.upload.onprogress=e=>{if(e.lengthComputable){progress.value=Math.round(e.loaded*100/e.total);"
    "status.textContent='Uploading '+progress.value+'%';}else{status.textContent='Uploading...';}};"
    "x.onload=async()=>{status.textContent=x.responseText||'Upload complete.';"
    "if(x.status>=200&&x.status<300){progress.value=100;try{await load();}catch(e){}"
    "setTimeout(()=>location.reload(),4500);status.textContent+=' Device is rebooting.';}"
    "else{submit.disabled=false;progress.className='';}};"
    "x.onerror=()=>{status.textContent='Upload failed.';submit.disabled=false;progress.className='';};"
    "x.open('POST','/api/upload');x.send(new FormData(upload));};"
    "load();</script></body></html>";

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const char *needle, size_t needle_len)
{
    if (needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static bool valid_filename(const char *filename)
{
    size_t len = strlen(filename);
    if (len < 5 || len >= SD_MAX_SONG_PATH_LEN) {
        return false;
    }
    if (strcasecmp(filename + len - 4, ".mp3") != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)filename[i];
        if (isalnum(ch) || ch == ' ' || ch == '_' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

static bool valid_phrase(const char *phrase)
{
    size_t len = strlen(phrase);
    if (len == 0 || len >= SD_MAX_PHRASE_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)phrase[i];
        if (isalnum(ch) || ch == ' ' || ch == '\'' || ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static void json_add_manifest(cJSON *root)
{
    cJSON *manifest = cJSON_AddArrayToObject(root, "manifest");
    FILE *file = fopen(SD_MANIFEST_PATH, "r");
    if (!file) {
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *clean = trim(line);
        if (*clean == '\0' || *clean == '#') {
            continue;
        }
        char *sep = strchr(clean, '|');
        if (!sep) {
            continue;
        }
        *sep = '\0';
        char *phrase = trim(clean);
        char *song = trim(sep + 1);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "phrase", phrase);
        cJSON_AddStringToObject(item, "song", song);
        cJSON_AddItemToArray(manifest, item);
    }
    fclose(file);
}

static void json_add_songs(cJSON *root)
{
    cJSON *songs = cJSON_AddArrayToObject(root, "songs");
    DIR *dir = opendir(SD_SONGS_DIR);
    if (!dir) {
        return;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        size_t len = strlen(entry->d_name);
        if (len >= 5 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
            cJSON_AddItemToArray(songs, cJSON_CreateString(entry->d_name));
        }
    }
    closedir(dir);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t list_get_handler(httpd_req_t *req)
{
    esp_err_t mount_err = sd_music_library_mount();
    if (mount_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card not available");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "json root");
    json_add_manifest(root);
    json_add_songs(root);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "json print");

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t receive_body(httpd_req_t *req, uint8_t **body, size_t *body_len)
{
    ESP_RETURN_ON_FALSE(req->content_len > 0, ESP_ERR_INVALID_ARG, TAG, "empty upload");
    ESP_RETURN_ON_FALSE(req->content_len <= MAX_UPLOAD_BYTES, ESP_ERR_INVALID_SIZE, TAG,
                        "upload too large: %d", req->content_len);

    uint8_t *buffer = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = malloc(req->content_len + 1);
    }
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "upload buffer");

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int read = httpd_req_recv(req, (char *)buffer + received, req->content_len - received);
        if (read <= 0) {
            free(buffer);
            if (read == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
                return ESP_ERR_TIMEOUT;
            }
            return ESP_FAIL;
        }
        received += (size_t)read;
    }
    buffer[received] = '\0';
    *body = buffer;
    *body_len = received;
    return ESP_OK;
}

static bool header_value(const char *headers, const char *key, char *out, size_t out_size)
{
    const char *found = strstr(headers, key);
    if (!found) {
        return false;
    }
    found += strlen(key);
    const char *end = strchr(found, '"');
    if (!end || end == found) {
        return false;
    }
    size_t len = (size_t)(end - found);
    if (len >= out_size) {
        return false;
    }
    memcpy(out, found, len);
    out[len] = '\0';
    return true;
}

static esp_err_t parse_multipart(const uint8_t *body, size_t body_len, const char *boundary,
                                 char *phrase, size_t phrase_size, char *filename,
                                 size_t filename_size, const uint8_t **file_data,
                                 size_t *file_len)
{
    char delimiter[96];
    snprintf(delimiter, sizeof(delimiter), "--%s", boundary);
    size_t delimiter_len = strlen(delimiter);
    const uint8_t *cursor = find_bytes(body, body_len, delimiter, delimiter_len);
    ESP_RETURN_ON_FALSE(cursor, ESP_ERR_INVALID_ARG, TAG, "missing first boundary");

    while (cursor) {
        cursor += delimiter_len;
        if ((size_t)(cursor - body) + 2 <= body_len && cursor[0] == '-' && cursor[1] == '-') {
            break;
        }
        if ((size_t)(cursor - body) + 2 > body_len || cursor[0] != '\r' || cursor[1] != '\n') {
            return ESP_ERR_INVALID_ARG;
        }
        cursor += 2;

        const uint8_t *headers_end = find_bytes(cursor, body_len - (size_t)(cursor - body),
                                                "\r\n\r\n", 4);
        ESP_RETURN_ON_FALSE(headers_end, ESP_ERR_INVALID_ARG, TAG, "part headers");
        size_t headers_len = (size_t)(headers_end - cursor);
        char *headers = calloc(1, headers_len + 1);
        ESP_RETURN_ON_FALSE(headers, ESP_ERR_NO_MEM, TAG, "headers");
        memcpy(headers, cursor, headers_len);

        const uint8_t *content = headers_end + 4;
        const uint8_t *next = find_bytes(content, body_len - (size_t)(content - body),
                                         "\r\n--", 4);
        if (!next) {
            free(headers);
            return ESP_ERR_INVALID_ARG;
        }
        size_t content_len = (size_t)(next - content);

        char name[32] = {0};
        char part_filename[SD_MAX_SONG_PATH_LEN] = {0};
        (void)header_value(headers, "name=\"", name, sizeof(name));
        bool has_filename = header_value(headers, "filename=\"", part_filename,
                                         sizeof(part_filename));

        if (strcmp(name, "phrase") == 0) {
            size_t copy = content_len < phrase_size - 1 ? content_len : phrase_size - 1;
            memcpy(phrase, content, copy);
            phrase[copy] = '\0';
            char *clean = trim(phrase);
            if (clean != phrase) {
                memmove(phrase, clean, strlen(clean) + 1);
            }
        } else if (strcmp(name, "song") == 0 && has_filename) {
            strlcpy(filename, part_filename, filename_size);
            *file_data = content;
            *file_len = content_len;
        }

        free(headers);
        cursor = find_bytes(next + 2, body_len - (size_t)(next + 2 - body), delimiter,
                            delimiter_len);
    }

    ESP_RETURN_ON_FALSE(phrase[0] && filename[0] && *file_data && *file_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "missing upload fields");
    return ESP_OK;
}

static esp_err_t save_upload(const char *phrase, const char *filename, const uint8_t *file_data,
                             size_t file_len)
{
    ESP_RETURN_ON_ERROR(sd_music_library_mount(), TAG, "sd");
    (void)mkdir(SD_SONGS_DIR, 0775);

    char tmp_path[SD_MAX_SONG_PATH_LEN + 24];
    char final_path[SD_MAX_SONG_PATH_LEN + 16];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.upload.tmp", SD_SONGS_DIR);
    snprintf(final_path, sizeof(final_path), "%s/%s", SD_SONGS_DIR, filename);

    FILE *file = fopen(tmp_path, "wb");
    ESP_RETURN_ON_FALSE(file, ESP_FAIL, TAG, "open %s", tmp_path);
    size_t written = fwrite(file_data, 1, file_len, file);
    fclose(file);
    ESP_RETURN_ON_FALSE(written == file_len, ESP_FAIL, TAG, "write %s", tmp_path);

    (void)remove(final_path);
    ESP_RETURN_ON_FALSE(rename(tmp_path, final_path) == 0, ESP_FAIL, TAG, "rename upload");

    FILE *manifest = fopen(SD_MANIFEST_PATH, "a");
    ESP_RETURN_ON_FALSE(manifest, ESP_FAIL, TAG, "open manifest");
    int printed = fprintf(manifest, "\n%s|%s", phrase, filename);
    fclose(manifest);
    ESP_RETURN_ON_FALSE(printed > 0, ESP_FAIL, TAG, "append manifest");
    ESP_LOGI(TAG, "uploaded %s phrase=\"%s\" bytes=%u", filename, phrase, (unsigned)file_len);
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char content_type[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) !=
        ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Type");
        return ESP_OK;
    }
    char *boundary = strstr(content_type, "boundary=");
    if (!boundary) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing multipart boundary");
        return ESP_OK;
    }
    boundary += strlen("boundary=");

    uint8_t *body = NULL;
    size_t body_len = 0;
    esp_err_t err = receive_body(req, &body, &body_len);
    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload receive failed");
        }
        return ESP_OK;
    }

    char phrase[SD_MAX_PHRASE_LEN] = {0};
    char filename[SD_MAX_SONG_PATH_LEN] = {0};
    const uint8_t *file_data = NULL;
    size_t file_len = 0;
    err = parse_multipart(body, body_len, boundary, phrase, sizeof(phrase), filename,
                          sizeof(filename), &file_data, &file_len);
    if (err == ESP_OK && (!valid_phrase(phrase) || !valid_filename(filename))) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        err = save_upload(phrase, filename, file_data, file_len);
    }
    free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "upload failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Uploaded. Rebooting now.");
    xTaskCreatePinnedToCore(reboot_task, "admin_reboot", 2048, NULL, 5, NULL, 0);
    return ESP_OK;
}

esp_err_t web_admin_start(void)
{
    if (server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "start");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t list = {
        .uri = "/api/list",
        .method = HTTP_GET,
        .handler = list_get_handler,
    };
    const httpd_uri_t upload = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "root");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &list), TAG, "list");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &upload), TAG, "upload");
    ESP_LOGI(TAG, "started on port %u", config.server_port);
    return ESP_OK;
}
