/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * components/muse/muse_tts.c on pthreads against a scripted server
 * (tests/tts_fakes), driven the way muse_chat_session.cpp's pump_tts() drives
 * it. For tests/test_muse_tts.py. Exits non-zero if a check fails.
 *
 *   muse_tts_harness          the scenarios
 *   muse_tts_harness no-key   (built without an API key) TTS stays off
 */

#define _DEFAULT_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "fake_http.h"
#include "muse_tts.h"

static int s_failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            s_failures++;                                                  \
            fprintf(stdout, "FAIL %s:%d: %s: ", __func__, __LINE__, #cond); \
            fprintf(stdout, __VA_ARGS__);                                  \
            fprintf(stdout, "\n");                                         \
        }                                                                  \
    } while (0)

static void b64(const unsigned char *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

typedef struct {
    char *body;
    unsigned char *audio;
    size_t audio_len;
} script_t;

/* pieces of random audio (seeded), as the service streams them; end: add the end piece. */
static script_t make_body(unsigned seed, int pieces, bool end, const char *error_after_first)
{
    script_t s = { malloc(1 << 20), malloc(1 << 18), 0 };
    size_t o = 0;
    srand(seed);
    for (int p = 0; p < pieces; p++) {
        size_t n = 200 + (size_t)(rand() % 2500);
        unsigned char *a = s.audio + s.audio_len;
        for (size_t i = 0; i < n; i++) {
            a[i] = (unsigned char)rand();
        }
        static char enc[8192];
        b64(a, n, enc);
        o += (size_t)sprintf(s.body + o, "{\"code\":0,\"message\":\"\",\"data\":\"%s\"}\n", enc);
        s.audio_len += n;
        if (p == 0 && error_after_first) {
            o += (size_t)sprintf(s.body + o, "%s\n", error_after_first);
        }
    }
    if (end) {
        o += (size_t)sprintf(s.body + o, "{\"code\":20000000,\"message\":\"ok\",\"usage\":{\"text_words\":12}}\n");
    }
    s.body[o] = '\0';
    return s;
}

static void free_script(script_t *s)
{
    free(s->body);
    free(s->audio);
}

typedef struct {
    unsigned char buf[1 << 18];
    size_t len;
    muse_tts_state_t state;
    int64_t first_us, end_us;
} heard_t;

/* As pump_tts() does: state first, then take everything, until it's over and drained. */
static void listen(heard_t *h, int64_t t0, int timeout_ms, size_t stop_after)
{
    h->len = 0;
    h->first_us = h->end_us = 0;
    int64_t give_up = t0 + timeout_ms * 1000LL;
    for (;;) {
        muse_tts_state_t st = muse_tts_state();
        size_t n;
        while ((n = muse_tts_take(h->buf + h->len, sizeof(h->buf) - h->len)) > 0) {
            if (!h->first_us) {
                h->first_us = esp_timer_get_time() - t0;
            }
            h->len += n;
        }
        if ((st != MUSE_TTS_RUNNING && !muse_tts_pending()) || (stop_after && h->len >= stop_after)) {
            h->state = st;
            h->end_us = esp_timer_get_time() - t0;
            return;
        }
        if (esp_timer_get_time() > give_up) {
            h->state = MUSE_TTS_RUNNING;
            h->end_us = esp_timer_get_time() - t0;
            return;
        }
        usleep(2000);
    }
}

static heard_t s_heard;

static void test_streams_as_it_arrives(void)
{
    script_t s = make_body(1, 8, true, NULL);
    fake_server_t srv = { .connect_ms = 30, .status = 200, .headers_ms = 50, .logid = "20261004abc",
                          .body = s.body, .chunk = 1500, .chunk_ms = 60 };
    fake_server_set(&srv);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("你好，我是 **Muse**。"), "begin");
    listen(&s_heard, t0, 10000, 0);
    CHECK(s_heard.state == MUSE_TTS_DONE, "state %d", s_heard.state);
    CHECK(s_heard.len == s.audio_len && !memcmp(s_heard.buf, s.audio, s.audio_len), "audio %zu of %zu bytes",
          s_heard.len, s.audio_len);
    /* The first piece is playable long before the last one has arrived. */
    CHECK(s_heard.first_us < s_heard.end_us / 2, "first audio at %lld us, all of it at %lld us",
          (long long)s_heard.first_us, (long long)s_heard.end_us);

    cJSON *req = cJSON_Parse(fake_last_body());
    cJSON *p = cJSON_GetObjectItem(req, "req_params");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(p, "text")) ?: "", "你好，我是 **Muse**。"), "text");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(p, "speaker")) ?: "", "zh_female_wenrouxiaoya_uranus_bigtts"),
          "speaker");
    cJSON *audio = cJSON_GetObjectItem(p, "audio_params");
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(audio, "format")) ?: "", "mp3"), "format");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(audio, "sample_rate")) == 16000, "sample_rate");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(audio, "speech_rate")) == 10, "speech_rate");
    /* additions is a JSON object inside a string */
    const char *add_s = cJSON_GetStringValue(cJSON_GetObjectItem(p, "additions"));
    cJSON *add = cJSON_Parse(add_s ? add_s : "");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(add, "disable_markdown_filter")), "additions %s", add_s ? add_s : "(none)");
    cJSON *ctx = cJSON_GetObjectItem(add, "context_texts");
    CHECK(cJSON_GetArraySize(ctx) == 1 &&
              !strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(ctx, 0)) ?: "", "你可以用温柔亲切的语气说话"),
          "context_texts");
    cJSON_Delete(add);
    cJSON_Delete(req);
    CHECK(!strcmp(fake_last_header("X-Api-Key"), "test-key"), "X-Api-Key");
    CHECK(!strcmp(fake_last_header("X-Api-Resource-Id"), "seed-tts-2.0"), "X-Api-Resource-Id");
    CHECK(strlen(fake_last_header("X-Api-Request-Id")) == 36, "X-Api-Request-Id %s", fake_last_header("X-Api-Request-Id"));
    CHECK(!strcmp(fake_last_header("Content-Type"), "application/json"), "Content-Type");
    free_script(&s);
}

