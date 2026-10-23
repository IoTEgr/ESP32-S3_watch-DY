#include "xiaozhi_voice_view.h"
#include "xiaozhi_wake_config.h"
#include <string.h>

LV_FONT_DECLARE(xiaozhi_status_font);
LV_FONT_DECLARE(xiaozhi_hint_font);
LV_FONT_DECLARE(xiaozhi_conversation_extra_font);
extern const lv_image_dsc_t *const xiaozhi_orb_frames[12];

static void hidden(lv_obj_t *obj, bool hide)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hide) return;
    if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void text(lv_obj_t *label, const char *value)
{
    if (!value) value = "";
    if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}

static void animate(lv_timer_t *timer)
{
    xiaozhi_voice_view_t *v = lv_timer_get_user_data(timer);
    /* One opaque RGB565 image: no live blur, scaling, decoder or frame heap. */
    if (!lv_obj_is_visible(v->root)) return;
    v->frame = (v->frame + 1) % 12;
    lv_image_set_src(v->orb, xiaozhi_orb_frames[v->frame]);
}

static void clicked(lv_event_t *event)
{
    xiaozhi_voice_view_t *v = lv_event_get_user_data(event);
    if (v->action) v->action(v->context);
}

static void root_deleted(lv_event_t *event)
{
    xiaozhi_voice_view_t *v = lv_event_get_user_data(event);
    if (v->timer) lv_timer_delete(v->timer);
    v->timer = NULL;
    v->root = NULL;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       int width, int y)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(obj, 5, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(obj, width);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(obj, "");
    return obj;
}

bool xiaozhi_voice_view_create(lv_obj_t *parent, xiaozhi_voice_view_t *v,
                             const lv_font_t *conversation_font,
                             void (*action)(void *), void *context)
{
    if (!parent || !v || !conversation_font) return false;
    memset(v, 0, sizeof(*v));
    /* The common SPIFFS font lacks 珈. Supplement chat/activation text at its
     * own 30px size, including 嘉 when the smaller built-in font is in use.
     * Keep the shared/const fonts untouched and preserve their metrics. */
    v->conversation_font = *conversation_font;
    v->conversation_fallback = xiaozhi_conversation_extra_font;
    v->conversation_fallback.fallback = conversation_font->fallback;
    v->conversation_font.fallback = &v->conversation_fallback;
    v->action = action;
    v->context = context;
    lv_obj_update_layout(parent);
    int width = lv_obj_get_width(parent), height = lv_obj_get_height(parent);
    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, width, height);
    lv_obj_set_style_bg_color(v->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v->root, root_deleted, LV_EVENT_DELETE, v);

    v->orb = lv_image_create(v->root);
    lv_image_set_src(v->orb, xiaozhi_orb_frames[0]);
    lv_obj_align(v->orb, LV_ALIGN_TOP_MID, 0, height * 17 / 100);
    lv_obj_add_flag(v->orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v->orb, clicked, LV_EVENT_CLICKED, v);
    v->status = label(v->root, &xiaozhi_status_font, 0xEEEEF2,
                      width - 48, height * 58 / 100);
    v->hint = label(v->root, &xiaozhi_hint_font, 0x85858F,
                    width - 48, height * 67 / 100);
    lv_label_set_text(v->hint, XIAOZHI_WAKE_HINT_TEXT);
    v->subtitle = label(v->root, &v->conversation_font, 0xCBCBD3, width - 56, 0);
    lv_obj_set_height(v->subtitle, height * 20 / 100);
    lv_obj_align(v->subtitle, LV_ALIGN_BOTTOM_MID, 0, -22);
    hidden(v->subtitle, true);

    v->activation = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->activation);
    lv_obj_set_size(v->activation, width - 56, height * 29 / 100);
    lv_obj_align(v->activation, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_remove_flag(v->activation, LV_OBJ_FLAG_SCROLLABLE);
    v->code = label(v->activation, &v->conversation_font, 0xB5CEFF, width - 56, 0);
    v->instructions = label(v->activation, &v->conversation_font, 0xADADB8,
                            width - 56, 46);
    lv_obj_set_height(v->instructions, height * 29 / 100 - 46);
    hidden(v->activation, true);
    v->timer = lv_timer_create(animate, 240, v);
    return true;
}

void xiaozhi_voice_view_destroy(xiaozhi_voice_view_t *v)
{
    if (!v) return;
    if (v->root) lv_obj_delete(v->root);
    /* The delete callback also handles a parent screen being cleaned first. */
    if (v->timer) lv_timer_delete(v->timer);
    memset(v, 0, sizeof(*v));
}

void xiaozhi_voice_view_status(xiaozhi_voice_view_t *v, const char *value,
                             xiaozhi_voice_stage_t stage)
{
    if (!v->root) return;
    text(v->status, value);
    if (v->stage == stage) return;
    v->stage = stage;
    if (v->timer) {
        if (stage == XIAOZHI_VOICE_UNAVAILABLE) lv_timer_pause(v->timer);
        else {
            lv_timer_set_period(v->timer, stage == XIAOZHI_VOICE_SPEAKING ? 100 :
                                stage == XIAOZHI_VOICE_THINKING ? 120 :
                                stage == XIAOZHI_VOICE_LISTENING ? 160 : 240);
            lv_timer_resume(v->timer);
        }
    }
    text(v->hint, stage == XIAOZHI_VOICE_LISTENING ? "我在听，请说吧" :
         stage == XIAOZHI_VOICE_THINKING || stage == XIAOZHI_VOICE_SPEAKING ?
         "轻点小球可打断" : stage == XIAOZHI_VOICE_UNAVAILABLE ?
         "请检查网络或稍后重试" : XIAOZHI_WAKE_HINT_TEXT);
}

void xiaozhi_voice_view_chat(xiaozhi_voice_view_t *v, const char *value)
{
    if (!v->root) return;
    text(v->subtitle, value);
    hidden(v->subtitle, v->activating || !value || !value[0]);
}

void xiaozhi_voice_view_activation(xiaozhi_voice_view_t *v, const char *code,
                                 const char *message, bool visible)
{
    if (!v->root) return;
    text(v->code, code);
    text(v->instructions, message);
    v->activating = visible;
    hidden(v->activation, !visible);
    hidden(v->hint, visible);
    hidden(v->subtitle, visible || !lv_label_get_text(v->subtitle)[0]);
}
