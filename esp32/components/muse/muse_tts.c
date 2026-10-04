/*
 * Volcengine Streaming TTS client for Muse Gadget
 * Model: DoubaoVoice 2.0 (seed-tts-2.0, unidirectional streaming)
 * Speaker: zh_female_wenrouxiaoya_uranus_bigtts (温柔小雅 2.0)
 */

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "muse_tts";

#define VOLC_STREAM_URL "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#define VOLC_RESOURCE_ID "seed-tts-2.0"
#define VOLC_SPEAKER "zh_female_wenrouxiaoya_uranus_bigtts"
#define VOLC_FALLBACK_URL "https://openspeech.bytedance.com/api/v3/tts/create"

#if __has_include("volc_key_local.h")
#include "volc_key_local.h"
#define VOLC_API_KEY VOLC_LOCAL_API_KEY
#elif defined(CONFIG_VOLC_TTS_API_KEY) && (sizeof(CONFIG_VOLC_TTS_API_KEY) > 1)
#define VOLC_API_KEY CONFIG_VOLC_TTS_API_KEY
#else
#define VOLC_API_KEY "YOUR_VOLCENGINE_API_KEY"
#endif

#define STREAM_BUF_SIZE (64 * 1024)
#define READ_CHUNK_SIZE 1024
#define LINE_BUF_SIZE 4096
#define DECODE_BUF_SIZE 2048

static StreamBufferHandle_t s_mp3_stream = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_task_handle = NULL;

static atomic_bool s_tts_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_tts_cancelled = ATOMIC_VAR_INIT(false);
static atomic_bool s_server_done = ATOMIC_VAR_INIT(true);

typedef struct {
    char *text;
} tts_cmd_t;

/* 解析单行 JSON 并提取 MP3 数据 */
static void process_json_line(const char *line)
{
    /* 检查结束码 code: 20000000 */
    if (strstr(line, "\"code\":20000000") || strstr(line, "\"code\": 20000000")) {
        ESP_LOGI(TAG, "Volcengine TTS streaming completed (code: 20000000)");
        atomic_store(&s_server_done, true);
        return;
    }

    /* 定位 \"data\": \"...\" */
    const char *data_key = "\"data\":\"";
    char *p = strstr(line, data_key);
    if (!p) {
        data_key = "\"data\": \"";
        p = strstr(line, data_key);
    }

    if (p) {
        p += strlen(data_key);
        char *end = strchr(p, '\"');
        if (end && end > p) {
            size_t b64_len = end - p;
            static uint8_t dec_buf[DECODE_BUF_SIZE];
            size_t olen = 0;
            int ret = mbedtls_base64_decode(dec_buf, sizeof(dec_buf), &olen, (const unsigned char *)p, b64_len);
            if (ret == 0 && olen > 0 && s_mp3_stream) {
                xStreamBufferSend(s_mp3_stream, dec_buf, olen, pdMS_TO_TICKS(50));
            }
        }
    } else {
        /* 如果不是数据行，打印可能的错误日志 */
        if (!strstr(line, "\"code\":0") && !strstr(line, "\"code\": 0")) {
            ESP_LOGW(TAG, "TTS response line: %.120s", line);
        }
    }
}

