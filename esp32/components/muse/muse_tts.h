/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Speech for Muse's replies: Volcengine's streaming TTS (豆包语音合成) on a
 * task of its own. The MP3 is handed over as it arrives, so a reply starts
 * speaking about a second after its text is complete.
 *
 * Used from one task (the Muse chat session's): begin a message, take its MP3
 * as it comes, and watch the state for the end.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_TTS_RUNNING,   /* connecting, or speech still arriving */
    MUSE_TTS_DONE,      /* all of it is queued (some may be left to take) */
    MUSE_TTS_FAILED,    /* gave up; what did arrive is queued */
} muse_tts_state_t;

/* An API key is set (CONFIG_MUSE_TTS_API_KEY); without one, replies stay text. */
bool muse_tts_available(void);

/* Starts speaking text (UTF-8, Markdown allowed), cancelling any speech in progress.
 * False if TTS isn't set up or memory ran out. */
bool muse_tts_begin(const char *text);

/* Takes up to cap bytes of the MP3 that has arrived. Never blocks. */
size_t muse_tts_take(uint8_t *out, size_t cap);

/* Bytes of MP3 waiting to be taken. */
size_t muse_tts_pending(void);

/* The latest request's state. Read it before taking: once it says DONE or
 * FAILED, everything that request will deliver is already waiting. */
muse_tts_state_t muse_tts_state(void);

/* Stops the latest request and drops what it queued. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif