#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { WATCH_FACE_PHRASES = 8, WATCH_FACE_SCENES = 4 };

typedef struct {
    lv_obj_t *root, *background;
    lv_obj_t *time, *date, *weekday, *temperature, *weather;
    lv_obj_t *battery_track, *battery_fill;
    lv_obj_t *phrases[WATCH_FACE_PHRASES];
} watch_face_view_t;

/* Shared by the ESP32 firmware and the off-screen LVGL preview. */
bool watch_face_view_create(lv_obj_t *parent, watch_face_view_t *view);
void watch_face_view_set_scene(watch_face_view_t *view, unsigned scene);
void watch_face_view_place_animation(lv_obj_t *animation, unsigned gif_number);
unsigned watch_face_view_scene_for_gif(unsigned gif_number);

#ifdef __cplusplus
}
#endif
