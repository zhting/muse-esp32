/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Volcengine's streaming TTS (HTTP chunked or SSE, V3) answers with one JSON
 * object per piece of audio:
 *
 *   {"code":0,"message":"","data":"<base64 audio>"}    any number of these
 *   {"code":20000000,"message":"ok","usage":{...}}     the end
 *
 * Any other non-zero code is an error. This frames the objects however the
 * body is chunked (newlines, SSE "data:" prefixes and blank lines between them
 * are skipped) and hands the audio to a sink as each one is decoded. Plain C,
 * so the host tests can run it (tests/test_muse_tts_stream.py).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_TTS_END_CODE 20000000

typedef enum {
    MUSE_TTS_STREAM_MORE,    /* waiting for more of the body */
    MUSE_TTS_STREAM_END,     /* the end object arrived */
    MUSE_TTS_STREAM_ERROR,   /* an error object, a bad one, or the sink said stop */
} muse_tts_stream_status_t;

/* Takes decoded audio. Returning false stops the stream (a cancel). */
typedef bool (*muse_tts_sink_t)(const uint8_t *data, size_t len, void *ctx);
/* realloc() that frees ptr (and returns NULL) when size is 0. */
typedef void *(*muse_tts_realloc_t)(void *ptr, size_t size);

typedef struct {
    char *buf;               /* the object so far; audio is decoded into it too */
    size_t len, cap, max;    /* buf grows from its first cap up to max */
    int depth;               /* JSON nesting; 0 between objects */
    bool in_string, escaped, overflow;
    muse_tts_realloc_t grow;
    muse_tts_sink_t sink;
    void *ctx;
    muse_tts_stream_status_t status;
    int code;                /* the last object's code */
    char message[96];        /* why it failed, for the log */
    size_t audio_bytes;      /* decoded so far */
    unsigned objects;        /* objects seen */
} muse_tts_stream_t;

/* cap: the buffer to start with; it grows with grow() up to max. */
bool muse_tts_stream_init(muse_tts_stream_t *s, size_t cap, size_t max, muse_tts_realloc_t grow,
                          muse_tts_sink_t sink, void *ctx);
void muse_tts_stream_free(muse_tts_stream_t *s);

/* Feeds the next piece of the body. Once it returns END or ERROR, it stays there. */
muse_tts_stream_status_t muse_tts_stream_feed(muse_tts_stream_t *s, const char *data, size_t len);

/* The body ended. An unfinished object, or no end object after no audio, is an error;
 * audio without the end object counts as the end. */
muse_tts_stream_status_t muse_tts_stream_finish(muse_tts_stream_t *s);

/* Standard or URL-safe base64 to bytes, skipping whitespace, stopping at '='.
 * Returns the bytes written, or -1 for a bad character or too little room. */
int muse_tts_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif