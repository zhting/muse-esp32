/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DEFAULT (1 << 12)
static inline void *heap_caps_malloc_prefer(size_t size, size_t num, ...) { (void)num; return malloc(size); }
static inline void *heap_caps_realloc_prefer(void *p, size_t size, size_t num, ...)
{
    (void)num;
    if (!size) { free(p); return NULL; }
    return realloc(p, size);
}
static inline void heap_caps_free(void *p) { free(p); }