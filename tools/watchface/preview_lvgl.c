/* A real LVGL display, using exactly the same view code and assets as ESP32. */
#include "watch_face_view.h"
#include "watch_control_view.h"
#include "watch_launcher_view.h"
#include "watch_face_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int verify_voice(lv_display_t *display, void (*save_image)(const char *));

static unsigned char frame[410 * 502 * 2];
static unsigned char draw_buffer[410 * 502 * 2];
static unsigned char draw_buffer2[410 * 20 * 2];
static int partial_mode;
static unsigned flush_count,underlay_draw_count,motion_draw_tasks;
static void count_draw_task(lv_event_t *e) { (void)e; ++motion_draw_tasks; }
static void watch_control_tasks(lv_obj_t *obj) {
    lv_obj_add_flag(obj,LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(obj,count_draw_task,LV_EVENT_DRAW_TASK_ADDED,NULL);
    for(unsigned i=0;i<lv_obj_get_child_count(obj);++i)watch_control_tasks(lv_obj_get_child(obj,i));
}
static void count_underlay_draw(lv_event_t *e) { (void)e; ++underlay_draw_count; }
static void watch_underlay_tree(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj,count_underlay_draw,LV_EVENT_DRAW_MAIN,NULL);
    for(unsigned i=0;i<lv_obj_get_child_count(obj);++i)watch_underlay_tree(lv_obj_get_child(obj,i));
}

static void flush(lv_display_t *display, const lv_area_t *area, unsigned char *pixels)
{
    int w = lv_area_get_width(area);
    if(partial_mode && w*lv_area_get_height(area)>410*20)exit(72);
    ++flush_count;
    for (int y = area->y1; y <= area->y2; ++y)
        memcpy(frame + (y * 410 + area->x1) * 2,
               pixels + (y - area->y1) * w * 2, w * 2);
    lv_display_flush_ready(display);
}

static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) exit(3);
    fprintf(f, "P6\n410 502\n255\n");
    for (unsigned i = 0; i < sizeof(frame); i += 2) {
        unsigned v = frame[i] | ((unsigned)frame[i+1] << 8);
        fputc(((v >> 11) & 31) * 255 / 31, f);
        fputc(((v >> 5) & 63) * 255 / 63, f);
        fputc((v & 31) * 255 / 31, f);
    }
    fclose(f);
}


/* Compare the firmware decoder with independently composited Pillow frames.
 * Every source delay is checked across two loops, including the final frame. */
static unsigned frame_hash(lv_obj_t *gif)
{
    const lv_image_dsc_t *image = lv_image_get_src(gif);
    unsigned hash = 2166136261u;
    for (unsigned y = 0; y < image->header.h; ++y) {
        const unsigned char *row = image->data + y * image->header.stride;
        for (unsigned x = 0; x < image->header.w; ++x) {
            const unsigned char *pixel = row + x * 4;
            for (unsigned b = 0; b < 4; ++b)
                hash = (hash ^ (pixel[3] ? pixel[b] : 0)) * 16777619u;
        }
    }
    return hash;
}

