#pragma once
#include "watch_control_view.h"
#ifdef __cplusplus
extern "C" {
#endif
#define WATCH_LAUNCHER_MAX_APPS 64
#define WATCH_LAUNCHER_FAVORITES 6
typedef struct { int id; const void *icon; } watch_launcher_app_t;
typedef void (*watch_launcher_launch_cb)(void *context,int app_id);
typedef struct {
    watch_control_view_t backdrop;
    lv_obj_t *home,*all,*favorites[WATCH_LAUNCHER_FAVORITES],*overview,*cells[WATCH_LAUNCHER_MAX_APPS];
    int16_t centers[WATCH_LAUNCHER_MAX_APPS][2];
    lv_point_t press;
    bool dragging;
    unsigned app_count;
    bool all_apps;
    watch_launcher_launch_cb launch;
    void *context;
} watch_launcher_view_t;
bool watch_launcher_view_create(lv_obj_t *parent,watch_launcher_view_t *view,
    const watch_launcher_app_t favorites[WATCH_LAUNCHER_FAVORITES],const watch_launcher_app_t *apps,unsigned count,
    watch_launcher_launch_cb launch,void *context);
void watch_launcher_view_page(watch_launcher_view_t *view,bool all_apps,bool reset_scroll);
void watch_launcher_view_hide(watch_launcher_view_t *view);
lv_obj_t *watch_launcher_view_surface(watch_launcher_view_t *view);
#ifdef __cplusplus
}
#endif
