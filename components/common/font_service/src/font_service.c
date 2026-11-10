/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "font_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_paths.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FONT_SERVICE_PATH_MAX 256U

static const char *TAG = "font_service";

struct font_service_blob {
    char path[FONT_SERVICE_PATH_MAX];
    uint8_t *data;
    size_t size;
    size_t ref_count;
    struct font_service_blob *next;
};

static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static struct font_service_blob *s_blobs;

static esp_err_t font_service_ensure_lock(void)
{
    portENTER_CRITICAL(&s_init_lock);
    bool ready = (s_lock != NULL);
    portEXIT_CRITICAL(&s_init_lock);
    if (ready) {
        return ESP_OK;
    }

    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_init_lock);
    if (!s_lock) {
        s_lock = candidate;
        candidate = NULL;
    }
    portEXIT_CRITICAL(&s_init_lock);

    if (candidate) {
        vSemaphoreDelete(candidate);
    }
    return ESP_OK;
}

static esp_err_t font_service_normalize_vfs_path(const char *vfs_path, char *out, size_t out_size)
{
    size_t j = 0;

    if (!vfs_path || vfs_path[0] != '/' || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; vfs_path[i] != '\0'; ++i) {
        char c = vfs_path[i];
        if (c == '\\') {
            c = '/';
        }
        if (c == '/' && j > 0 && out[j - 1] == '/') {
            continue;
        }
        if (j + 1 >= out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        out[j++] = c;
    }

    while (j > 1 && out[j - 1] == '/') {
        --j;
    }
    out[j] = '\0';
    return j > 1 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static struct font_service_blob *font_service_find_locked(const char *canonical_path)
{
    for (struct font_service_blob *cur = s_blobs; cur != NULL; cur = cur->next) {
        if (strcmp(cur->path, canonical_path) == 0) {
            return cur;
        }
    }
    return NULL;
}

static esp_err_t font_service_read_file(const char *vfs_path, uint8_t **out_data, size_t *out_size)
{
    FILE *fp = fopen(vfs_path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        ESP_LOGW(TAG, "font size seek failed: %s", vfs_path);
        return ESP_FAIL;
    }

    long file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        ESP_LOGW(TAG, "font file is empty: %s", vfs_path);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        ESP_LOGW(TAG, "font rewind failed: %s", vfs_path);
        return ESP_FAIL;
    }

    uint8_t *data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_8BIT);
    }
    if (!data) {
        fclose(fp);
        ESP_LOGW(TAG, "font allocation failed: %ld bytes (%s)", file_size, vfs_path);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);
    if (read_len != (size_t)file_size) {
        free(data);
        ESP_LOGW(TAG, "font read failed: %s, read=%u size=%ld",
                 vfs_path, (unsigned)read_len, file_size);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_size = (size_t)file_size;
    return ESP_OK;
}

esp_err_t font_service_acquire_file(const char *vfs_path,
                                    font_service_handle_t *out_handle,
                                    font_service_view_t *out_view)
{
    char canonical_path[FONT_SERVICE_PATH_MAX];
    esp_err_t err;

    ESP_RETURN_ON_FALSE(vfs_path && out_handle && out_view,
                        ESP_ERR_INVALID_ARG, TAG, "acquire argument missing");
    *out_handle = NULL;
    out_view->data = NULL;
    out_view->size = 0;

    err = font_service_normalize_vfs_path(vfs_path, canonical_path, sizeof(canonical_path));
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(font_service_ensure_lock(), TAG, "lock init failed");
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    struct font_service_blob *blob = font_service_find_locked(canonical_path);
    if (blob != NULL) {
        blob->ref_count++;
        *out_handle = blob;
        out_view->data = blob->data;
        out_view->size = blob->size;
        size_t refs = blob->ref_count;
        size_t size = blob->size;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "reused '%s' (%u bytes, refs=%u)",
                 canonical_path, (unsigned)size, (unsigned)refs);
        return ESP_OK;
    }

    uint8_t *data = NULL;
    size_t size = 0;
    err = font_service_read_file(canonical_path, &data, &size);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    blob = (struct font_service_blob *)calloc(1, sizeof(*blob));
    if (!blob) {
        free(data);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(blob->path, canonical_path, sizeof(blob->path));
    blob->data = data;
    blob->size = size;
    blob->ref_count = 1;
    blob->next = s_blobs;
    s_blobs = blob;

    *out_handle = blob;
    out_view->data = blob->data;
    out_view->size = blob->size;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "loaded '%s' (%u bytes, refs=1)",
             canonical_path, (unsigned)size);
    return ESP_OK;
}

esp_err_t font_service_acquire_default(font_service_handle_t *out_handle,
                                       font_service_view_t *out_view)
{
    char vfs_path[FONT_SERVICE_PATH_MAX];
    ESP_RETURN_ON_ERROR(
        claw_paths_join(CLAW_PATH_SYSTEM, FONT_SERVICE_DEFAULT_REL_PATH,
                        vfs_path, sizeof(vfs_path)),
        TAG, "resolve default system font path failed");
    return font_service_acquire_file(vfs_path, out_handle, out_view);
}

void font_service_release(font_service_handle_t handle)
{
    if (!handle || !s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    struct font_service_blob **cursor = &s_blobs;
    while (*cursor != NULL) {
        struct font_service_blob *cur = *cursor;
        if (cur == handle) {
            char path_copy[FONT_SERVICE_PATH_MAX];
            strlcpy(path_copy, cur->path, sizeof(path_copy));
            if (cur->ref_count > 0) {
                cur->ref_count--;
            }
            if (cur->ref_count == 0) {
                *cursor = cur->next;
                size_t size = cur->size;
                free(cur->data);
                free(cur);
                xSemaphoreGive(s_lock);
                ESP_LOGI(TAG, "unloaded '%s' (%u bytes)", path_copy, (unsigned)size);
                return;
            }
            size_t refs = cur->ref_count;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "released '%s' (refs=%u)", path_copy, (unsigned)refs);
            return;
        }
        cursor = &cur->next;
    }

    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "release called with unknown font handle %p", (void *)handle);
}

size_t font_service_ref_count(font_service_handle_t handle)
{
    size_t refs = 0;
    if (!handle || !s_lock) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    for (struct font_service_blob *cur = s_blobs; cur != NULL; cur = cur->next) {
        if (cur == handle) {
            refs = cur->ref_count;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return refs;
}