static void fetch_fallback_seed_audio(const char *text)
{
    ESP_LOGI(TAG, "Falling back to seed-audio-1.0 for speech generation: %.64s", text);
    char prompt[1024];
    snprintf(prompt, sizeof(prompt), "女子（年轻女性，温柔甜美，嗓音轻柔清澈）用亲切温柔的语气说道：“%s”", text);

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "model", "seed-audio-1.0");
    cJSON_AddStringToObject(root, "text_prompt", prompt);
    cJSON *audio_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_cfg, "format", "mp3");
    cJSON_AddNumberToObject(audio_cfg, "sample_rate", 16000);
    cJSON_AddItemToObject(root, "audio_config", audio_cfg);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return;

    size_t resp_cap = 128 * 1024;
    char *resp_buf = (char *)heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) resp_buf = (char *)malloc(resp_cap);
    if (!resp_buf) {
        free(post_data);
        return;
    }
    resp_buf[0] = '\0';
    size_t resp_len = 0;

    esp_http_client_config_t config = {
        .url = VOLC_FALLBACK_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 25000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        heap_caps_free(resp_buf);
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Api-Key", VOLC_API_KEY);

    int post_len = strlen(post_data);
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err == ESP_OK) {
        esp_http_client_write(client, post_data, post_len);
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            while (!atomic_load(&s_tts_cancelled)) {
                int r = esp_http_client_read(client, resp_buf + resp_len, (int)(resp_cap - 1 - resp_len));
                if (r <= 0) break;
                resp_len += r;
                resp_buf[resp_len] = '\0';
                if (resp_len >= resp_cap - 1) break;
            }
        } else {
            ESP_LOGW(TAG, "Fallback seed-audio-1.0 HTTP status %d", status);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (resp_len > 0 && !atomic_load(&s_tts_cancelled)) {
        const char *key = "\"audio\":";
        char *p = strstr(resp_buf, key);
        if (!p) {
            key = "\"audio\" :";
            p = strstr(resp_buf, key);
        }
        if (p) {
            p += strlen(key);
            while (*p == ' ' || *p == '\"') p++;
            char *end = strchr(p, '\"');
            if (end && end > p) {
                size_t b64_len = end - p;
                size_t out_max = (b64_len / 4) * 3 + 4;
                uint8_t *decoded = (uint8_t *)heap_caps_malloc(out_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!decoded) decoded = (uint8_t *)malloc(out_max);
                if (decoded) {
                    size_t olen = 0;
                    int b64_ret = mbedtls_base64_decode(decoded, out_max, &olen, (const unsigned char *)p, b64_len);
                    if (b64_ret == 0 && olen > 0 && s_mp3_stream) {
                        ESP_LOGI(TAG, "Fallback successfully decoded %u bytes MP3", (unsigned)olen);
                        size_t sent = 0;
                        while (sent < olen && !atomic_load(&s_tts_cancelled)) {
                            size_t to_send = olen - sent;
                            if (to_send > 2048) to_send = 2048;
                            size_t actual = xStreamBufferSend(s_mp3_stream, decoded + sent, to_send, pdMS_TO_TICKS(500));
                            if (actual == 0) break;
                            sent += actual;
                        }
                    } else {
                        ESP_LOGE(TAG, "Fallback base64 decode failed: ret=%d", b64_ret);
                    }
                    heap_caps_free(decoded);
                }
            }
        } else {
            ESP_LOGE(TAG, "Fallback JSON missing \"audio\" field: %.100s", resp_buf);
        }
    }

    heap_caps_free(resp_buf);
}

static void tts_worker_task(void *arg)
{
    (void)arg;
    char *line_buf = (char *)heap_caps_malloc(LINE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line_buf) {
        line_buf = (char *)malloc(LINE_BUF_SIZE);
    }
    char *read_buf = (char *)malloc(READ_CHUNK_SIZE);

    tts_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!cmd.text) {
            continue;
        }

        atomic_store(&s_tts_running, true);
        atomic_store(&s_tts_cancelled, false);
        atomic_store(&s_server_done, false);
        if (s_mp3_stream) {
            xStreamBufferReset(s_mp3_stream);
        }

        /* 1. 构造 2.0 流式 JSON 请求体 */
        cJSON *root = cJSON_CreateObject();
        cJSON *user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "uid", "muse-gadget");
        cJSON_AddItemToObject(root, "user", user);

        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "text", cmd.text);
        cJSON_AddStringToObject(req, "speaker", VOLC_SPEAKER);

        cJSON *audio = cJSON_CreateObject();
        cJSON_AddStringToObject(audio, "format", "mp3");
        cJSON_AddNumberToObject(audio, "sample_rate", 16000);
        cJSON_AddNumberToObject(audio, "speech_rate", 0);
        cJSON_AddItemToObject(req, "audio_params", audio);

        cJSON_AddStringToObject(req, "additions", "{\"disable_markdown_filter\":true}");
        cJSON_AddItemToObject(root, "req_params", req);

        char *post_data = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (!post_data) {
            ESP_LOGE(TAG, "Failed to create JSON payload");
            free(cmd.text);
            atomic_store(&s_server_done, true);
            atomic_store(&s_tts_running, false);
            continue;
        }

        /* 2. 发起 HTTP POST 请求 */
        esp_http_client_config_t config = {
            .url = VOLC_STREAM_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 10000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            free(post_data);
            free(cmd.text);
            atomic_store(&s_server_done, true);
            atomic_store(&s_tts_running, false);
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "X-Api-Key", VOLC_API_KEY);
        esp_http_client_set_header(client, "X-Api-Resource-Id", VOLC_RESOURCE_ID);

        int post_len = strlen(post_data);
        esp_err_t err = esp_http_client_open(client, post_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP client open failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        int written = esp_http_client_write(client, post_data, post_len);
        if (written < 0) {
            ESP_LOGW(TAG, "HTTP client write failed");
            goto cleanup;
        }

        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        (void)content_len;
        if (status != 200) {
            ESP_LOGW(TAG, "Streaming TTS 2.0 returned HTTP %d, falling back to seed-audio-1.0...", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;

            if (!atomic_load(&s_tts_cancelled)) {
                fetch_fallback_seed_audio(cmd.text);
            }
            goto cleanup;
        }

        /* 3. 流式读取并按行解析 */
        size_t line_pos = 0;
        while (!atomic_load(&s_tts_cancelled)) {
            int r = esp_http_client_read(client, read_buf, READ_CHUNK_SIZE);
            if (r < 0) {
                ESP_LOGW(TAG, "HTTP read error: %d", r);
                break;
            }
            if (r == 0) {
                /* 对端关闭或传输结束 */
                break;
            }

            for (int i = 0; i < r; i++) {
                char c = read_buf[i];
                if (c == '\n') {
                    if (line_buf && line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        process_json_line(line_buf);
                        line_pos = 0;
                    }
                    if (atomic_load(&s_server_done)) {
                        break;
                    }
                } else if (c != '\r') {
                    if (line_buf && line_pos + 1 < LINE_BUF_SIZE) {
                        line_buf[line_pos++] = c;
                    }
                }
            }

            if (atomic_load(&s_server_done)) {
                break;
            }
        }

        /* 处理尾部可能未带换行符的一行 */
        if (line_buf && line_pos > 0 && !atomic_load(&s_tts_cancelled)) {
            line_buf[line_pos] = '\0';
            process_json_line(line_buf);
        }

cleanup:
        if (client) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        free(post_data);
        free(cmd.text);
        atomic_store(&s_server_done, true);
        atomic_store(&s_tts_running, false);
    }

    if (line_buf) heap_caps_free(line_buf);
    if (read_buf) free(read_buf);
    vTaskDelete(NULL);
}

