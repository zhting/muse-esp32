/* SPDX-License-Identifier: Apache-2.0 */
/* Just enough FreeRTOS, on pthreads, for components/muse/muse_tts.c. Ticks are ms. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))