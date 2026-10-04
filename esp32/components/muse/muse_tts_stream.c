/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frames and decodes Volcengine's streaming TTS body; see muse_tts_stream.h.
 */

#include "muse_tts_stream.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

bool muse_tts_stream_init(muse_tts_stream_t *s, size_t cap, size_t max, muse_tts_realloc_t grow,
                          muse_tts_sink_t sink, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->grow = grow;
    s->sink = sink;
    s->ctx = ctx;
    s->max = max > cap ? max : cap;
    s->buf = grow(NULL, cap);
    if (!s->buf) {
        return false;
    }
    s->cap = cap;
    return true;
}

void muse_tts_stream_free(muse_tts_stream_t *s)
{
    if (s->buf) {
        s->grow(s->buf, 0);
    }
    s->buf = NULL;
    s->cap = s->len = 0;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

int muse_tts_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') {
            break;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        int v = b64_value(c);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) {
                return -1;
            }
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

static muse_tts_stream_status_t fail(muse_tts_stream_t *s, const char *why)
{
    if (!s->message[0]) {
        strncpy(s->message, why, sizeof(s->message) - 1);
    }
    return s->status = MUSE_TTS_STREAM_ERROR;
}

/* One whole object in buf[0..len). */
static muse_tts_stream_status_t object_done(muse_tts_stream_t *s)
{
    s->objects++;
    if (s->overflow) {
        char why[64];
        snprintf(why, sizeof(why), "a piece over %u bytes", (unsigned)s->max);
        return fail(s, why);
    }
    cJSON *j = cJSON_ParseWithLength(s->buf, s->len);
    if (!j) {
        return fail(s, "not JSON");
    }
    cJSON *code = cJSON_GetObjectItem(j, "code");
    s->code = cJSON_IsNumber(code) ? (int)code->valuedouble : 0;
    if (s->code != 0 && s->code != MUSE_TTS_END_CODE) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(j, "message"));
        fail(s, msg && msg[0] ? msg : "error code");
        cJSON_Delete(j);
        return s->status;
    }
    /* cJSON has its own copy of the string, so the audio can go where the object was. */
    const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(j, "data"));
    if (data && data[0]) {
        int n = muse_tts_base64_decode(data, strlen(data), (uint8_t *)s->buf, s->cap);
        if (n < 0) {
            cJSON_Delete(j);
            return fail(s, "bad base64");
        }
        if (n > 0) {
            s->audio_bytes += (size_t)n;
            if (!s->sink((const uint8_t *)s->buf, (size_t)n, s->ctx)) {
                cJSON_Delete(j);
                return fail(s, "stopped");
            }
        }
    }
    cJSON_Delete(j);
    if (s->code == MUSE_TTS_END_CODE) {
        s->status = MUSE_TTS_STREAM_END;
    }
    return s->status;
}

static bool put(muse_tts_stream_t *s, char c)
{
    if (s->overflow) {
        return true;
    }
    if (s->len + 1 >= s->cap) {
        size_t cap = s->cap * 2 < s->max ? s->cap * 2 : s->max;
        char *grown = cap > s->cap ? s->grow(s->buf, cap) : NULL;
        if (!grown) {
            s->overflow = true;   /* keep framing it, then report it */
            return true;
        }
        s->buf = grown;
        s->cap = cap;
    }
    s->buf[s->len++] = c;
    return true;
}

muse_tts_stream_status_t muse_tts_stream_feed(muse_tts_stream_t *s, const char *data, size_t len)
{
    for (size_t i = 0; i < len && s->status == MUSE_TTS_STREAM_MORE; i++) {
        char c = data[i];
        if (s->depth == 0) {
            if (c != '{') {
                continue;   /* newlines, SSE "data:" prefixes and blank lines between objects */
            }
            s->len = 0;
            s->overflow = false;
            s->in_string = s->escaped = false;
        }
        put(s, c);
        if (s->in_string) {
            if (s->escaped) {
                s->escaped = false;
            } else if (c == '\\') {
                s->escaped = true;
            } else if (c == '"') {
                s->in_string = false;
            }
        } else if (c == '"') {
            s->in_string = true;
        } else if (c == '{' || c == '[') {
            s->depth++;
        } else if ((c == '}' || c == ']') && --s->depth == 0) {
            object_done(s);
        }
    }
    return s->status;
}

muse_tts_stream_status_t muse_tts_stream_finish(muse_tts_stream_t *s)
{
    if (s->status != MUSE_TTS_STREAM_MORE) {
        return s->status;
    }
    if (s->depth) {
        return fail(s, "the body ended mid-piece");
    }
    if (!s->audio_bytes) {
        return fail(s, "no audio");
    }
    return s->status = MUSE_TTS_STREAM_END;
}