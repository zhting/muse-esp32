/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Speaks Muse's replies with Volcengine's streaming TTS: 豆包语音合成, HTTP
 * chunked V3 (POST /api/v3/tts/unidirectional, X-Api-Key auth). A task of its
 * own makes the request and queues the MP3 as each piece arrives; the chat
 * session plays it from there. Speech starts with the first piece, about a
 * second after the request (TLS handshake plus the service's first packet),
 * where waiting for a whole synthesized clip took as long as the clip.
 *
 * Settings, under menuconfig > Muse > Speech: the API key (keep it out of
 * git: set it in your build directory's sdkconfig), the resource ID, voice,
 * speech rate and an optional speaking instruction for 2.0 voices.
 */

#include "muse_tts.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "muse_tts_stream.h"
#include "sdkconfig.h"

/* Builds that don't offer the settings (no Muse voice session) get TTS off. */
#ifndef CONFIG_MUSE_TTS_API_KEY
#define CONFIG_MUSE_TTS_API_KEY ""
#endif
#ifndef CONFIG_MUSE_TTS_RESOURCE_ID
#define CONFIG_MUSE_TTS_RESOURCE_ID "seed-tts-2.0"
#endif
#ifndef CONFIG_MUSE_TTS_SPEAKER
#define CONFIG_MUSE_TTS_SPEAKER "zh_female_wenrouxiaoya_uranus_bigtts"
#endif
#ifndef CONFIG_MUSE_TTS_SPEECH_RATE
#define CONFIG_MUSE_TTS_SPEECH_RATE 0
#endif
#ifndef CONFIG_MUSE_TTS_STYLE
#define CONFIG_MUSE_TTS_STYLE ""
#endif

#include <sys/time.h>
#include <time.h>

#if __has_include("muse_tts_key_local.h")
#include "muse_tts_key_local.h"
#endif

#ifndef LOCAL_TTS_API_KEY
#define LOCAL_TTS_API_KEY ""
#endif

static const char *get_api_key(void)
{
    if (CONFIG_MUSE_TTS_API_KEY[0] != '\0') {
        return CONFIG_MUSE_TTS_API_KEY;
    }
    return LOCAL_TTS_API_KEY;
}

static void ensure_valid_time(void)
{
    time_t now = time(NULL);
    if (now < 1735689600) {
        struct timeval tv = {
            .tv_sec = 1791078727,
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
    }
}

static const char *TAG = "muse_tts";

#define TTS_URL "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#define TTS_RATE 16000                       /* the speaker's rate, so nothing is resampled */
#define QUEUE_BYTES (64 * 1024)              /* MP3 that has arrived and isn't taken yet */
#define PIECE_CAP (16 * 1024)                /* one JSON piece of the body, to start with */
#define PIECE_MAX (512 * 1024)               /* ...and at most */
#define READ_BYTES 1024                      /* small reads hand each piece over as it lands */
#define CONNECT_TIMEOUT_MS 10000             /* DNS, TCP, TLS and sending the request */
#define LOGID_MAX 72
/* The host test (tests/test_muse_tts.py) shortens these. */
#ifndef HEADERS_POLL_MS
#define HEADERS_POLL_MS 3000                 /* waiting for the answer (shorter logs a warning each time) */
#endif
#ifndef POLL_MS
#define POLL_MS 200                          /* reading the stream: how soon a cancel is noticed */
#endif
#ifndef FIRST_AUDIO_TIMEOUT_US
#define FIRST_AUDIO_TIMEOUT_US (20 * 1000000LL)
#endif
#ifndef STALL_TIMEOUT_US
#define STALL_TIMEOUT_US (10 * 1000000LL)    /* no data mid-stream */
#endif

typedef struct {
    char *text;
    uint32_t gen;
} req_t;

typedef struct {
    uint32_t gen;
    int64_t first_audio_us;
} run_t;

static QueueHandle_t s_reqs;
static StreamBufferHandle_t s_mp3;
static atomic_uint s_gen;           /* the latest request; begin and cancel bump it */
static atomic_uint s_queued_gen;    /* whose MP3 s_mp3 holds */
static atomic_uint s_result_gen;    /* which request s_result is for */
static atomic_int s_result;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void *psram_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static bool cancelled(uint32_t gen)
{
    return atomic_load(&s_gen) != gen;
}

/* The stream's sink: queues MP3 for the session, waiting while the queue is full. */
static bool queue_mp3(const uint8_t *data, size_t len, void *ctx)
{
    run_t *run = (run_t *)ctx;
    if (!run->first_audio_us) {
        run->first_audio_us = now_us();
    }
    while (len) {
        if (cancelled(run->gen)) {
            return false;
        }
        size_t sent = xStreamBufferSend(s_mp3, data, len, pdMS_TO_TICKS(POLL_MS));
        data += sent;
        len -= sent;
    }
    return true;
}

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    /* The service's request ID, which its support asks for. */
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "X-Tt-Logid") == 0) {
        strlcpy((char *)e->user_data, e->header_value, LOGID_MAX);
    }
    return ESP_OK;
}

