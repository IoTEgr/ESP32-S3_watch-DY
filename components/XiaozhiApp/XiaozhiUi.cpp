/* SPDX-License-Identifier: Apache-2.0 */
#include "XiaozhiUi.hpp"
#include "XiaozhiLocalization.hpp"
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "cbin_font.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

LV_FONT_DECLARE(font_puhui_basic_30_4);

namespace esp_brookesia::apps {
namespace {
constexpr char TAG[] = "XiaozhiUi";
constexpr char COMMON_FONT_PATH[] = BSP_SPIFFS_MOUNT_POINT "/xiaozhi/font.bin";
constexpr size_t MAX_COMMON_FONT_SIZE = 4 * 1024 * 1024;
}

XiaozhiUi::~XiaozhiUi()
{
    destroy();
    releaseTextFont();
}

bool XiaozhiUi::preload()
{
    _text_font = &font_puhui_basic_30_4;
    if (_cbin_font) _text_font = _cbin_font;
    else (void)loadTextFont();
    return true;
}

bool XiaozhiUi::create(lv_obj_t *parent, ActionCallback action_callback,
                       void *action_context, bool multiline_subtitle)
{
    (void)multiline_subtitle;
    destroyView();
    _text_font = _cbin_font ? _cbin_font : &font_puhui_basic_30_4;
    if (!xiaozhi_voice_view_create(parent, &_view, _text_font,
                                   action_callback, action_context)) return false;
    _root = _view.root;
    return true;
}

void XiaozhiUi::destroy() { destroyView(); }
void XiaozhiUi::destroyView()
{
    xiaozhi_voice_view_destroy(&_view);
    _root = nullptr;
}

void XiaozhiUi::setNetworkReady(bool ready)
{
    // Network-required state is presented below the orb by setStatus().
    (void)ready;
}

void XiaozhiUi::setStatus(const char *status)
{
    auto stage = XIAOZHI_VOICE_IDLE;
    if (!status) status = "";
    if (!strcmp(status, xiaozhiUiText(XiaozhiUiText::Listening)))
        stage = XIAOZHI_VOICE_LISTENING;
    else if (!strcmp(status, xiaozhiUiText(XiaozhiUiText::Processing)))
        stage = XIAOZHI_VOICE_THINKING;
    else if (!strcmp(status, xiaozhiUiText(XiaozhiUiText::Speaking)))
        stage = XIAOZHI_VOICE_SPEAKING;
    else if (!strcmp(status, xiaozhiUiText(XiaozhiUiText::Error)) ||
             !strcmp(status, xiaozhiUiText(XiaozhiUiText::NetworkRequired)))
        stage = XIAOZHI_VOICE_UNAVAILABLE;
    xiaozhi_voice_view_status(&_view, status, stage);
}

void XiaozhiUi::setChatMessage(const char *role, const char *message)
{
    (void)role;
    xiaozhi_voice_view_chat(&_view, message);
}

void XiaozhiUi::clearChatMessages() { setChatMessage("system", ""); }

void XiaozhiUi::setEmotion(const char *emotion)
{
    // Retain the conversational orb instead of replacing it with emoji.
    (void)emotion;
}

void XiaozhiUi::setActivation(const char *code, const char *message, bool visible)
{
    xiaozhi_voice_view_activation(&_view, code, message, visible);
}

bool XiaozhiUi::loadTextFont()
{
    if (_cbin_font) {
        _text_font = _cbin_font;
        return true;
    }
    FILE *file = fopen(COMMON_FONT_PATH, "rb");
    if (!file) {
        ESP_LOGW(TAG, "Common font is unavailable: %s", COMMON_FONT_PATH);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || static_cast<size_t>(file_size) > MAX_COMMON_FONT_SIZE ||
            fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Common font has an invalid size: %ld", file_size);
        fclose(file);
        return false;
    }

    size_t size = static_cast<size_t>(file_size);
    auto *data = static_cast<uint8_t *>(
                     heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                 );
    if (!data) {
        ESP_LOGW(TAG, "No PSRAM for common font (%u bytes)", static_cast<unsigned>(size));
        fclose(file);
        return false;
    }

    size_t read_size = fread(data, 1, size, file);
    fclose(file);
    if (read_size != size) {
        ESP_LOGW(TAG, "Common font read was incomplete (%u/%u)",
                 static_cast<unsigned>(read_size), static_cast<unsigned>(size));
        heap_caps_free(data);
        return false;
    }

    if (!bsp_display_lock(0)) {
        ESP_LOGW(TAG, "Unable to lock LVGL while decoding the common font");
        heap_caps_free(data);
        return false;
    }
    lv_font_t *font = cbin_font_create(data);
    bsp_display_unlock();

    if (!font) {
        ESP_LOGW(TAG, "Common font could not be decoded");
        heap_caps_free(data);
        return false;
    }

    _font_data = data;
    _cbin_font = font;
    _text_font = font;
    ESP_LOGI(TAG, "Loaded common font from PSRAM (%u bytes)", static_cast<unsigned>(size));
    return true;
}

void XiaozhiUi::releaseTextFont()
{
    if (_cbin_font && !bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Unable to lock LVGL while releasing the common font");
        return;
    }
    if (_cbin_font) {
        cbin_font_delete(_cbin_font);
        _cbin_font = nullptr;
        bsp_display_unlock();
    }
    if (_font_data) {
        heap_caps_free(_font_data);
        _font_data = nullptr;
    }
}

} // namespace esp_brookesia::apps
