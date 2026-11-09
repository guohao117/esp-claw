/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default relative path of the system CJK fallback font under CLAW_PATH_SYSTEM / CLAW_PATH_DATA.
 */
#define FONT_SERVICE_DEFAULT_REL_PATH "fonts/NotoSansSC-Regular-sub.ttf"

/**
 * @brief Opaque handle representing a reference-counted shared font resource.
 */
typedef struct font_service_blob *font_service_handle_t;

/**
 * @brief Read-only memory view of a shared font resource.
 *
 *        In FreeRTOS's flat memory space, multiple rendering engines (GSP FreeType,
 *        LVGL TinyTTF, system_ui) can concurrently read `data` without locking
 *        as long as each consumer holds an active `font_service_handle_t` reference.
 */
typedef struct {
    const uint8_t *data;
    size_t size;
} font_service_view_t;

/**
 * @brief Acquire a shared read-only font resource by resolved VFS path.
 *
 *        If `vfs_path` is already loaded in memory, increments its reference count
 *        and returns the existing buffer view immediately without disk I/O or extra
 *        PSRAM allocation. Otherwise loads the file into PSRAM (falling back to
 *        8-bit internal heap if PSRAM is unavailable) with reference count 1.
 *
 * @param[in]  vfs_path    Resolved absolute VFS path (e.g. "/system/fonts/NotoSansSC-Regular-sub.ttf")
 * @param[out] out_handle  Receives the reference handle (must be released via font_service_release)
 * @param[out] out_view    Receives the read-only pointer and byte size
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if any argument is NULL or path is invalid
 *         - ESP_ERR_NOT_FOUND if the file does not exist
 *         - ESP_ERR_INVALID_SIZE if the file is empty or path is too long
 *         - ESP_ERR_NO_MEM if memory allocation fails
 *         - ESP_FAIL on file read failure
 */
esp_err_t font_service_acquire_file(const char *vfs_path,
                                    font_service_handle_t *out_handle,
                                    font_service_view_t *out_view);

/**
 * @brief Acquire the default system CJK font (CLAW_PATH_SYSTEM / FONT_SERVICE_DEFAULT_REL_PATH).
 *
 * @param[out] out_handle  Receives the reference handle
 * @param[out] out_view    Receives the read-only pointer and byte size
 *
 * @return ESP_OK on success, or an esp_err_t error code
 */
esp_err_t font_service_acquire_default(font_service_handle_t *out_handle,
                                       font_service_view_t *out_view);

/**
 * @brief Release a reference to a shared font resource.
 *
 *        Decrements the reference count. When the reference count reaches zero,
 *        the backing buffer is freed from PSRAM. Passing NULL is a safe no-op.
 *
 * @param[in] handle  Handle previously returned by font_service_acquire_*
 */
void font_service_release(font_service_handle_t handle);

/**
 * @brief Query the current reference count of a font handle (for diagnostics/testing).
 *
 * @param[in] handle  Font resource handle
 * @return Reference count, or 0 if handle is NULL/invalid
 */
size_t font_service_ref_count(font_service_handle_t handle);

#ifdef __cplusplus
}
#endif
