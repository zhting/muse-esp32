/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs components/muse/muse_tts_stream.c over a TTS response body from a file,
 * fed in pieces of a given size, for tests/test_muse_tts_stream.py.
 *
 *   muse_tts_stream_harness BODY CHUNK [PIECE_MAX] [STOP_AFTER]
 *   muse_tts_stream_harness --base64 TEXT
 *
 * Prints "status=<more|end|error> code=<n> objects=<n> message=<...>" and then
 * the decoded audio in hex. --base64 prints the decoded bytes in hex, or -1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_tts_stream.h"

static unsigned char *s_audio;
static size_t s_audio_len, s_stop_after = (size_t)-1;

static void *grow(void *ptr, size_t size)
{
    if (!size) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static bool sink(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_audio_len >= s_stop_after) {
        return false;
    }
    s_audio = realloc(s_audio, s_audio_len + len);
    memcpy(s_audio + s_audio_len, data, len);
    s_audio_len += len;
    return true;
}

static void hex(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--base64")) {
        size_t n = strlen(argv[2]);
        unsigned char *out = malloc(n + 1);
        int got = muse_tts_base64_decode(argv[2], n, out, n);
        if (got < 0) {
            printf("-1\n");
        } else {
            hex(out, (size_t)got);
        }
        free(out);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s BODY CHUNK [PIECE_MAX] [STOP_AFTER]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    static char body[4 << 20];
    size_t len = fread(body, 1, sizeof(body), f);
    fclose(f);
    size_t chunk = strtoul(argv[2], NULL, 10);
    size_t max = argc > 3 ? strtoul(argv[3], NULL, 10) : 512 * 1024;
    if (argc > 4) {
        s_stop_after = strtoul(argv[4], NULL, 10);
    }

    muse_tts_stream_t st;
    if (!muse_tts_stream_init(&st, 64, max, grow, sink, NULL)) {
        return 2;
    }
    muse_tts_stream_status_t ss = MUSE_TTS_STREAM_MORE;
    for (size_t off = 0; off < len && ss == MUSE_TTS_STREAM_MORE; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        ss = muse_tts_stream_feed(&st, body + off, n);
    }
    if (ss == MUSE_TTS_STREAM_MORE) {
        ss = muse_tts_stream_finish(&st);
    }
    static const char *const names[] = { "more", "end", "error" };
    printf("status=%s code=%d objects=%u message=%s\n", names[ss], st.code, st.objects, st.message);
    hex(s_audio, s_audio_len);
    muse_tts_stream_free(&st);
    free(s_audio);
    return 0;
}