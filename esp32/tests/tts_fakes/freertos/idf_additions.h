/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t size, size_t trigger, uint32_t caps);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           unsigned prio, TaskHandle_t *out, int core, uint32_t caps);