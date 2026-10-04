/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct fake_stream *StreamBufferHandle_t;
size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t wait);
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t len, TickType_t wait);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s);
BaseType_t xStreamBufferReset(StreamBufferHandle_t s);