static bool ensure_tts_inited(void)
{
    if (s_cmd_queue && s_mp3_stream) {
        return true;
    }

    if (!s_mp3_stream) {
        s_mp3_stream = xStreamBufferCreateWithCaps(STREAM_BUF_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mp3_stream) {
            s_mp3_stream = xStreamBufferCreate(STREAM_BUF_SIZE, 1);
        }
    }

    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreate(2, sizeof(tts_cmd_t));
    }

    if (!s_task_handle && s_cmd_queue && s_mp3_stream) {
        BaseType_t ret = xTaskCreateWithCaps(
            tts_worker_task,
            "muse_tts",
            8192,
            NULL,
            5,
            &s_task_handle,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (ret != pdPASS) {
            ret = xTaskCreate(tts_worker_task, "muse_tts", 8192, NULL, 5, &s_task_handle);
        }
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create muse_tts worker task");
            return false;
        }
    }

    return (s_cmd_queue && s_mp3_stream && s_task_handle);
}

bool muse_tts_start(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }

    if (strcmp(VOLC_API_KEY, "YOUR_VOLCENGINE_API_KEY") == 0 || strlen(VOLC_API_KEY) == 0) {
        ESP_LOGW(TAG, "Volcengine API key not configured! Please configure CONFIG_VOLC_TTS_API_KEY or set VOLC_API_KEY");
        return false;
    }

    if (!ensure_tts_inited()) {
        return false;
    }

    /* 若之前有未完成的请求，先打断 */
    muse_tts_cancel();

    tts_cmd_t cmd = {
        .text = strdup(text)
    };
    if (!cmd.text) {
        return false;
    }

    /* 必须在入队前同步重置标志，防止主线程竞态判定提前结束 */
    atomic_store(&s_tts_cancelled, false);
    atomic_store(&s_server_done, false);
    atomic_store(&s_tts_running, true);

    ESP_LOGI(TAG, "Enqueuing streaming TTS 2.0 (unidirectional) for %u chars", (unsigned)strlen(text));
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(cmd.text);
        atomic_store(&s_server_done, true);
        atomic_store(&s_tts_running, false);
        return false;
    }

    return true;
}

size_t muse_tts_read_chunk(uint8_t *out_mp3, size_t max_len)
{
    if (!s_mp3_stream || !out_mp3 || max_len == 0) {
        return 0;
    }
    return xStreamBufferReceive(s_mp3_stream, out_mp3, max_len, 0);
}

bool muse_tts_is_finished(void)
{
    if (!s_mp3_stream) {
        return true;
    }
    return atomic_load(&s_server_done) && (xStreamBufferBytesAvailable(s_mp3_stream) == 0);
}

void muse_tts_cancel(void)
{
    atomic_store(&s_tts_cancelled, true);
    if (s_mp3_stream) {
        xStreamBufferReset(s_mp3_stream);
    }
    atomic_store(&s_server_done, true);
}
