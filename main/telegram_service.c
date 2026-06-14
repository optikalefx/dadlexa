#include "telegram_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "telegram";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max;
} response_buffer_t;

/* Telegram/API data-prep helper: builds a bot API method URL from the configured
 * token; the caller performs the actual HTTP request. */
static char *make_bot_url(const char *method)
{
    const app_config_t *cfg = app_config_get();
    size_t len = strlen("https://api.telegram.org/bot") + strlen(cfg->telegram_bot_token) +
                 strlen("/") + strlen(method) + 1;
    char *url = malloc(len);
    if (url) {
        snprintf(url, len, "https://api.telegram.org/bot%s/%s", cfg->telegram_bot_token, method);
    }
    return url;
}

/* Telegram/API data-prep helper: builds a file-download URL for a Telegram
 * file_path returned by getFile. */
static char *make_file_url(const char *path)
{
    const app_config_t *cfg = app_config_get();
    size_t len = strlen("https://api.telegram.org/file/bot") + strlen(cfg->telegram_bot_token) +
                 strlen("/") + strlen(path) + 1;
    char *url = malloc(len);
    if (url) {
        snprintf(url, len, "https://api.telegram.org/file/bot%s/%s", cfg->telegram_bot_token, path);
    }
    return url;
}

/* Network data-prep helper: grows the HTTP response buffer and appends incoming
 * bytes without interpreting Telegram JSON yet. */
