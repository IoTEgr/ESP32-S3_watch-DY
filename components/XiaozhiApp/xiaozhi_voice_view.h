#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOZHI_VOICE_IDLE,
    XIAOZHI_VOICE_LISTENING,
    XIAOZHI_VOICE_THINKING,
    XIAOZHI_VOICE_SPEAKING,
    XIAOZHI_VOICE_UNAVAILABLE,
} xiaozhi_voice_stage_t;

typedef struct {
    /* Per-view copies retain the primary font metrics and its fallback chain. */
    lv_font_t conversation_font, conversation_fallback;
    lv_obj_t *root, *orb, *status, *hint, *subtitle;
    lv_obj_t *activation, *code, *instructions;
    lv_timer_t *timer;
    void (*action)(void *);
    void *context;
    xiaozhi_voice_stage_t stage;
    unsigned frame;
    bool activating;
} xiaozhi_voice_view_t;

bool xiaozhi_voice_view_create(lv_obj_t *parent, xiaozhi_voice_view_t *view,
                             const lv_font_t *conversation_font,
                             void (*action)(void *), void *context);
void xiaozhi_voice_view_destroy(xiaozhi_voice_view_t *view);
void xiaozhi_voice_view_status(xiaozhi_voice_view_t *view, const char *text,
                             xiaozhi_voice_stage_t stage);
void xiaozhi_voice_view_chat(xiaozhi_voice_view_t *view, const char *text);
void xiaozhi_voice_view_activation(xiaozhi_voice_view_t *view, const char *code,
                                 const char *message, bool visible);

#ifdef __cplusplus
}
#endif