static char *request_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *user = cJSON_AddObjectToObject(root, "user");
    cJSON_AddStringToObject(user, "uid", "muse-gadget");
    cJSON *req = cJSON_AddObjectToObject(root, "req_params");
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "speaker", CONFIG_MUSE_TTS_SPEAKER);
    cJSON *audio = cJSON_AddObjectToObject(req, "audio_params");
    cJSON_AddStringToObject(audio, "format", "mp3");
    cJSON_AddNumberToObject(audio, "sample_rate", TTS_RATE);
    cJSON_AddNumberToObject(audio, "speech_rate", CONFIG_MUSE_TTS_SPEECH_RATE);
    /* additions is a JSON object sent as a string. With disable_markdown_filter
     * true, the service reads **this** as "this" rather than the asterisks. */
    cJSON *add = cJSON_CreateObject();
    cJSON_AddBoolToObject(add, "disable_markdown_filter", true);
    if (CONFIG_MUSE_TTS_STYLE[0]) {
        cJSON *style = cJSON_AddArrayToObject(add, "context_texts");
        cJSON_AddItemToArray(style, cJSON_CreateString(CONFIG_MUSE_TTS_STYLE));
    }
    char *additions = cJSON_PrintUnformatted(add);
    cJSON_Delete(add);
    if (additions) {
        cJSON_AddStringToObject(req, "additions", additions);
        cJSON_free(additions);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static void request_id(char *out, size_t cap)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2],
             r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

/* Reads an error response's body for the log. */
static void log_error_body(esp_http_client_handle_t c, int status, const char *logid)
{
    char body[256];
    int n = 0;
    for (int tries = 0; tries < 10 && n < (int)sizeof(body) - 1; tries++) {
        int r = esp_http_client_read(c, body + n, sizeof(body) - 1 - n);
        if (r > 0) {
            n += r;
        } else if (r != -ESP_ERR_HTTP_EAGAIN) {
            break;
        }
    }
    body[n] = '\0';
    ESP_LOGW(TAG, "HTTP %d from the TTS service: %s (logid %s)", status, body, logid[0] ? logid : "-");
}

#define VOLC_CREATE_URL "https://openspeech.bytedance.com/api/v3/tts/create"

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} fallback_http_resp_t;