static bool append_response(response_buffer_t *buf, const char *data, size_t len)
{
    if (buf->len + len + 1 > buf->max) {
        return false;
    }
    if (buf->len + len + 1 > buf->cap) {
        size_t next = buf->cap == 0 ? 1024 : buf->cap * 2;
        while (next < buf->len + len + 1) {
            next *= 2;
        }
        if (next > buf->max) {
            next = buf->max;
        }
        char *resized = realloc(buf->data, next);
        if (!resized) {
            return false;
        }
        buf->data = resized;
        buf->cap = next;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

/* Telegram/API boundary callback: receives chunks from esp_http_client and
 * stores them for the higher-level Telegram parser. */
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
        response_buffer_t *buf = (response_buffer_t *)evt->user_data;
        if (!append_response(buf, evt->data, evt->data_len)) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* Telegram/API boundary: performs a TLS HTTP GET and collects the response body
 * for Telegram API calls or Telegram file downloads. */
static esp_err_t http_get_text(const char *url, response_buffer_t *response)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = http_event,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "http init");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_RETURN_ON_ERROR(err, TAG, "http get");
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

/* Telegram/API data-prep helper: form-encodes text fields before they are sent
 * to Telegram's application/x-www-form-urlencoded endpoints. */
static char *url_encode(const char *input)
{
    size_t len = strlen(input);
    char *out = malloc(len * 3 + 1);
    if (!out) {
        return NULL;
    }
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else if (c == ' ') {
            *p++ = '+';
        } else {
            snprintf(p, 4, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}

/* Telegram/API boundary: sends a plain chat message, used for online status and
 * recording failure notifications. */
esp_err_t telegram_send_message(const char *message)
{
    char *url = make_bot_url("sendMessage");
    char *chat = url_encode(app_config_get()->telegram_chat_id);
    char *text = url_encode(message);
    ESP_RETURN_ON_FALSE(url && chat && text, ESP_ERR_NO_MEM, TAG, "message alloc");

    size_t body_len = strlen("chat_id=&text=") + strlen(chat) + strlen(text) + 1;
    char *body = malloc(body_len);
    if (!body) {
        free(url);
        free(chat);
        free(text);
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, body_len, "chat_id=%s&text=%s", chat, text);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err = ESP_FAIL;
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, body, strlen(body));
        err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (err == ESP_OK && (status < 200 || status >= 300)) {
            err = ESP_FAIL;
        }
        esp_http_client_cleanup(client);
    }
    free(body);
    free(url);
    free(chat);
    free(text);
    return err;
}

/* Telegram/API boundary: streams a multipart upload to Telegram for binary
 * voice/audio payloads without copying the whole request body. */
static esp_err_t telegram_send_multipart_file(const char *method, const char *field_name,
                                              const char *filename, const char *content_type,
                                              const uint8_t *data, size_t data_size,
                                              const char *caption)
{
    ESP_RETURN_ON_FALSE(method && field_name && filename && content_type && data && data_size,
                        ESP_ERR_INVALID_ARG, TAG, "invalid upload");

    char *url = make_bot_url(method);
    ESP_RETURN_ON_FALSE(url, ESP_ERR_NO_MEM, TAG, "url alloc");

    const char *boundary = "----JacobSmartSpeakerBoundary";
    const char *chat_id = app_config_get()->telegram_chat_id;
    char chat_part[128];
    snprintf(chat_part, sizeof(chat_part),
             "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
             boundary, chat_id);

    char caption_part[256] = {0};
    if (caption && caption[0]) {
        snprintf(caption_part, sizeof(caption_part),
                 "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n",
                 boundary, caption);
    }

    char file_header[256];
    snprintf(file_header, sizeof(file_header),
             "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
             "Content-Type: %s\r\n\r\n",
             boundary, field_name, filename, content_type);

    char closing[64];
    snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", boundary);

    int content_len = strlen(chat_part) + strlen(caption_part) + strlen(file_header) +
                      data_size + strlen(closing);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    free(url);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "audio http init");

    char multipart_content_type[96];
    snprintf(multipart_content_type, sizeof(multipart_content_type),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", multipart_content_type);
    esp_err_t err = esp_http_client_open(client, content_len);
    if (err == ESP_OK) {
        esp_http_client_write(client, chat_part, strlen(chat_part));
        if (caption_part[0]) {
            esp_http_client_write(client, caption_part, strlen(caption_part));
        }
        esp_http_client_write(client, file_header, strlen(file_header));
        esp_http_client_write(client, (const char *)data, data_size);
        esp_http_client_write(client, closing, strlen(closing));
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            err = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

/* Telegram/API boundary: uploads an Ogg Opus recording as a Telegram voice
 * message to the configured chat. */
esp_err_t telegram_send_voice_ogg(const uint8_t *ogg, size_t ogg_size, const char *caption)
{
    return telegram_send_multipart_file("sendVoice", "voice", "recording.ogg", "audio/ogg",
                                        ogg, ogg_size, caption);
}

/* Telegram/API data-prep helper: scans parsed getUpdates JSON to find the
 * highest update_id so later polls ignore older messages. */
static int64_t max_update_id(cJSON *root)
{
    int64_t max_id = -1;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        cJSON *id = cJSON_GetObjectItem(item, "update_id");
        if (cJSON_IsNumber(id) && (int64_t)id->valuedouble > max_id) {
            max_id = (int64_t)id->valuedouble;
        }
    }
    return max_id;
}

/* Telegram/API boundary: calls getUpdates once and computes the next offset used
 * before waiting for a reply to the just-uploaded voice message. */
esp_err_t telegram_get_next_update_offset(int64_t *offset)
{
    char *url = make_bot_url("getUpdates?timeout=0");
    ESP_RETURN_ON_FALSE(url, ESP_ERR_NO_MEM, TAG, "url alloc");
    response_buffer_t response = {.max = 16384};
    esp_err_t err = http_get_text(url, &response);
    free(url);
    ESP_RETURN_ON_ERROR(err, TAG, "get updates");

    cJSON *root = cJSON_Parse(response.data ? response.data : "");
    free(response.data);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "updates json");
    int64_t max_id = max_update_id(root);
    cJSON_Delete(root);
    *offset = max_id >= 0 ? max_id + 1 : 0;
    return ESP_OK;
}

/* Telegram/API data-prep helper: inspects one update JSON object, verifies it
 * came from the configured chat, and extracts a reply file_id if present. */
static bool update_has_reply_file(cJSON *update, char *file_id, size_t file_id_size)
{
    cJSON *message = cJSON_GetObjectItem(update, "message");
    if (!cJSON_IsObject(message)) {
        return false;
    }
    cJSON *chat = cJSON_GetObjectItem(message, "chat");
    cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
    char chat_id_text[32];
    snprintf(chat_id_text, sizeof(chat_id_text), "%.0f", chat_id ? chat_id->valuedouble : -1);
    if (strcmp(chat_id_text, app_config_get()->telegram_chat_id) != 0) {
        return false;
    }

    const char *keys[] = {"voice", "audio", "document"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *file_obj = cJSON_GetObjectItem(message, keys[i]);
        cJSON *id = cJSON_GetObjectItem(file_obj, "file_id");
        if (cJSON_IsString(id) && id->valuestring) {
            strlcpy(file_id, id->valuestring, file_id_size);
            return true;
        }
    }
    return false;
}

/* Telegram/API boundary: polls getUpdates and reports the first voice/audio/file
 * attachment found after the tracked offset. */
esp_err_t telegram_poll_reply_file(int64_t *offset, char *file_id, size_t file_id_size)
{
    char method[96];
    snprintf(method, sizeof(method), "getUpdates?timeout=0&offset=%lld", (long long)*offset);
    char *url = make_bot_url(method);
    ESP_RETURN_ON_FALSE(url, ESP_ERR_NO_MEM, TAG, "url alloc");

    response_buffer_t response = {.max = 32768};
    esp_err_t err = http_get_text(url, &response);
    free(url);
    ESP_RETURN_ON_ERROR(err, TAG, "poll updates");

    cJSON *root = cJSON_Parse(response.data ? response.data : "");
    free(response.data);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "poll json");

    bool found = false;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        cJSON *id = cJSON_GetObjectItem(item, "update_id");
        if (cJSON_IsNumber(id) && (int64_t)id->valuedouble >= *offset) {
            *offset = (int64_t)id->valuedouble + 1;
        }
        if (update_has_reply_file(item, file_id, file_id_size)) {
            found = true;
            break;
        }
    }
    cJSON_Delete(root);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Telegram/API boundary: resolves a Telegram file_id to file_path, downloads the
 * reply bytes over TLS, and returns heap-owned data for playback/cache. */
esp_err_t telegram_download_file_by_id(const char *file_id, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;

    char method[256];
    snprintf(method, sizeof(method), "getFile?file_id=%s", file_id);
    char *url = make_bot_url(method);
    ESP_RETURN_ON_FALSE(url, ESP_ERR_NO_MEM, TAG, "url alloc");
    response_buffer_t response = {.max = 8192};
    esp_err_t err = http_get_text(url, &response);
    free(url);
    ESP_RETURN_ON_ERROR(err, TAG, "get file");

    cJSON *root = cJSON_Parse(response.data ? response.data : "");
    free(response.data);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "file json");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *path = cJSON_GetObjectItem(result, "file_path");
    ESP_RETURN_ON_FALSE(cJSON_IsString(path) && path->valuestring, ESP_FAIL, TAG, "file path");
    char *file_url = make_file_url(path->valuestring);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(file_url, ESP_ERR_NO_MEM, TAG, "file url");

    response_buffer_t file = {.max = TELEGRAM_REPLY_MAX_DOWNLOAD_BYTES};
    err = http_get_text(file_url, &file);
    free(file_url);
    ESP_RETURN_ON_ERROR(err, TAG, "download");
    *data = (uint8_t *)file.data;
    *size = file.len;
    return ESP_OK;
}