static int verify_gifs(lv_obj_t *parent)
{
    FILE *expected = fopen("output/watchface/gif-expected.txt", "r");
    FILE *report = fopen("output/watchface/gif-runtime-verification.txt", "w");
    if (!expected || !report) return 20;
    unsigned total = 0;
    for (unsigned n = 1; n <= 11; ++n) {
        unsigned number, count, delay[100], hash[100];
        if (fscanf(expected, "%u %u", &number, &count) != 2 || number != n || count > 100) return 21;
        for (unsigned i = 0; i < count; ++i)
            if (fscanf(expected, "%u %u", &delay[i], &hash[i]) != 2) return 22;
        char path[100];
        sprintf(path, "spiffs/watchface/face%02u.gif", n);
        FILE *file = fopen(path, "rb");
        if (!file) return 23;
        fseek(file, 0, SEEK_END);
        long length = ftell(file);
        rewind(file);
        unsigned char *data = malloc(length);
        if (!data || fread(data, 1, length, file) != (size_t)length) return 24;
        fclose(file);
        lv_image_dsc_t source = {0};
        source.header.magic = LV_IMAGE_HEADER_MAGIC;
        source.header.cf = LV_COLOR_FORMAT_RAW;
        source.data_size = length;
        source.data = data;
        lv_obj_t *gif = lv_gif_create(parent);
        lv_gif_set_src(gif, &source);
        lv_gif_set_loop_count(gif, 0);
        if (!lv_gif_is_loaded(gif)) return 25;
        for (unsigned loop = 0; loop < 2; ++loop) {
            for (unsigned i = 0; i < count; ++i) {
                unsigned actual = frame_hash(gif);
                if (actual != hash[i]) {
                    fprintf(report,"FAIL gif=%u loop=%u frame=%u expected=%u actual=%u tick=%u\n",
                            n,loop,i,hash[i],actual,(unsigned)lv_tick_get());
                    fclose(report); return 26;
                }
                /* The frame must remain intact until its full source delay. */
                for (unsigned ms = 1; ms < delay[i]; ++ms) {
                    lv_tick_inc(1);
                    lv_timer_handler();
                }
                if (frame_hash(gif) != hash[i]) {
                    fprintf(report,"FAIL early advance gif=%u loop=%u frame=%u\n",n,loop,i);
                    fclose(report); return 27;
                }
                lv_tick_inc(1);
                lv_timer_handler();
                ++total;
            }
        }
        /* Simulate a busy UI waking at uneven intervals. Deadline scheduling
         * must still agree with wall-clock playback, without cumulative drift. */
        unsigned cycle_ms = 0;
        for (unsigned i = 0; i < count; ++i) cycle_ms += delay[i];
        unsigned elapsed = 0, expected_index = 0, boundary = delay[0], step_index = 0;
        while (elapsed < cycle_ms) {
            unsigned step = 1 + (step_index++ * 7) % 9;
            if (step > cycle_ms - elapsed) step = cycle_ms - elapsed;
            elapsed += step;
            lv_tick_inc(step);
            lv_timer_handler();
            while (elapsed >= boundary && expected_index + 1 < count)
                boundary += delay[++expected_index];
            unsigned wanted = elapsed == cycle_ms ? 0 : expected_index;
            if (frame_hash(gif) != hash[wanted]) {
                fprintf(report,"FAIL drift gif=%u elapsed=%u expected_frame=%u\n",n,elapsed,wanted);
                fclose(report); return 28;
            }
        }
        /* A display taking 60-110ms must advance over multiple overdue 40ms
         * source frames rather than slowing the animation's wall-clock cycle. */
        elapsed = expected_index = step_index = 0;
        boundary = delay[0];
        const unsigned heavy_steps[] = {80, 110, 60, 95};
        while (elapsed < cycle_ms) {
            unsigned step = heavy_steps[step_index++ % 4];
            if (step > cycle_ms - elapsed) step = cycle_ms - elapsed;
            elapsed += step;
            lv_tick_inc(step);
            lv_timer_handler();
            while (elapsed >= boundary && expected_index + 1 < count)
                boundary += delay[++expected_index];
            unsigned wanted = elapsed == cycle_ms ? 0 : expected_index;
            if (frame_hash(gif) != hash[wanted]) {
                fprintf(report,"FAIL slow-refresh catch-up gif=%u elapsed=%u expected_frame=%u\n",n,elapsed,wanted);
                fclose(report); return 29;
            }
        }
        /* Simulate preparing the first frame with emission off, then sleeping
         * for a second. Resume must hold that frame for its full original delay. */
        lv_gif_pause(gif);
        lv_tick_inc(1000);
        lv_timer_handler();
        if(frame_hash(gif) != hash[0]) return 30;
        lv_gif_resume(gif);
        for (unsigned ms = 1; ms < delay[0]; ++ms) {
            lv_tick_inc(1);
            lv_timer_handler();
        }
        if(frame_hash(gif) != hash[0]) return 31;
        lv_tick_inc(1);
        lv_timer_handler();
        if(frame_hash(gif) != hash[1]) return 32;
        lv_obj_delete(gif);
        free(data);
        fprintf(report,"PASS gif=%02u frames=%u loops=2 composition+exact_delays; uneven_wakeups=PASS; slow_refresh_catchup=PASS; prepared_wake=PASS\n",n,count);
    }
    fprintf(report,"PASS total_frame_checks=%u (373 source frames, 2 loops)\n",total);
    fclose(expected); fclose(report);
    return 0;
}