static esp_err_t fallback_http_event_handler(esp_http_client_event_t *evt)
{
    fallback_http_resp_t *r = (fallback_http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (r && r->buf && r->len + evt->data_len < r->cap) {
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

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

static bool speak_seed_audio_fallback(const req_t *q, run_t *run, int64_t t0)
{
    if (cancelled(q->gen)) {
        return false;
    }
    ensure_valid_time();

    char clean_text[768];
    clean_for_speech(q->text, clean_text, sizeof(clean_text));
    if (!clean_text[0]) {
        return false;
    }

    char prompt[1024];
    snprintf(prompt, sizeof(prompt), "女子（年轻女性，温柔甜美，嗓音轻柔清澈）用亲切温柔的语气说道：“%s”", clean_text);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    cJSON_AddStringToObject(root, "model", "seed-audio-1.0");
    cJSON_AddStringToObject(root, "text_prompt", prompt);

    cJSON *audio_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_cfg, "format", "mp3");
    cJSON_AddNumberToObject(audio_cfg, "sample_rate", TTS_RATE);
    cJSON_AddNumberToObject(audio_cfg, "pitch_rate", 0);
    cJSON_AddNumberToObject(audio_cfg, "speech_rate", CONFIG_MUSE_TTS_SPEECH_RATE);
    cJSON_AddNumberToObject(audio_cfg, "loudness_rate", 0);
    cJSON_AddItemToObject(root, "audio_config", audio_cfg);
    cJSON_AddItemToObject(root, "watermark", cJSON_CreateObject());

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) {
        return false;
    }

    const size_t resp_cap = 128 * 1024;
    char *resp_buf = (char *)heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) {
        resp_buf = (char *)malloc(resp_cap);
    }
    if (!resp_buf) {
        ESP_LOGE(TAG, "failed to allocate fallback buffer");
        cJSON_free(post_data);
        return false;
    }
    resp_buf[0] = '\0';

    fallback_http_resp_t resp = {
        .buf = resp_buf,
        .len = 0,
        .cap = resp_cap,
    };

    esp_http_client_config_t cfg = {
        .url = VOLC_CREATE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 25000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = fallback_http_event_handler,
        .user_data = &resp,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(post_data);
        heap_caps_free(resp_buf);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Api-Key", get_api_key());
    esp_http_client_set_post_field(client, post_data, (int)strlen(post_data));

    ESP_LOGI(TAG, "Requesting seed-audio-1.0 fallback: len=%u", (unsigned)strlen(clean_text));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(post_data);

    bool ok = false;
    if (err == ESP_OK && status == 200 && !cancelled(q->gen)) {
        const char *key = "\"audio\":";
        char *p = strstr(resp.buf, key);
        if (!p) {
            key = "\"audio\" :";
            p = strstr(resp.buf, key);
        }
        if (p) {
            p += strlen(key);
            while (*p == ' ' || *p == '\"') p++;
            char *end = strchr(p, '\"');
            if (end && end > p) {
                size_t b64_len = end - p;
                size_t max_mp3 = (b64_len * 3) / 4 + 16;
                uint8_t *decoded_mp3 = (uint8_t *)heap_caps_malloc(max_mp3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (decoded_mp3) {
                    int decoded_len = muse_tts_base64_decode(p, b64_len, decoded_mp3, max_mp3);
                    if (decoded_len > 0 && !cancelled(q->gen)) {
                        run->first_audio_us = now_us();
                        ok = queue_mp3(decoded_mp3, (size_t)decoded_len, run);
                        ESP_LOGI(TAG, "seed-audio-1.0 delivered %d bytes MP3 in +%d ms",
                                 decoded_len, (int)((now_us() - t0) / 1000));
                    } else {
                        ESP_LOGE(TAG, "base64 decode failed for seed-audio-1.0");
                    }
                    heap_caps_free(decoded_mp3);
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "seed-audio-1.0 request failed: err=%s status=%d", esp_err_to_name(err), status);
    }

    heap_caps_free(resp_buf);
    return ok;
}

/* Speaks one request, queueing its MP3. True if it all arrived. */
static bool speak(const req_t *q)
{
    int64_t t0 = now_us(), t_conn = 0, t_head = 0;
    run_t run = { .gen = q->gen };
    char logid[LOGID_MAX] = "";
    muse_tts_stream_t st;
    muse_tts_stream_status_t ss = MUSE_TTS_STREAM_ERROR;
    esp_http_client_handle_t c = NULL;
    size_t text_len = strlen(q->text);

    char *body = request_body(q->text);
    bool stream_ok = muse_tts_stream_init(&st, PIECE_CAP, PIECE_MAX, psram_realloc, queue_mp3, &run);
    if (!body || !stream_ok) {
        ESP_LOGE(TAG, "out of memory");
        goto out;
    }
    esp_http_client_config_t cfg = {
        .url = TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_data = logid,
        .disable_auto_redirect = true,
    };
    c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "HTTP client init failed");
        goto out;
    }
    char rid[40];
    request_id(rid, sizeof(rid));
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "X-Api-Key", get_api_key());
    esp_http_client_set_header(c, "X-Api-Resource-Id", CONFIG_MUSE_TTS_RESOURCE_ID);
    esp_http_client_set_header(c, "X-Api-Request-Id", rid);

    int body_len = (int)strlen(body);
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't reach the TTS service: %s", esp_err_to_name(err));
        goto out;
    }
    if (esp_http_client_write(c, body, body_len) != body_len) {
        ESP_LOGW(TAG, "sending the request failed");
        goto out;
    }
    t_conn = now_us();

    /* From here, shorter reads, so a cancel is noticed while waiting. */
    esp_http_client_set_timeout_ms(c, HEADERS_POLL_MS);
    for (;;) {
        int64_t r = esp_http_client_fetch_headers(c);
        if (r >= 0) {
            break;
        }
        if (r != -ESP_ERR_HTTP_EAGAIN || cancelled(q->gen) || now_us() - t_conn > FIRST_AUDIO_TIMEOUT_US) {
            if (!cancelled(q->gen)) {
                ESP_LOGW(TAG, "no answer from the TTS service");
            }
            goto out;
        }
    }
    t_head = now_us();
    esp_http_client_set_timeout_ms(c, POLL_MS);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        log_error_body(c, status, logid);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        c = NULL;
        if (status == 403 || status == 400 || status == 404) {
            ESP_LOGW(TAG, "Streaming 2.0 returned %d (likely not granted). Falling back to seed-audio-1.0...", status);
            bool ok = speak_seed_audio_fallback(q, &run, t0);
            if (stream_ok) {
                muse_tts_stream_free(&st);
            }
            cJSON_free(body);
            return ok;
        }
        goto out;
    }

    char buf[READ_BYTES];
    int64_t last_rx = now_us();
    ss = MUSE_TTS_STREAM_MORE;
    while (ss == MUSE_TTS_STREAM_MORE && !cancelled(q->gen)) {
        int n = esp_http_client_read(c, buf, sizeof(buf));
        if (n > 0) {
            last_rx = now_us();
            ss = muse_tts_stream_feed(&st, buf, (size_t)n);
        } else if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (now_us() - last_rx > (run.first_audio_us ? STALL_TIMEOUT_US : FIRST_AUDIO_TIMEOUT_US)) {
                ESP_LOGW(TAG, "the TTS stream stalled");
                ss = MUSE_TTS_STREAM_ERROR;
            }
        } else if (n == 0 && esp_http_client_is_complete_data_received(c)) {
            ss = muse_tts_stream_finish(&st);
        } else {
            ESP_LOGW(TAG, "the TTS stream broke off (%d)", n);
            ss = MUSE_TTS_STREAM_ERROR;
        }
    }
    if (ss == MUSE_TTS_STREAM_ERROR && !cancelled(q->gen)) {
        ESP_LOGW(TAG, "TTS failed: %s (code %d, logid %s)", st.message[0] ? st.message : "see above", st.code,
                 logid[0] ? logid : "-");
    }

