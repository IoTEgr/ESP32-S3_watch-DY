/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "xiaozhi_voice_view.h"

namespace esp_brookesia::apps {

class XiaozhiUi {
public:
    using ActionCallback = void (*)(void *context);

    XiaozhiUi() = default;
    ~XiaozhiUi();

    XiaozhiUi(const XiaozhiUi &) = delete;
    XiaozhiUi &operator=(const XiaozhiUi &) = delete;

    bool preload();
    bool create(
        lv_obj_t *parent,
        ActionCallback action_callback,
        void *action_context,
        bool multiline_subtitle = false
    );
    void destroy();

    void setNetworkReady(bool ready);
    void setStatus(const char *status);
    void setChatMessage(const char *role, const char *text);
    void clearChatMessages();
    void setEmotion(const char *emotion);
    void setActivation(const char *code, const char *message, bool visible);

    lv_obj_t *root() const
    {
        return _root;
    }

private:
    bool loadTextFont();
    void destroyView();
    void releaseTextFont();
    lv_obj_t *_root = nullptr;
    xiaozhi_voice_view_t _view = {};

    uint8_t *_font_data = nullptr;
    lv_font_t *_cbin_font = nullptr;
    const lv_font_t *_text_font = nullptr;
};

} // namespace esp_brookesia::apps