static int control_action=-1,control_value=-1,control_calls=0;
static void control_event(void *context,watch_control_action_t action,int value)
{
    (void)context;control_action=action;control_value=value;++control_calls;
}
static lv_indev_state_t pointer_state=LV_INDEV_STATE_RELEASED;
static lv_point_t pointer_point={0,0};
static void pointer_read(lv_indev_t *indev,lv_indev_data_t *data)
{
    (void)indev;data->state=pointer_state;data->point=pointer_point;
}
static void control_tap(lv_indev_t *indev,int x,int y)
{
    pointer_point.x=x;pointer_point.y=y;
    pointer_state=LV_INDEV_STATE_PRESSED;lv_indev_read(indev);
    lv_tick_inc(30);
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(indev);
}
LV_IMAGE_DECLARE(img_app_musicplayer); LV_IMAGE_DECLARE(img_app_settings);
LV_IMAGE_DECLARE(img_app_gallery); LV_IMAGE_DECLARE(img_app_drawpanel);
LV_IMAGE_DECLARE(img_app_xiaozhi); LV_IMAGE_DECLARE(img_app_qmi8658ball);
LV_IMAGE_DECLARE(img_app_calculator); LV_IMAGE_DECLARE(img_app_specanalyzer);
LV_IMAGE_DECLARE(img_app_rtc); LV_IMAGE_DECLARE(img_app_recorder); LV_IMAGE_DECLARE(img_app_vedioplayer);
static int launcher_id=-1,launcher_calls;
static void launcher_event(void *context,int id) { (void)context;launcher_id=id;++launcher_calls; }
static int verify_launcher(lv_display_t *display)
{
    const void *icons[]={&img_app_qmi8658ball,&img_app_rtc,&img_app_calculator,
        &img_app_drawpanel,&img_app_specanalyzer,&img_app_musicplayer,&img_app_gallery,
        &img_app_vedioplayer,&img_app_recorder,&img_app_settings,&img_app_xiaozhi};
    watch_launcher_app_t apps[64],favorites[WATCH_LAUNCHER_FAVORITES];
    for(unsigned i=0;i<64;++i){apps[i].id=100+i;apps[i].icon=icons[i%11];}
    const unsigned favorite_indices[]={5,9,6,3,8,10};
    for(unsigned i=0;i<WATCH_LAUNCHER_FAVORITES;++i)favorites[i]=apps[favorite_indices[i]];
    watch_launcher_view_t v;
    if(!watch_launcher_view_create(lv_layer_sys(),&v,favorites,apps,11,launcher_event,NULL))return 90;
    if(lv_obj_get_child_count(v.home)!=WATCH_LAUNCHER_FAVORITES+2)return 91;
    if(!watch_control_view_capture(&v.backdrop,lv_screen_active()))return 92;
    watch_launcher_view_page(&v,false,true);lv_refr_now(display);
    save("output/watchface/launcher-favorites.ppm");
    lv_indev_t *pointer=lv_indev_create();lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer,pointer_read);lv_indev_set_mode(pointer,LV_INDEV_MODE_EVENT);
    for(unsigned i=0;i<WATCH_LAUNCHER_FAVORITES;++i) {
        lv_area_t a;lv_obj_get_coords(v.favorites[i],&a);
        control_tap(pointer,(a.x1+a.x2)/2,(a.y1+a.y2)/2);
        if(launcher_id!=favorites[i].id)return 93;
    }
    lv_area_t overview;lv_obj_get_coords(v.overview,&overview);
    /* Seven equal circles: ChatGPT in the center with six touch gaps. */
    lv_area_t ai;lv_obj_get_coords(v.favorites[5],&ai);
    if((ai.x1+ai.x2+1)/2!=205 || (ai.y1+ai.y2+1)/2!=251)return 121;
    for(unsigned i=0;i<WATCH_LAUNCHER_FAVORITES+1;++i) {
        lv_obj_t *obj=i<WATCH_LAUNCHER_FAVORITES?v.favorites[i]:v.overview;
        lv_area_t a;lv_obj_get_coords(obj,&a);
        if(lv_area_get_width(&a)<=90 || lv_area_get_width(&a)!=lv_area_get_width(&overview) ||
           lv_area_get_height(&a)!=lv_area_get_height(&overview))return 118;
        const lv_point_t corner={a.x1,a.y1};
        if(lv_obj_hit_test(obj,&corner))return 119;
        for(unsigned j=0;j<i;++j) {
            lv_area_t b;lv_obj_get_coords(v.favorites[j],&b);
            int dx=(a.x1+a.x2-b.x1-b.x2)/2,dy=(a.y1+a.y2-b.y1-b.y2)/2;
            const int gap=lv_area_get_width(&a)+20;
            if(dx*dx+dy*dy<gap*gap)return 120;
        }
    }
    control_tap(pointer,(overview.x1+overview.x2)/2,(overview.y1+overview.y2)/2);

    if(!v.all_apps||!lv_obj_has_flag(v.home,LV_OBJ_FLAG_HIDDEN))return 94;
    lv_refr_now(display);save("output/watchface/launcher-all-apps.ppm");
    if(frame[0]||frame[1])return 95;
    for(unsigned i=0;i<11;++i) {
        lv_area_t a;lv_obj_get_coords(v.cells[i],&a);
        if(a.x1<0||a.x2>=410||a.y1<0||a.y2>=502)return 96;
        control_tap(pointer,(a.x1+a.x2)/2,(a.y1+a.y2)/2);
        if(launcher_id!=apps[i].id)return 97;
        for(unsigned j=0;j<i;++j) {
            lv_area_t b;lv_obj_get_coords(v.cells[j],&b);
            int dx=(a.x1+a.x2-b.x1-b.x2)/2,dy=(a.y1+a.y2-b.y1-b.y2)/2;
            const int separation=(lv_obj_get_width(v.cells[i])+lv_obj_get_width(v.cells[j]))/2+6;
            if(dx*dx+dy*dy<separation*separation)return 98;
        }
    }
    watch_launcher_view_page(&v,false,false);lv_refr_now(display);
    const int before_swipe=launcher_calls;
    /* Swipes from the overview or an app never change pages or launch apps. */
    pointer_state=LV_INDEV_STATE_PRESSED;pointer_point=(lv_point_t){205,350};lv_indev_read(pointer);
    for(int y=325;y>=150;y-=25){pointer_point.y=y;lv_tick_inc(15);lv_indev_read(pointer);}
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer);
    if(v.all_apps || launcher_calls!=before_swipe || lv_obj_has_flag(v.home,LV_OBJ_FLAG_HIDDEN))return 110;
    lv_area_t swipe_icon;lv_obj_get_coords(v.favorites[4],&swipe_icon);
    const int swipe_x=(swipe_icon.x1+swipe_icon.x2)/2,swipe_y=(swipe_icon.y1+swipe_icon.y2)/2;
    pointer_state=LV_INDEV_STATE_PRESSED;pointer_point=(lv_point_t){swipe_x,swipe_y};lv_indev_read(pointer);
    for(int y=swipe_y-25;y>=swipe_y-200;y-=25){pointer_point.y=y;lv_tick_inc(15);lv_indev_read(pointer);}
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer);
    if(v.all_apps || launcher_calls!=before_swipe)return 109;
    control_tap(pointer,swipe_x,swipe_y);
    if(launcher_calls!=before_swipe+1 || launcher_id!=favorites[4].id)return 111;
    /* Tapping the bottom overview is the only touch route into all apps. */
    const int before_overview=launcher_calls;
    control_tap(pointer,(overview.x1+overview.x2)/2,(overview.y1+overview.y2)/2);
    lv_obj_update_layout(v.all);
    if(!v.all_apps || !lv_obj_has_flag(v.home,LV_OBJ_FLAG_HIDDEN) ||
       lv_obj_get_y(v.all)!=0 || lv_anim_get(v.all,(lv_anim_exec_xcb_t)lv_obj_set_y))return 117;
    int smallest=100,largest=0;
    for(unsigned i=0;i<11;++i) {
        const int size=lv_obj_get_width(v.cells[i]);smallest=LV_MIN(smallest,size);largest=LV_MAX(largest,size);
    }
    if(largest-smallest<30)return 112;
    /* Downward dragging at the top does not navigate away from all apps. */
    pointer_state=LV_INDEV_STATE_PRESSED;pointer_point=(lv_point_t){205,100};lv_indev_read(pointer);
    for(int y=125;y<=250;y+=25){pointer_point.y=y;lv_tick_inc(15);lv_indev_read(pointer);}
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer);
    lv_tick_inc(250);lv_timer_handler();lv_obj_update_layout(v.all);
    if(!v.all_apps || launcher_calls!=before_overview || lv_obj_get_y(v.all)!=0)return 114;
    /* The control center captures the visible menu, not the hidden main screen. */
    watch_control_view_t controls;
    if(!watch_control_view_create(lv_layer_sys(),&controls,control_event,NULL))return 100;
    watch_launcher_view_page(&v,false,false);lv_refr_now(display);
    unsigned char *saved=malloc(sizeof(frame));memcpy(saved,frame,sizeof(frame));
    if(!watch_control_view_capture(&controls,watch_launcher_view_surface(&v)))return 101;
    lv_obj_remove_flag(controls.root,LV_OBJ_FLAG_HIDDEN);watch_control_view_transition(&controls,1024);
    lv_refr_now(display);lv_obj_add_flag(controls.root,LV_OBJ_FLAG_HIDDEN);watch_control_view_release(&controls);
    lv_obj_delete(controls.root);lv_refr_now(display);
    if(memcmp(saved,frame,sizeof(frame)))return 102;
    free(saved);
    watch_launcher_view_hide(&v);
    if(lv_obj_get_style_opa_layered(lv_screen_active(),0)!=255)return 103;
    /* A simulated app screen must be visible after releasing the menu's hold;
     * returning to the menu reuses the same glass and preserves the page. */
    lv_obj_t *main_screen=lv_screen_active(),*app=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(app,lv_color_hex(0xff0000),0);lv_obj_set_style_bg_opa(app,LV_OPA_COVER,0);lv_screen_load(app);lv_refr_now(display);
    if(frame[0]!=0 || frame[1]!=0xf8)return 104;
    lv_screen_load(main_screen);watch_control_backdrop_hold(&v.backdrop,main_screen);
    watch_launcher_view_page(&v,true,false);lv_refr_now(display);
    if(lv_obj_get_style_opa_layered(app,0)!=255)return 105;
    lv_obj_delete(app);watch_launcher_view_hide(&v);watch_control_view_release(&v.backdrop);
    lv_obj_delete(v.home);lv_obj_delete(v.all);
    if(!watch_launcher_view_create(lv_layer_sys(),&v,favorites,apps,64,launcher_event,NULL))return 106;
    watch_launcher_view_page(&v,true,true);lv_obj_update_layout(v.all);
    for(unsigned i=0;i<64;++i)
        if(lv_obj_get_width(v.cells[i])<42 || lv_obj_get_width(v.cells[i])>92)return 116;
    const int before_scroll=launcher_calls;
    pointer_state=LV_INDEV_STATE_PRESSED;pointer_point=(lv_point_t){205,300};lv_indev_read(pointer);
    for(int y=275;y>=100;y-=25){pointer_point.y=y;lv_tick_inc(15);lv_indev_read(pointer);}
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer);lv_refr_now(display);
    if(lv_obj_get_scroll_y(v.all)<100 || launcher_calls!=before_scroll)return 115;
    lv_obj_scroll_to_y(v.all,10000,LV_ANIM_OFF);lv_obj_update_layout(v.all);lv_refr_now(display);
    save("output/watchface/launcher-scrolled.ppm");
    lv_area_t last;lv_obj_get_coords(v.cells[63],&last);
    if(last.y1<0||last.y2>=502||lv_obj_get_scroll_y(v.all)<=0)return 107;
    control_tap(pointer,(last.x1+last.x2)/2,(last.y1+last.y2)/2);
    if(launcher_id!=apps[63].id)return 108;
    watch_launcher_view_hide(&v);watch_control_view_release(&v.backdrop);
    lv_obj_delete(v.home);lv_obj_delete(v.all);lv_indev_delete(pointer);
    puts("LAUNCHER=PASS (overview tap opens instantly, swipes do not navigate, no accidental launch, 64-app pointer scrolling, nested control overlay, app-screen/return lifecycle)");
    return 0;
}
static void preview_control_step(void *context,int32_t progress)
{
    watch_control_view_transition((watch_control_view_t *)context,progress);
}
static void save_control_motion(lv_display_t *display,watch_control_view_t *v,bool opening)
{
    lv_anim_t animation;lv_anim_init(&animation);
    lv_anim_set_var(&animation,v);lv_anim_set_exec_cb(&animation,preview_control_step);
    lv_anim_set_values(&animation,opening?0:1024,opening?1024:0);
    lv_anim_set_duration(&animation,opening?300:240);
    lv_anim_set_path_cb(&animation,opening?lv_anim_path_ease_out:lv_anim_path_ease_in);
    lv_anim_start(&animation);
    for(unsigned n=0;n<13;++n) {
        if(n) {lv_tick_inc(30);lv_timer_handler();}
        lv_obj_update_layout(v->root);lv_refr_now(display);
        char path[120];sprintf(path,"output/watchface/control-%s-%02u.ppm",opening?"open":"close",n);
        save(path);
    }
}
static int verify_controls(lv_display_t *display)
{
    watch_control_view_t v;
    watch_underlay_tree(lv_screen_active());
    watch_underlay_tree(lv_layer_bottom());watch_underlay_tree(lv_layer_top());
    if(!watch_control_view_create(lv_layer_sys(),&v,control_event,NULL))return 40;
    if(!lv_obj_has_flag(v.root,LV_OBJ_FLAG_HIDDEN))return 41;
    lv_obj_remove_flag(v.root,LV_OBJ_FLAG_HIDDEN);
    if(!watch_control_view_capture(&v,lv_screen_active()))return 58;
    if(lv_obj_get_style_opa_layered(lv_screen_active(),0)!=LV_OPA_TRANSP ||
       lv_obj_get_style_opa_layered(lv_layer_top(),0)!=LV_OPA_TRANSP ||
       lv_obj_get_style_opa_layered(lv_layer_bottom(),0)!=LV_OPA_TRANSP)return 73;
    underlay_draw_count=0;
    watch_control_view_status(&v,true);
    watch_control_view_transition(&v,0);
    lv_obj_update_layout(v.root);lv_refr_now(display);
    for(unsigned y=0;y<502;++y) {
        if(memcmp(frame+y*410*2,v.sharp_buffer.data+y*v.sharp_buffer.header.stride,410*2))return 59;
    }
    save("output/watchface/control-underlay.ppm");
    /* Check the packed-channel compositor against independent channel math,
     * including both endpoints and RGB565 rounding at 31 intermediate levels. */
    lv_draw_buf_t *background_check=lv_draw_buf_create(410,502,LV_COLOR_FORMAT_RGB565,LV_STRIDE_AUTO);
    if(!background_check)return 80;
    for(unsigned alpha=0;alpha<=32;++alpha) {
        watch_control_view_transition(&v,alpha*32);
        if(lv_snapshot_take_to_draw_buf(v.background,LV_COLOR_FORMAT_RGB565,background_check)!=LV_RESULT_OK)return 81;
        const lv_draw_buf_t *visible=background_check;
        for(unsigned y=0;y<502;y+=7)for(unsigned x=0;x<410;x+=7) {
            unsigned a=((uint16_t *)(v.sharp_buffer.data+y*v.sharp_buffer.header.stride))[x];
            unsigned b=((uint16_t *)(v.blur_buffer.data+y*v.blur_buffer.header.stride))[x];
            unsigned r=(((a>>11)*(32-alpha)+(b>>11)*alpha)/32);
            unsigned g=((((a>>5)&63)*(32-alpha)+((b>>5)&63)*alpha)/32);
            unsigned blue=(((a&31)*(32-alpha)+(b&31)*alpha)/32);
            unsigned actual=((uint16_t *)(visible->data+y*visible->header.stride))[x];
            if(actual!=((r<<11)|(g<<5)|blue))return 79;
        }
    }
    lv_draw_buf_destroy(background_check);
    watch_control_view_transition(&v,1024);
    watch_control_view_tile(&v,WC_WIFI,true,"Wi-Fi 已连接");
    lv_label_set_text(v.battery,"87%");
    watch_control_view_tile(&v,WC_MUTE,true,"静音已开启");
    watch_control_view_tile(&v,WC_WRIST,true,"抬腕亮屏 开");
    watch_control_view_tile(&v,WC_HAPTIC,true,"振动 开");
    lv_obj_update_layout(v.root);
    for(unsigned i=0;i<WC_TILE_COUNT;++i) {
        if(lv_obj_get_x(v.tiles[i])<0||lv_obj_get_x(v.tiles[i])+lv_obj_get_width(v.tiles[i])>354)return 42;
        if(lv_obj_get_child_count(v.tiles[i])!=1)return 43;
    }
    watch_control_tasks(v.root);
    watch_control_view_transition(&v,512);lv_obj_update_layout(v.root);
    motion_draw_tasks=0;lv_refr_now(display);
    const unsigned cached_tasks=motion_draw_tasks;
    if(cached_tasks>12 || !v.motion_raster)return 84;
    /* Compare task count against ordinary live capsules at the same pose. */
    v.motion_raster=false;
    for(unsigned i=0;i<WC_TILE_COUNT;++i)lv_obj_remove_local_style_prop(v.tiles[i],LV_STYLE_OPA_LAYERED,0);
    lv_obj_invalidate(v.root);motion_draw_tasks=0;lv_refr_now(display);
    const unsigned live_tasks=motion_draw_tasks;
    if(live_tasks<=cached_tasks)return 85;
    printf("MOTION_TASKS=PASS (%u live vs %u cached LVGL tasks per frame, %s)\n",
           live_tasks,cached_tasks,partial_mode?"PARTIAL":"FULL");
    for(unsigned i=0;i<WC_TILE_COUNT;++i)lv_obj_set_style_opa_layered(v.tiles[i],LV_OPA_TRANSP,0);
    v.motion_raster=true;
    watch_control_view_transition(&v,1024);
    save_control_motion(display,&v,true);
    if(v.transition_progress!=1024)return 67;
    lv_refr_now(display);save("output/watchface/control-main.ppm");
    save_control_motion(display,&v,false);
    if(v.transition_progress!=0)return 68;
    watch_control_view_transition(&v,1024);
    lv_indev_t *pointer=lv_indev_create();
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer,pointer_read);
    lv_indev_set_mode(pointer,LV_INDEV_MODE_EVENT);
    control_tap(pointer,110,120);
    if(control_action!=WC_WIFI)return 44;
    control_tap(pointer,292,120);
    if(control_action!=WC_AIRPLANE)return 45;
    control_tap(pointer,110,250);
    if(control_action!=WC_BATTERY)return 60;
    lv_obj_scroll_to_y(v.list,10000,LV_ANIM_OFF);lv_obj_update_layout(v.root);
    lv_refr_now(display);save("output/watchface/control-bottom.ppm");
    if(lv_obj_get_scroll_y(v.list)<400)return 46;
    lv_area_t tile;lv_obj_get_coords(v.tiles[WC_SETTINGS],&tile);
    if(tile.y2>492)return 47;
    control_tap(pointer,(tile.x1+tile.x2)/2,(tile.y1+tile.y2)/2);
    if(control_action!=WC_SETTINGS)return 48;
    watch_control_view_detail(&v,WC_BRIGHTNESS,"屏幕亮度","65%","滑动调节\n亮度会保留到下次亮屏",65);
    lv_obj_update_layout(v.root);
    lv_refr_now(display);save("output/watchface/control-brightness.ppm");
    if(lv_slider_get_min_value(v.slider)!=10||lv_slider_get_value(v.slider)!=65)return 49;
    if(frame[(340*410+80)*2]==0&&frame[(340*410+80)*2+1]==0)return 56;
    control_tap(pointer,230,339);
    if(control_action!=WC_SLIDER||control_value<1000||control_value>1100)return 50;
    watch_control_view_detail(&v,WC_TIMER,"倒计时","选择时长","结束后屏幕与振动提醒",0);
    const unsigned required_glyphs[]={0x9009,0x62e9,0x65f6,0x957f};
    const lv_font_t *timer_font=lv_obj_get_style_text_font(v.detail_value,0);
    for(unsigned i=0;i<4;++i) {
        lv_font_glyph_dsc_t glyph;
        if(!lv_font_get_glyph_dsc(timer_font,&glyph,required_glyphs[i],0))return 57;
    }
    lv_refr_now(display);save("output/watchface/control-timer-select.ppm");
    watch_control_view_detail(&v,WC_TIMER,"倒计时","03:00","结束后屏幕与振动提醒",0);
    lv_obj_update_layout(v.root);lv_refr_now(display);save("output/watchface/control-timer.ppm");
    for(unsigned i=0;i<4;++i) {
        lv_obj_get_coords(v.choices[i],&tile);
        control_tap(pointer,(tile.x1+tile.x2)/2,(tile.y1+tile.y2)/2);
        if(control_action!=(int)WC_TIMER_1+(int)i)return 51;
    }
    watch_control_view_detail(&v,-1,"","","",0);
    watch_control_view_torch(&v,1);lv_refr_now(display);
    if(frame[0]!=255||frame[1]!=255)return 52;
    control_tap(pointer,200,200);if(control_action!=WC_TORCH_NEXT)return 53;
    watch_control_view_torch(&v,2);lv_refr_now(display);save("output/watchface/control-red-light.ppm");
    if((frame[1]&0xf8)!=0xf8)return 54;
    watch_control_view_torch(&v,0);watch_control_view_lock(&v,true);
    lv_refr_now(display);save("output/watchface/control-lock.ppm");
    int before=control_calls;
    control_tap(pointer,100,120);
    if(control_calls!=before)return 55;
    watch_control_view_lock(&v,false);
    watch_control_view_detail(&v,WC_BATTERY,"电池电量","--%","当前未检测到电池\nUSB 供电不等于电池满电",0);
    lv_refr_now(display);save("output/watchface/control-battery.ppm");
    watch_control_view_detail(&v,-1,"","","",0);
    lv_obj_scroll_to_y(v.list,0,LV_ANIM_OFF);
    watch_control_view_transition(&v,512);
    lv_obj_update_layout(v.root);
    const int first_dy=lv_obj_get_y(v.tiles[WC_WIFI])-82;
    const int second_dy=lv_obj_get_y(v.tiles[WC_BATTERY])-208;
    const int third_dy=lv_obj_get_y(v.tiles[WC_TORCH])-334;
    if(!(first_dy<0 && second_dy>0 && third_dy>second_dy))return 61;
    if(lv_obj_get_x(v.tiles[WC_WIFI])>=28||lv_obj_get_x(v.tiles[WC_AIRPLANE])<=210)return 62;
    if(lv_obj_get_y(v.root)!=0)return 63;
    watch_control_view_transition(&v,1024);
    lv_obj_update_layout(v.root);
    if(lv_obj_get_y(v.tiles[WC_TORCH])!=252||lv_obj_get_x(v.tiles[WC_WIFI])!=0)return 64;
    /* Reverse a closing transition midway through; settled parents and the
     * scroll position must be restored with no orphaned moving capsules. */
    watch_control_view_transition(&v,512);
    lv_anim_t reverse;lv_anim_init(&reverse);
    lv_anim_set_var(&reverse,&v);lv_anim_set_exec_cb(&reverse,preview_control_step);
    lv_anim_set_values(&reverse,512,0);lv_anim_set_duration(&reverse,240);
    lv_anim_start(&reverse);
    for(unsigned i=0;i<3;++i){lv_tick_inc(30);lv_timer_handler();}
    const int halfway=v.transition_progress;
    if(halfway<=0||halfway>=512)return 70;
    lv_anim_delete(&v,preview_control_step);
    lv_anim_set_values(&reverse,halfway,1024);lv_anim_set_duration(&reverse,180);
    lv_anim_start(&reverse);
    for(unsigned i=0;i<8;++i){lv_tick_inc(30);lv_timer_handler();}
    if(v.transition_progress!=1024||v.motion_detached||lv_obj_get_parent(v.tiles[WC_WIFI])!=v.list)return 71;
    watch_control_view_release(&v);
    if(v.sharp_pixels||v.blur_pixels)return 65;
    for(unsigned i=0;i<WC_TILE_COUNT;++i)if(v.sprite_pixels[i])return 82;
    if(underlay_draw_count)return 74;
    if(lv_obj_get_style_opa_layered(lv_screen_active(),0)!=LV_OPA_COVER ||
       lv_obj_get_style_opa_layered(lv_layer_top(),0)!=LV_OPA_COVER ||
       lv_obj_get_style_opa_layered(lv_layer_bottom(),0)!=LV_OPA_COVER)return 75;
    lv_mem_monitor_t memory_before,memory_after;
    lv_mem_monitor(&memory_before);
    for(unsigned pass=0;pass<12;++pass) {
        if(!watch_control_view_capture(&v,lv_screen_active()))return 66;
        watch_control_view_transition(&v,512);
        if(!v.motion_raster)return 83;
        watch_control_view_transition(&v,1024);
        watch_control_view_release(&v);
    }
    lv_mem_monitor(&memory_after);
    if(memory_after.free_size+1024<memory_before.free_size)return 69;
    /* Preserve a pre-existing local screen opacity, too. */
    lv_obj_set_style_opa_layered(lv_screen_active(),123,0);
    lv_obj_set_style_opa_layered(lv_layer_top(),77,0);
    if(!watch_control_view_capture(&v,lv_screen_active()))return 76;
    watch_control_view_release(&v);
    if(lv_obj_get_style_opa_layered(lv_screen_active(),0)!=123 || lv_obj_get_style_opa_layered(lv_layer_top(),0)!=77)return 77;
    lv_obj_remove_local_style_prop(lv_layer_top(),LV_STYLE_OPA_LAYERED,0);
    lv_obj_remove_local_style_prop(lv_screen_active(),LV_STYLE_OPA_LAYERED,0);
    lv_obj_delete(v.root);lv_indev_delete(pointer);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(display);
    if(!underlay_draw_count)return 78;
    printf("RENDER_PATH=PASS (%s, flushes=%u, no live-underlay draws while controls visible, underlay restored)\n",
           partial_mode?"PARTIAL double 20-line buffers":"FULL",flush_count);
    puts("GLASS_EFFECT=PASS (33 blend levels match independent channel math, exact sharp first frame, real snapshot blur, fixed backdrop, row-dependent convergence, 12 backdrop/sprite capture/release cycles, direct draw strips)");
    puts("CONTROL_CENTER=PASS (14 tiles, scroll, pointer dispatch, slider release, 4 timer choices, torch colors, touch lock)");
    return 0;
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    partial_mode=argc>1 && !strcmp(argv[1],"--partial");
    lv_init();
    lv_display_t *display = lv_display_create(410, 502);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,partial_mode?draw_buffer2:NULL,
                           partial_mode?sizeof(draw_buffer2):sizeof(draw_buffer),
                           partial_mode?LV_DISPLAY_RENDER_MODE_PARTIAL:LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_timer_pause(lv_display_get_refr_timer(display));
    int gif_result = verify_gifs(screen);
    if (gif_result) return gif_result;
    lv_timer_resume(lv_display_get_refr_timer(display));
    watch_face_view_t v;
    if (!watch_face_view_create(screen, &v)) return 4;
    lv_label_set_text(v.time, "08:00");
    lv_label_set_text(v.date, "06-09");
    lv_label_set_text(v.weekday, "星期二");
    lv_label_set_text(v.temperature, "23°");
    lv_obj_set_width(v.battery_fill, 79);
    unsigned gifs[] = {11,7,8,3};
    FILE *metrics = fopen("output/watchface/lvgl-layout.txt", "w");
    for (unsigned scene = 0; scene < 4; ++scene) {
        watch_face_view_set_scene(&v, scene);
        char path[100];
        sprintf(path,"A:/spiffs/watchface/face%02u.gif",gifs[scene]);
        lv_obj_t *gif = lv_gif_create(v.root);
        lv_gif_set_src(gif,path);
        watch_face_view_place_animation(gif, gifs[scene]);
        if (lv_obj_get_child(v.root, -1) != gif) return 7;
        lv_timer_handler();
        lv_obj_update_layout(v.root);
        lv_refr_now(display);
        sprintf(path,"output/watchface/preview-%u.ppm",scene+1);
        save(path);
        for (unsigned i=0;i<8;++i) {
            lv_obj_t *p=v.phrases[i];
            if (lv_obj_has_flag(p,LV_OBJ_FLAG_HIDDEN)) continue;
            int x=lv_obj_get_x(p),y=lv_obj_get_y(p),w=lv_obj_get_width(p),h=lv_obj_get_height(p);
            fprintf(metrics,"scene=%u slot=%u x=%d y=%d w=%d h=%d\n",scene,i,x,y,w,h);
            if(x<0 || x+w>410 || y<178 || y+h>450) { fclose(metrics); return 5; }
        }
        lv_obj_delete(gif);
    }
    fclose(metrics);
    const char *clocks[] = {"00:00", "10:28", "11:11", "23:59"};
    for (unsigned i=0; i<4; ++i) {
        lv_label_set_text(v.time, clocks[i]);
        lv_obj_update_layout(v.root);
        if (lv_obj_get_x(v.time)<75 || lv_obj_get_x(v.time)+lv_obj_get_width(v.time)>330)
            return 6;
    }
    lv_label_set_text(v.time,"14:28");
    lv_obj_t *sample_gif=lv_gif_create(v.root);
    lv_gif_set_src(sample_gif,"A:/spiffs/watchface/face11.gif");
    lv_gif_pause(sample_gif);watch_face_view_place_animation(sample_gif,11);
    puts("Starting control render verification");
    int control_result=verify_controls(display);
    if(control_result)return control_result;
    int launcher_result=verify_launcher(display);
    lv_obj_delete(sample_gif);
    if(launcher_result)return launcher_result;
    int voice_result=verify_voice(display,save);
    if(voice_result)return voice_result;
    puts("LVGL_PREVIEW=PASS (4 scenes, captions inside screen)");
    return 0;
}