out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    bool ok = ss == MUSE_TTS_STREAM_END && !cancelled(q->gen);
    if (cancelled(q->gen)) {
        ESP_LOGI(TAG, "cancelled");
    } else {
        /* Where the time went: connected = DNS + TCP + TLS + request sent;
         * answered = response headers; first audio = the first piece decoded. */
        ESP_LOGI(TAG, "%u bytes of text -> %u bytes of MP3: connected +%d ms, answered +%d ms, first audio +%d ms, "
                      "done +%d ms",
                 (unsigned)text_len, (unsigned)(stream_ok ? st.audio_bytes : 0),
                 t_conn ? (int)((t_conn - t0) / 1000) : -1, t_head ? (int)((t_head - t0) / 1000) : -1,
                 run.first_audio_us ? (int)((run.first_audio_us - t0) / 1000) : -1, (int)((now_us() - t0) / 1000));
    }
    if (stream_ok) {
        muse_tts_stream_free(&st);
    }
    cJSON_free(body);
    return ok;
}

static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        req_t q;
        if (xQueueReceive(s_reqs, &q, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!cancelled(q.gen)) {
            /* This task is the only writer, and the session never waits on it,
             * so it can be reset. Until s_queued_gen says so, nothing takes. */
            xStreamBufferReset(s_mp3);
            atomic_store(&s_queued_gen, q.gen);
            bool ok = speak(&q);
            atomic_store(&s_result, ok ? MUSE_TTS_DONE : MUSE_TTS_FAILED);
            atomic_store(&s_result_gen, q.gen);
        }
        heap_caps_free(q.text);
    }
}

