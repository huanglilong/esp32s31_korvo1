/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL PSRAM memory pool allocator.
 *
 * Provides lvgl_psram_alloc() which allocates the LVGL TLSF memory pool
 * from PSRAM instead of internal SRAM. This saves ~64KB of DIRAM.
 *
 * Used via CMake compile definitions:
 *   -DLV_MEM_POOL_INCLUDE=\"lvgl_psram_mem.h\"
 *   -DLV_MEM_POOL_ALLOC=lvgl_psram_alloc
 */
#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

/**
 * @brief Allocate LVGL's TLSF memory pool from PSRAM.
 *
 * LVGL calls this macro once during lv_mem_init() to obtain the
 * memory pool backing the TLSF allocator. By using heap_caps_malloc
 * with MALLOC_CAP_SPIRAM, the 64KB pool resides in PSRAM.
 *
 * @param size  Size of the pool (LV_MEM_SIZE, typically 64KB).
 * @return Pointer to the allocated pool, or NULL on failure.
 */
static inline void *lvgl_psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}