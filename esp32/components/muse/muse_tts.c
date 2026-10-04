/*
 * Volcengine TTS client for Muse Gadget
 * Model: seed-audio-1.0 (Gentle Female Voice)
 */

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include <sys/time.h>
#include <time.h>

static const char *TAG = "muse_tts";

#define VOLC_TTS_URL "https://openspeech.bytedance.com/api/v3/tts/create"
#define RESP_MAX_BYTES (128 * 1024)

#ifndef CONFIG_VOLC_TTS_API_KEY
#define VOLC_TTS_DEFAULT_KEY ""
#else
#define VOLC_TTS_DEFAULT_KEY CONFIG_VOLC_TTS_API_KEY
#endif

/* 可在编译时通过 menuconfig 指定 CONFIG_VOLC_TTS_API_KEY，或在此填入你的火山 API Key */
#define VOLC_API_KEY (sizeof(VOLC_TTS_DEFAULT_KEY) > 1 ? VOLC_TTS_DEFAULT_KEY : "YOUR_VOLCENGINE_API_KEY")



static void ensure_valid_time(void)
{
    time_t now = time(NULL);
    if (now < 1735689600) { /* 2025-01-01 前说明系统还在 1970 初始时间 */
        struct timeval tv = {
            .tv_sec = 1791078727, /* 2026-10-04 */
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Synchronized system time to 2026 for TLS validation (was %ld)", (long)now);
    }
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (r && r->buf && r->len + evt->data_len < r->cap) {
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* 过滤常见 Markdown 格式标记，使朗读更自然 */
static void clean_for_speech(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        char c = *p;
        if (c == '*' || c == '#' || c == '`' || c == '~' || c == '>') {
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

int muse_tts_fetch(const char *text, uint8_t *out_mp3, size_t max_mp3_len)
{
    if (!text || !text[0] || !out_mp3 || max_mp3_len == 0) {
        return 0;
    }

    if (strcmp(VOLC_API_KEY, "YOUR_VOLCENGINE_API_KEY") == 0 || strlen(VOLC_API_KEY) == 0) {
        ESP_LOGW(TAG, "Volcengine API key not configured! Please configure CONFIG_VOLC_TTS_API_KEY or set VOLC_API_KEY in muse_tts.c");
        return 0;
    }

    ensure_valid_time();

    /* 1. 清洗文本并构造温柔女声音色 Prompt */
    char clean_text[768];
    clean_for_speech(text, clean_text, sizeof(clean_text));
    if (!clean_text[0]) {
        return 0;
    }

    char prompt[1024];
    snprintf(prompt, sizeof(prompt), "女子（年轻女性，温柔甜美，嗓音轻柔清澈）用亲切温柔的语气说道：“%s”", clean_text);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "model", "seed-audio-1.0");
    cJSON_AddStringToObject(root, "text_prompt", prompt);

    cJSON *audio_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_cfg, "format", "mp3");
    cJSON_AddNumberToObject(audio_cfg, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_cfg, "pitch_rate", 0);
    cJSON_AddNumberToObject(audio_cfg, "speech_rate", 0);
    cJSON_AddNumberToObject(audio_cfg, "loudness_rate", 0);
    cJSON_AddItemToObject(root, "audio_config", audio_cfg);
    cJSON_AddItemToObject(root, "watermark", cJSON_CreateObject());

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) {
        return 0;
    }

    /* 2. 在 PSRAM 中分配响应接收缓冲区 */
    char *resp_buf = (char *)heap_caps_malloc(RESP_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) {
        resp_buf = (char *)malloc(RESP_MAX_BYTES);
    }
    if (!resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for response", RESP_MAX_BYTES);
        free(post_data);
        return 0;
    }
    resp_buf[0] = '\0';

    http_resp_t resp = {
        .buf = resp_buf,
        .len = 0,
        .cap = RESP_MAX_BYTES,
    };

    /* 3. 发起 HTTPS POST 请求，优先使用系统内置 X509 CA 根证书 bundle */
    esp_http_client_config_t config = {
        .url = VOLC_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 25000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        free(post_data);
        heap_caps_free(resp_buf);
        return 0;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Api-Key", VOLC_API_KEY);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    ESP_LOGI(TAG, "Requesting Volcengine TTS (gentle female): len=%d", (int)strlen(clean_text));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP perform failed: err=%s, status=%d, resp_len=%u", esp_err_to_name(err), status, (unsigned)resp.len);
        heap_caps_free(resp_buf);
        return 0;
    }

    ESP_LOGI(TAG, "HTTP 200 OK, received %u bytes response", (unsigned)resp.len);

    /* 4. 从 JSON 中定位并提取 \"audio\": \"<base64>\" */
    int ret_len = 0;
    const char *key = "\"audio\":";
    char *p = strstr(resp.buf, key);
    if (!p) {
        key = "\"audio\" :";
        p = strstr(resp.buf, key);
    }

    if (p) {
        p += strlen(key);
        while (*p == ' ' || *p == '\"') {
            p++;
        }
        char *end = strchr(p, '\"');
        if (end && end > p) {
            size_t b64_len = end - p;
            size_t olen = 0;
            int b64_ret = mbedtls_base64_decode(out_mp3, max_mp3_len, &olen, (const unsigned char *)p, b64_len);
            if (b64_ret == 0 && olen > 0) {
                ESP_LOGI(TAG, "Successfully decoded %u bytes MP3 audio", (unsigned)olen);
                ret_len = (int)olen;
            } else {
                ESP_LOGE(TAG, "Base64 decode failed: ret=%d, olen=%u", b64_ret, (unsigned)olen);
            }
        } else {
            ESP_LOGE(TAG, "Could not find end quote for audio field");
        }
    } else {
        ESP_LOGE(TAG, "Could not find \"audio\" key in response: %.128s", resp.buf);
    }

    heap_caps_free(resp_buf);
    return ret_len;
}