static bool start(void)
{
    static bool started;
    if (started) {
        return true;
    }
    /* Both exist before the task does: it waits on s_reqs as soon as it runs. */
    if (!s_reqs) {
        s_reqs = xQueueCreate(1, sizeof(req_t));
    }
    if (!s_mp3) {
        s_mp3 = xStreamBufferCreateWithCaps(QUEUE_BYTES, 1, MALLOC_CAP_SPIRAM);
    }
    /* Stack in PSRAM, like the chat session's, which runs TLS the same way. */
    if (!s_reqs || !s_mp3 ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", 16 * 1024, NULL, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        return false;   /* tried again with the next message */
    }
    started = true;
    ESP_LOGI(TAG, "speaking replies with %s (%s)", CONFIG_MUSE_TTS_SPEAKER, CONFIG_MUSE_TTS_RESOURCE_ID);
    return true;
}

static void drop_waiting(void)
{
    req_t stale;
    while (xQueueReceive(s_reqs, &stale, 0) == pdTRUE) {
        heap_caps_free(stale.text);
    }
}

bool muse_tts_available(void)
{
    return get_api_key()[0] != '\0';
}

bool muse_tts_begin(const char *text)
{
    if (!muse_tts_available()) {
        static bool said;
        if (!said) {
            said = true;
            ESP_LOGI(TAG, "no TTS API key (CONFIG_MUSE_TTS_API_KEY): replies are shown, not spoken");
        }
        return false;
    }
    if (!text || !text[0] || !start()) {
        return false;
    }
    size_t n = strlen(text) + 1;
    char *copy = heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    if (!copy) {
        return false;
    }
    memcpy(copy, text, n);
    req_t q = { copy, atomic_fetch_add(&s_gen, 1) + 1 };
    drop_waiting();   /* one the task hasn't started is out of date */
    if (xQueueSend(s_reqs, &q, 0) != pdTRUE) {
        heap_caps_free(copy);
        return false;
    }
    return true;
}

size_t muse_tts_take(uint8_t *out, size_t cap)
{
    if (!s_mp3 || !cap || atomic_load(&s_queued_gen) != atomic_load(&s_gen)) {
        return 0;
    }
    return xStreamBufferReceive(s_mp3, out, cap, 0);
}

size_t muse_tts_pending(void)
{
    if (!s_mp3 || atomic_load(&s_queued_gen) != atomic_load(&s_gen)) {
        return 0;
    }
    return xStreamBufferBytesAvailable(s_mp3);
}

muse_tts_state_t muse_tts_state(void)
{
    if (atomic_load(&s_result_gen) != atomic_load(&s_gen)) {
        return MUSE_TTS_RUNNING;
    }
    return (muse_tts_state_t)atomic_load(&s_result);
}

void muse_tts_cancel(void)
{
    if (!s_reqs) {
        return;
    }
    atomic_fetch_add(&s_gen, 1);
    drop_waiting();
}