/* SPDX-License-Identifier: Apache-2.0 */
/* The scripted TTS server behind tests/tts_fakes/esp_http_client.h. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool connect_fails;
    int connect_ms;            /* open(): DNS + TCP + TLS */
    int status;
    int headers_ms;            /* after the request, until the response headers */
    const char *logid;
    const char *body;
    size_t chunk;              /* bytes per chunk of the body */
    int chunk_ms;              /* before each chunk */
    size_t stall_at;           /* stop sending (without ending) at this offset; 0: never */
} fake_server_t;

/* Takes effect for the next request opened. */
void fake_server_set(const fake_server_t *s);
/* The last request: its body and a header's value ("" if not sent). */
const char *fake_last_body(void);
const char *fake_last_header(const char *key);
int fake_opened(void);
int fake_closed(void);