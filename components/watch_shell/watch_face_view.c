#include "watch_face_view.h"
#include "watch_face_layout.h"
#include "watch_face_animations.h"

LV_IMAGE_DECLARE(face_bg);
LV_IMAGE_DECLARE(wx_yin);
LV_FONT_DECLARE(font_clock);
LV_FONT_DECLARE(font_meta);
LV_FONT_DECLARE(font_phrase);
LV_FONT_DECLARE(font_phrase_large);
LV_FONT_DECLARE(font_phrase_small);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    lv_obj_set_style_text_line_space(obj, 0, 0);
    lv_obj_set_style_text_outline_stroke_width(obj, 0, 0);
    lv_label_set_text(obj, text);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

bool watch_face_view_create(lv_obj_t *parent, watch_face_view_t *v)
{
    if (!parent || !v) return false;
    *v = (watch_face_view_t){0};
    v->root = lv_obj_create(parent);
    if (!v->root) return false;
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, FACE_WIDTH, FACE_HEIGHT);
    lv_obj_set_pos(v->root, 0, 0);
    lv_obj_set_style_bg_color(v->root, lv_color_hex(FACE_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->root, LV_OBJ_FLAG_CLICKABLE);

    v->background = lv_image_create(v->root);
    lv_image_set_src(v->background, &face_bg);
    lv_obj_set_pos(v->background, 0, 0);
    lv_obj_remove_flag(v->background, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    v->date = label(v->root, &font_meta, FACE_COLOR_META, "-- --");
    v->weekday = label(v->root, &font_meta, FACE_COLOR_META, "星期-");
    v->time = label(v->root, &font_clock, FACE_COLOR_TIME, "--:--");
    v->temperature = label(v->root, &font_meta, FACE_COLOR_META, "--°");
    if (!v->date || !v->weekday || !v->time || !v->temperature) return false;
    lv_obj_set_style_text_letter_space(v->date, FACE_DATE_LETTER_SPACE, 0);
    lv_obj_align(v->date, LV_ALIGN_TOP_LEFT, FACE_DATE_X, FACE_DATE_Y);
    lv_obj_align(v->weekday, LV_ALIGN_TOP_RIGHT, FACE_WEEKDAY_RIGHT - FACE_WIDTH, FACE_WEEKDAY_Y);
    lv_obj_set_style_text_align(v->time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->time, LV_ALIGN_TOP_MID, FACE_TIME_X, FACE_TIME_Y);
    lv_obj_set_pos(v->temperature, FACE_TEMPERATURE_X, FACE_TEMPERATURE_Y);

    v->weather = lv_image_create(v->root);
    lv_image_set_src(v->weather, &wx_yin);
    lv_obj_set_pos(v->weather, FACE_WEATHER_X, FACE_WEATHER_Y);
    lv_obj_remove_flag(v->weather, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    v->battery_track = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->battery_track);
    lv_obj_set_pos(v->battery_track, FACE_BATTERY_X, FACE_BATTERY_Y);
    lv_obj_set_size(v->battery_track, FACE_BATTERY_W, FACE_BATTERY_H);
    lv_obj_set_style_radius(v->battery_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(v->battery_track, lv_color_hex(FACE_COLOR_TRACK), 0);
    lv_obj_set_style_bg_opa(v->battery_track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v->battery_track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    v->battery_fill = lv_obj_create(v->battery_track);
    lv_obj_remove_style_all(v->battery_fill);
    lv_obj_set_pos(v->battery_fill, 0, 0);
    lv_obj_set_size(v->battery_fill, FACE_BATTERY_W, FACE_BATTERY_H);
    lv_obj_set_style_radius(v->battery_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(v->battery_fill, lv_color_hex(FACE_COLOR_FILL_A), 0);
    lv_obj_set_style_bg_grad_color(v->battery_fill, lv_color_hex(FACE_COLOR_FILL_B), 0);
    lv_obj_set_style_bg_grad_dir(v->battery_fill, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(v->battery_fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v->battery_fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < WATCH_FACE_PHRASES; ++i) {
        v->phrases[i] = label(v->root, &font_phrase, FACE_COLOR_PHRASE, "");
        if (!v->phrases[i]) return false;
        lv_obj_add_flag(v->phrases[i], LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

void watch_face_view_set_scene(watch_face_view_t *v, unsigned scene)
{
    if (!v) return;
    scene %= WATCH_FACE_SCENES;
    for (unsigned i = 0; i < WATCH_FACE_PHRASES; ++i) {
        const face_phrase_t *p = &FACE_SCENES[scene].phrases[i];
        lv_obj_t *obj = v->phrases[i];
        if (!obj) continue;
        if (!p->text) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); continue; }
        const lv_font_t *font = p->size == 2 ? &font_phrase_large :
                                p->size == 0 ? &font_phrase_small : &font_phrase;
        lv_obj_set_style_text_font(obj, font, 0);
        lv_label_set_text(obj, p->text);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(obj);
        /* x is always a positive screen coordinate, never a signed inset. */
        int x = p->right ? p->x - lv_obj_get_width(obj) : p->x;
        if (x < 4) x = 4;
        if (x + lv_obj_get_width(obj) > FACE_WIDTH - 4)
            x = FACE_WIDTH - 4 - lv_obj_get_width(obj);
        lv_obj_set_pos(obj, x, p->y);
    }
}

void watch_face_view_place_animation(lv_obj_t *animation, unsigned gif_number)
{
    if (!animation) return;
    const face_animation_t *a = &FACE_ANIMATIONS[(gif_number - 1) % 11];
    lv_obj_update_layout(animation);
    lv_obj_align(animation, LV_ALIGN_CENTER,
                 FACE_ANIMATION_CENTER_X - FACE_WIDTH / 2 + a->dx,
                 FACE_ANIMATION_CENTER_Y - FACE_HEIGHT / 2 + a->dy);
    lv_image_set_scale(animation, a->scale);
    /* The opaque character covers captions; its transparent margin does not. */
    lv_obj_move_to_index(animation, -1);
}

unsigned watch_face_view_scene_for_gif(unsigned n)
{
    if (n == 7) return 1;
    if (n == 3) return 3;
    if (n == 11) return 0;
    return 2;
}
