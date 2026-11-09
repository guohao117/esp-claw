/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include <stdio.h>
#include <stdlib.h>

#include "claw_paths.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *system_ui_skip_fs_prefix(const char *path)
{
    if (!path) {
        return NULL;
    }
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':') {
        path += 2;
    }
    while (*path == '/' || *path == '\\') {
        path++;
    }
    return path;
}

static esp_err_t system_ui_to_vfs_path(claw_path_root_t root, const char *path, char *out, size_t out_size)
{
    const char *rel = system_ui_skip_fs_prefix(path);

    ESP_RETURN_ON_FALSE(rel && rel[0], ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "fs path is empty");
    return claw_paths_join(root, rel, out, out_size);
}

static esp_err_t system_ui_acquire_font_data(const char *font_path,
                                             font_service_handle_t *out_blob,
                                             font_service_view_t *out_view)
{
    char vfs_path[SYSTEM_UI_PATH_MAX] = {0};
    esp_err_t data_ret;

    ESP_RETURN_ON_FALSE(out_blob && out_view, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "font output missing");
    *out_blob = NULL;
    out_view->data = NULL;
    out_view->size = 0;

    ESP_RETURN_ON_ERROR(system_ui_to_vfs_path(CLAW_PATH_DATA, font_path, vfs_path, sizeof(vfs_path)),
                        SYSTEM_UI_TAG, "resolve ui font data path failed");
    data_ret = font_service_acquire_file(vfs_path, out_blob, out_view);
    if (data_ret != ESP_ERR_NOT_FOUND) {
        return data_ret;
    }

    ESP_RETURN_ON_ERROR(system_ui_to_vfs_path(CLAW_PATH_SYSTEM, font_path, vfs_path, sizeof(vfs_path)),
                        SYSTEM_UI_TAG, "resolve ui font system path failed");
    return font_service_acquire_file(vfs_path, out_blob, out_view);
}

void system_ui_create_font_locked(const char *font_path, uint32_t font_size)
{
#if LV_USE_TINY_TTF
    int written;
    font_service_handle_t font_blob = NULL;
    font_service_view_t font_view = {0};

    if (!font_path || !font_path[0]) {
        font_path = SYSTEM_UI_DEFAULT_FONT_PATH;
    }
    if (font_size == 0) {
        font_size = SYSTEM_UI_DEFAULT_FONT_SIZE;
    }

    written = snprintf(s_ui.font_path, sizeof(s_ui.font_path), "%s", system_ui_skip_fs_prefix(font_path));
    if (written <= 0 || (size_t)written >= sizeof(s_ui.font_path)) {
        ESP_LOGW(SYSTEM_UI_TAG, "ui font path too long");
        return;
    }

    if (system_ui_acquire_font_data(s_ui.font_path, &font_blob, &font_view) != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "ui font data load failed: %s", s_ui.font_path);
        return;
    }

    s_ui.font = lv_tiny_ttf_create_data_ex(font_view.data,
                                           font_view.size,
                                           font_size,
                                           LV_FONT_KERNING_NORMAL,
                                           LV_TINY_TTF_CACHE_GLYPH_CNT);
    if (!s_ui.font) {
        font_service_release(font_blob);
        ESP_LOGW(SYSTEM_UI_TAG, "ui font load failed: %s", s_ui.font_path);
        return;
    }
    uint32_t clock_font_size = (uint32_t)system_ui_clamp_i32(system_ui_short_side_from(s_ui.width, s_ui.height) * 22 / 100, 48, SYSTEM_UI_CLOCK_FONT_SIZE);
    s_ui.clock_font = lv_tiny_ttf_create_data_ex(font_view.data,
                                                 font_view.size,
                                                 clock_font_size,
                                                 LV_FONT_KERNING_NORMAL,
                                                 LV_TINY_TTF_CACHE_GLYPH_CNT);
    if (!s_ui.clock_font) {
        ESP_LOGW(SYSTEM_UI_TAG, "ui clock font load failed: %s", s_ui.font_path);
    }
    s_ui.font_blob = font_blob;
    s_ui.font_data = font_view.data;
    s_ui.font_data_size = font_view.size;
    lv_obj_set_style_text_font(lv_layer_top(), s_ui.font, 0);
    ESP_LOGI(SYSTEM_UI_TAG, "ui font loaded: %s, size=%lu",
             s_ui.font_path, (unsigned long)font_size);
#else
    (void)font_path;
    (void)font_size;
    ESP_LOGW(SYSTEM_UI_TAG, "ui font disabled: LVGL tiny_ttf file support is off");
#endif
}

void system_ui_destroy_font_locked(void)
{
#if LV_USE_TINY_TTF
    if (s_ui.clock_font) {
        lv_tiny_ttf_destroy(s_ui.clock_font);
        s_ui.clock_font = NULL;
    }
    if (s_ui.font) {
        lv_tiny_ttf_destroy(s_ui.font);
        s_ui.font = NULL;
    }
#endif
    font_service_release(s_ui.font_blob);
    s_ui.font_blob = NULL;
    s_ui.font_data = NULL;
    s_ui.font_data_size = 0;
    s_ui.font_path[0] = '\0';
}