static void test_http_error(void)
{
    fake_server_t srv = { .status = 403, .headers_ms = 20, .logid = "x",
                          .body = "{\"code\":45000030,\"message\":\"resource not granted\"}", .chunk_ms = 5 };
    fake_server_set(&srv);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("hello"), "begin");
    listen(&s_heard, t0, 5000, 0);
    CHECK(s_heard.state == MUSE_TTS_FAILED && s_heard.len == 0, "state %d, %zu bytes", s_heard.state, s_heard.len);
    CHECK(s_heard.end_us < 2000000, "took %lld us", (long long)s_heard.end_us);
}

static void test_error_mid_stream_keeps_what_came(void)
{
    script_t s = make_body(2, 3, true, "{\"code\":40000000,\"message\":\"text too long\"}");
    fake_server_t srv = { .status = 200, .headers_ms = 10, .body = s.body, .chunk = 700, .chunk_ms = 10 };
    fake_server_set(&srv);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("hello"), "begin");
    listen(&s_heard, t0, 5000, 0);
    /* The first piece arrived before the error; the session plays it and ends there. */
    CHECK(s_heard.state == MUSE_TTS_FAILED, "state %d", s_heard.state);
    CHECK(s_heard.len > 0 && s_heard.len < s.audio_len && !memcmp(s_heard.buf, s.audio, s_heard.len),
          "%zu bytes", s_heard.len);
    free_script(&s);
}

static void test_cancel_then_next_message(void)
{
    script_t slow = make_body(3, 30, true, NULL);
    fake_server_t srv = { .status = 200, .headers_ms = 10, .body = slow.body, .chunk = 500, .chunk_ms = 40 };
    fake_server_set(&srv);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("a long answer"), "begin");
    listen(&s_heard, t0, 5000, 1000);
    CHECK(s_heard.len >= 1000, "heard %zu", s_heard.len);
    int closed = fake_closed();
    muse_tts_cancel();
    CHECK(muse_tts_take(s_heard.buf, sizeof(s_heard.buf)) == 0, "nothing to take once cancelled");
    CHECK(muse_tts_pending() == 0, "nothing pending once cancelled");

    script_t next = make_body(4, 4, true, NULL);
    fake_server_t srv2 = { .status = 200, .headers_ms = 10, .body = next.body, .chunk = 800, .chunk_ms = 10 };
    fake_server_set(&srv2);
    t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("the next one"), "begin");
    listen(&s_heard, t0, 5000, 0);
    CHECK(s_heard.state == MUSE_TTS_DONE, "state %d", s_heard.state);
    CHECK(s_heard.len == next.audio_len && !memcmp(s_heard.buf, next.audio, next.audio_len),
          "the next message's audio only: %zu of %zu bytes", s_heard.len, next.audio_len);
    CHECK(fake_closed() >= closed + 2, "the cancelled request's connection was closed");
    free_script(&slow);
    free_script(&next);
}

