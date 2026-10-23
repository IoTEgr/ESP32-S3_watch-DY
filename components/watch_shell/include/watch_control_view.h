#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC_WIFI, WC_BATTERY, WC_MUTE, WC_TORCH, WC_BRIGHTNESS, WC_VOLUME,
    WC_AIRPLANE, WC_WRIST, WC_HAPTIC, WC_KEEP_AWAKE, WC_LOCK, WC_TIMER,
    WC_SCREEN_OFF, WC_SETTINGS, WC_TILE_COUNT,
    WC_BACK, WC_CLOSE, WC_SLIDER, WC_TORCH_NEXT, WC_TIMER_1, WC_TIMER_3,
    WC_TIMER_5, WC_TIMER_CANCEL,
} watch_control_action_t;

typedef void (*watch_control_action_cb)(void *context, watch_control_action_t action, int value);
typedef struct {
    lv_obj_t *root, *connection, *list;
    lv_obj_t *background;
    lv_draw_buf_t sharp_buffer, blur_buffer;
    void *sharp_pixels, *blur_pixels;
    lv_obj_t *underlay;
    bool underlay_had_local_opa;
    lv_style_value_t underlay_local_opa;
    lv_obj_t *aux_layers[2];
    bool aux_had_local_opa[2];
    lv_style_value_t aux_local_opa[2];
    unsigned snapshot_ms, blur_ms;
    lv_draw_buf_t sprites[WC_TILE_COUNT];
    void *sprite_pixels[WC_TILE_COUNT];
    uint32_t sprite_keys[WC_TILE_COUNT];
    bool sprites_ready, motion_raster;
    unsigned sprite_prepare_ms;
    int motion_x[WC_TILE_COUNT], motion_y[WC_TILE_COUNT], tile_opa;
    int64_t background_us, sprites_us;
    unsigned raster_calls;
    int transition_progress;
    lv_obj_t *motion_layer;
    int motion_scroll_y;
    bool motion_detached;
    lv_obj_t *tiles[WC_TILE_COUNT], *battery;
    lv_obj_t *detail, *detail_title, *detail_value, *detail_body, *slider;
    lv_obj_t *choices[4], *choice_labels[4];
    lv_obj_t *torch, *torch_hint, *lock, *toast;
    watch_control_action_cb callback;
    void *context;
    int detail_kind;
} watch_control_view_t;

/* Lightweight shared glass surface, without control-center widgets. */
bool watch_control_backdrop_create(lv_obj_t *parent,watch_control_view_t *view);
void watch_control_backdrop_restore(watch_control_view_t *view);
void watch_control_backdrop_hold(watch_control_view_t *view,lv_obj_t *screen);
bool watch_control_view_create(lv_obj_t *parent, watch_control_view_t *view,
                               watch_control_action_cb callback, void *context);
void watch_control_view_tile(watch_control_view_t *view, unsigned action, bool active, const char *text);
void watch_control_view_status(watch_control_view_t *view, bool connected);
/* Two full RGB565 backdrops and cached capsule sprites live in PSRAM. The
 * compositor writes straight into the current LVGL draw buffer. No full-frame
 * intermediate image is produced on animation ticks. */
bool watch_control_view_capture(watch_control_view_t *view, lv_obj_t *screen);
void watch_control_view_release(watch_control_view_t *view);
bool watch_control_view_prepare_motion(watch_control_view_t *view);
/* Shared draw hooks; all calls stay on the LVGL GUI task. */
void watch_control_view_draw_background(lv_event_t *event);
void watch_control_view_draw_motion(lv_event_t *event);
/* 0 = dispersed/transparent, 1024 = settled/visible. Root never translates. */
void watch_control_view_transition(watch_control_view_t *view, int progress);
void watch_control_view_detail(watch_control_view_t *view, int kind, const char *title,
                               const char *value, const char *body, int slider_value);
void watch_control_view_torch(watch_control_view_t *view, unsigned mode);
void watch_control_view_lock(watch_control_view_t *view, bool locked);
void watch_control_view_toast(watch_control_view_t *view, const char *text);
#ifdef __cplusplus
}
#endif
