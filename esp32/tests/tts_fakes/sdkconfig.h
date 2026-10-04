/* SPDX-License-Identifier: Apache-2.0 */
/* Settings for tests/muse_tts_harness.c; the build can override the key. */
#pragma once
#ifndef CONFIG_MUSE_TTS_API_KEY
#define CONFIG_MUSE_TTS_API_KEY "test-key"
#endif
#define CONFIG_MUSE_TTS_RESOURCE_ID "seed-tts-2.0"
#define CONFIG_MUSE_TTS_SPEAKER "zh_female_wenrouxiaoya_uranus_bigtts"
#define CONFIG_MUSE_TTS_SPEECH_RATE 10
#define CONFIG_MUSE_TTS_STYLE "你可以用温柔亲切的语气说话"