static void test_begin_replaces_a_running_one(void)
{
    script_t slow = make_body(5, 30, true, NULL);
    fake_server_t srv = { .status = 200, .headers_ms = 200, .body = slow.body, .chunk = 500, .chunk_ms = 40 };
    fake_server_set(&srv);
    CHECK(muse_tts_begin("first"), "begin");
    usleep(50000);   /* still waiting for its answer */
    script_t next = make_body(6, 3, true, NULL);
    fake_server_t srv2 = { .status = 200, .headers_ms = 10, .body = next.body, .chunk = 800, .chunk_ms = 10 };
    fake_server_set(&srv2);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("second"), "begin");
    listen(&s_heard, t0, 8000, 0);
    CHECK(s_heard.state == MUSE_TTS_DONE && s_heard.len == next.audio_len &&
              !memcmp(s_heard.buf, next.audio, next.audio_len),
          "state %d, %zu of %zu bytes", s_heard.state, s_heard.len, next.audio_len);
    free_script(&slow);
    free_script(&next);
}

static void test_stall_and_unreachable(void)
{
    script_t s = make_body(7, 6, true, NULL);
    /* Two whole pieces, part of the third, then nothing. */
    const char *third = strchr(strchr(s.body, '\n') + 1, '\n') + 1;
    fake_server_t srv = { .status = 200, .headers_ms = 10, .body = s.body, .chunk = 600, .chunk_ms = 10,
                          .stall_at = (size_t)(third - s.body) + 10 };
    fake_server_set(&srv);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("stalls"), "begin");
    listen(&s_heard, t0, 8000, 0);
    CHECK(s_heard.state == MUSE_TTS_FAILED && s_heard.len > 0 && s_heard.len < s.audio_len,
          "state %d, %zu bytes", s_heard.state, s_heard.len);
    free_script(&s);

    fake_server_t down = { .connect_fails = true, .connect_ms = 20 };
    fake_server_set(&down);
    t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("unreachable"), "begin");
    listen(&s_heard, t0, 5000, 0);
    CHECK(s_heard.state == MUSE_TTS_FAILED && s_heard.len == 0, "state %d, %zu bytes", s_heard.state, s_heard.len);
}

static void test_no_answer(void)
{
    fake_server_t srv = { .status = 200, .headers_ms = 60000, .body = "{}" };
    fake_server_set(&srv);
    int64_t t0 = esp_timer_get_time();
    CHECK(muse_tts_begin("silence"), "begin");
    listen(&s_heard, t0, 8000, 0);
    CHECK(s_heard.state == MUSE_TTS_FAILED, "state %d after %lld us", s_heard.state, (long long)s_heard.end_us);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "no-key")) {
        CHECK(!muse_tts_available(), "available without a key");
        CHECK(!muse_tts_begin("hello"), "begin without a key");
        CHECK(muse_tts_take(s_heard.buf, 16) == 0 && muse_tts_pending() == 0, "nothing queued");
        muse_tts_cancel();   /* harmless before it ever started */
    } else {
        CHECK(muse_tts_available(), "available");
        CHECK(!muse_tts_begin("") && !muse_tts_begin(NULL), "nothing to say");
        test_streams_as_it_arrives();
        test_http_error();
        test_error_mid_stream_keeps_what_came();
        test_cancel_then_next_message();
        test_begin_replaces_a_running_one();
        test_stall_and_unreachable();
        test_no_answer();
    }
    printf("%s (%d failed)\n", s_failures ? "FAILED" : "OK", s_failures);
    return s_failures ? 1 : 0;
}