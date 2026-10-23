/* Shared firmware/host LVGL control-center view. No hardware calls here. */
#include "watch_control_view.h"
#include <string.h>

LV_FONT_DECLARE(font_control);
LV_FONT_DECLARE(font_control_small);
LV_FONT_DECLARE(font_control_large);
LV_IMAGE_DECLARE(cc_wifi_small); LV_IMAGE_DECLARE(cc_wifi); LV_IMAGE_DECLARE(cc_battery); LV_IMAGE_DECLARE(cc_mute);
LV_IMAGE_DECLARE(cc_torch); LV_IMAGE_DECLARE(cc_brightness); LV_IMAGE_DECLARE(cc_volume);
LV_IMAGE_DECLARE(cc_airplane); LV_IMAGE_DECLARE(cc_wrist); LV_IMAGE_DECLARE(cc_haptic);
LV_IMAGE_DECLARE(cc_awake); LV_IMAGE_DECLARE(cc_lock); LV_IMAGE_DECLARE(cc_timer);
LV_IMAGE_DECLARE(cc_power); LV_IMAGE_DECLARE(cc_settings);

static const lv_image_dsc_t *const icons[WC_TILE_COUNT] = {
    &cc_wifi,&cc_battery,&cc_mute,&cc_torch,&cc_brightness,&cc_volume,&cc_airplane,
    &cc_wrist,&cc_haptic,&cc_awake,&cc_lock,&cc_timer,&cc_power,&cc_settings
};
static uint32_t accent(unsigned n)
{
    if(n==WC_WIFI) return 0x087CFA;
    if(n==WC_MUTE) return 0xEF3158;
    if(n==WC_AIRPLANE) return 0xF59C12;
    if(n==WC_KEEP_AWAKE) return 0x8654E5;
    return 0x267F69;
}
static void text(lv_obj_t *o, const char *s)
{
    if(strcmp(lv_label_get_text(o),s)) lv_label_set_text(o,s);
}
static void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(o,0,0);
}
static lv_obj_t *label(lv_obj_t *p,const char *s,const lv_font_t *f,uint32_t color)
{
    lv_obj_t *o=lv_label_create(p);plain(o);
    lv_obj_set_style_text_font(o,f,0);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text(o,s);
    return o;
}
static void click(lv_event_t *e)
{
    watch_control_view_t *v=lv_event_get_user_data(e);
    lv_obj_t *o=lv_event_get_current_target(e);
    int action=(int)(intptr_t)lv_obj_get_user_data(o);
    int value=action==WC_SLIDER?lv_slider_get_value(o):0;
    if(action==WC_SLIDER && lv_event_get_code(e)==LV_EVENT_RELEASED) value+=1000;
    if(v->transition_progress < 1024) return;
    if(v->callback) v->callback(v->context,(watch_control_action_t)action,value);
}
static lv_obj_t *button(lv_obj_t *p,int x,int y,int w,int h,int radius,
                       int action,watch_control_view_t *v)
{
    lv_obj_t *o=lv_button_create(p);plain(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_radius(o,radius,0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x25262B),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x515259),LV_STATE_PRESSED);
    lv_obj_set_user_data(o,(void *)(intptr_t)action);
    lv_obj_add_event_cb(o,click,LV_EVENT_CLICKED,v);
    return o;
}
static void show(lv_obj_t *o,bool yes)
{
    if(yes) lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
}

bool watch_control_backdrop_create(lv_obj_t *parent,watch_control_view_t *v)
{
    if(!parent||!v)return false;
    memset(v,0,sizeof(*v));v->detail_kind=-1;v->transition_progress=1024;
    v->root=lv_obj_create(parent);plain(v->root);
    lv_obj_set_size(v->root,410,502);lv_obj_set_pos(v->root,0,0);
    lv_obj_set_style_bg_color(v->root,lv_color_black(),0);
    lv_obj_set_style_bg_opa(v->root,LV_OPA_COVER,0);
    lv_obj_add_flag(v->root,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
    v->background=lv_obj_create(v->root);plain(v->background);
    lv_obj_set_size(v->background,410,502);
    lv_obj_add_event_cb(v->background,watch_control_view_draw_background,LV_EVENT_DRAW_MAIN,v);
    lv_obj_remove_flag(v->background,LV_OBJ_FLAG_CLICKABLE);
    show(v->background,false);
    return true;
}
bool watch_control_view_create(lv_obj_t *parent,watch_control_view_t *v,
                               watch_control_action_cb callback,void *context)
{
    if(!watch_control_backdrop_create(parent,v))return false;
    v->callback=callback;v->context=context;
    v->connection=lv_obj_create(v->root);plain(v->connection);
    lv_obj_set_size(v->connection,74,28);lv_obj_set_pos(v->connection,168,25);
    lv_obj_set_style_radius(v->connection,14,0);
    lv_obj_set_style_bg_color(v->connection,lv_color_hex(0xDAE2F4),0);
    lv_obj_set_style_bg_opa(v->connection,LV_OPA_20,0);
    lv_obj_set_style_border_width(v->connection,1,0);
    lv_obj_set_style_border_color(v->connection,lv_color_white(),0);
    lv_obj_set_style_border_opa(v->connection,LV_OPA_20,0);
    lv_obj_t *signal=lv_image_create(v->connection);
    lv_image_set_src(signal,&cc_wifi_small);
    lv_obj_center(signal);
    lv_obj_set_style_image_recolor_opa(signal,LV_OPA_COVER,0);
    lv_obj_set_style_image_recolor(signal,lv_color_hex(0x76F2AF),0);
    v->list=lv_obj_create(v->root);plain(v->list);
    lv_obj_set_pos(v->list,28,82);lv_obj_set_size(v->list,354,410);
    lv_obj_add_flag(v->list,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v->list,LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(v->list,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->list,LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(v->list,lv_color_hex(0xC1CEDF),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(v->list,LV_OPA_40,LV_PART_SCROLLBAR);
    lv_obj_set_style_width(v->list,2,LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(v->list,-10,LV_PART_SCROLLBAR);
    const unsigned order[WC_TILE_COUNT]={
        WC_WIFI,WC_AIRPLANE,WC_BATTERY,WC_HAPTIC,WC_TORCH,WC_MUTE,
        WC_BRIGHTNESS,WC_VOLUME,WC_WRIST,WC_KEEP_AWAKE,WC_LOCK,WC_TIMER,
        WC_SCREEN_OFF,WC_SETTINGS
    };
    for(unsigned slot=0;slot<WC_TILE_COUNT;++slot) {
        const unsigned i=order[slot];
        lv_obj_t *b=button(v->list,(slot%2)*182,(slot/2)*126,172,112,56,i,v);
        v->tiles[i]=b;
        lv_obj_set_style_bg_color(b,lv_color_hex(0xD1DCF3),0);
        lv_obj_set_style_bg_opa(b,42,0);
        lv_obj_set_style_bg_opa(b,90,LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b,1,0);
        lv_obj_set_style_border_color(b,lv_color_hex(0xE1EBFF),0);
        lv_obj_set_style_border_opa(b,45,0);
        if(i==WC_BATTERY) {
            v->battery=label(b,"--%",&font_control_large,0xFFFFFF);
            lv_obj_center(v->battery);
        } else {
            lv_obj_t *image=lv_image_create(b);lv_image_set_src(image,icons[i]);
            lv_obj_set_style_image_recolor(image,lv_color_white(),0);
            lv_obj_set_style_image_recolor_opa(image,LV_OPA_COVER,0);
            lv_obj_center(image);
        }
    }
    v->transition_progress=1024;
    v->motion_layer=lv_obj_create(v->root);plain(v->motion_layer);
    lv_obj_set_size(v->motion_layer,410,502);
    lv_obj_add_event_cb(v->motion_layer,watch_control_view_draw_motion,LV_EVENT_DRAW_MAIN,v);
    lv_obj_remove_flag(v->motion_layer,LV_OBJ_FLAG_CLICKABLE);
    v->detail=lv_obj_create(v->root);plain(v->detail);
    lv_obj_set_size(v->detail,410,502);
    lv_obj_set_style_bg_color(v->detail,lv_color_black(),0);
    lv_obj_set_style_bg_opa(v->detail,LV_OPA_COVER,0);
    lv_obj_add_flag(v->detail,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *back=button(v->detail,28,20,65,46,23,WC_BACK,v);
    lv_obj_t *back_text=label(back,"‹",&font_control_large,0xFFFFFF);lv_obj_center(back_text);
    v->detail_title=label(v->detail,"",&font_control,0xD9D9DE);
    lv_obj_align(v->detail_title,LV_ALIGN_TOP_MID,0,90);
    v->detail_value=label(v->detail,"",&font_control_large,0xFFFFFF);
    lv_obj_align(v->detail_value,LV_ALIGN_TOP_MID,0,146);
    v->detail_body=label(v->detail,"",&font_control_small,0xA8A8B1);
    lv_obj_set_width(v->detail_body,338);
    lv_obj_align(v->detail_body,LV_ALIGN_TOP_MID,0,230);
    v->slider=lv_slider_create(v->detail);
    lv_obj_set_size(v->slider,290,32);lv_obj_set_pos(v->slider,60,325);
    lv_slider_set_range(v->slider,0,100);
    lv_obj_set_style_radius(v->slider,16,LV_PART_MAIN);
    lv_obj_set_style_radius(v->slider,16,LV_PART_INDICATOR);
    lv_obj_set_style_radius(v->slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
    lv_obj_set_style_bg_opa(v->slider,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_bg_opa(v->slider,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(v->slider,LV_OPA_COVER,LV_PART_KNOB);
    lv_obj_set_style_bg_color(v->slider,lv_color_hex(0x303138),LV_PART_MAIN);
    lv_obj_set_style_bg_color(v->slider,lv_color_hex(0xD8E4FF),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(v->slider,lv_color_white(),LV_PART_KNOB);
    lv_obj_set_style_pad_all(v->slider,5,LV_PART_KNOB);
    lv_obj_set_user_data(v->slider,(void *)(intptr_t)WC_SLIDER);
    lv_obj_add_event_cb(v->slider,click,LV_EVENT_RELEASED,v);
    lv_obj_add_event_cb(v->slider,click,LV_EVENT_VALUE_CHANGED,v);
    for(unsigned i=0;i<4;++i) {
        v->choices[i]=button(v->detail,30+(i%3)*120,318+(i/3)*80,
                            i==3?350:110,64,25,WC_TIMER_1+i,v);
        v->choice_labels[i]=label(v->choices[i],i==0?"1 分钟":i==1?"3 分钟":i==2?"5 分钟":"取消倒计时",
                                 &font_control_small,0xFFFFFF);
        lv_obj_center(v->choice_labels[i]);
    }
    show(v->detail,false);
    v->torch=lv_obj_create(v->root);plain(v->torch);
    lv_obj_set_size(v->torch,410,502);lv_obj_set_style_bg_opa(v->torch,LV_OPA_COVER,0);
    lv_obj_add_flag(v->torch,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(v->torch,(void *)(intptr_t)WC_TORCH_NEXT);
    lv_obj_add_event_cb(v->torch,click,LV_EVENT_CLICKED,v);
    v->torch_hint=label(v->torch,"轻点切换白光 / 红光\n按 PWR 返回",&font_control_small,0x5A5A5A);
    lv_obj_align(v->torch_hint,LV_ALIGN_BOTTOM_MID,0,-50);show(v->torch,false);
    v->lock=lv_obj_create(v->root);plain(v->lock);lv_obj_set_size(v->lock,410,502);
    lv_obj_set_style_bg_color(v->lock,lv_color_black(),0);
    lv_obj_set_style_bg_opa(v->lock,LV_OPA_COVER,0);lv_obj_add_flag(v->lock,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *lock_icon=lv_image_create(v->lock);lv_image_set_src(lock_icon,&cc_lock);
    lv_obj_align(lock_icon,LV_ALIGN_CENTER,0,-50);
    lv_obj_t *lock_text=label(v->lock,"触摸已锁定\n按 PWR 解锁",&font_control,0xB8C6E9);
    lv_obj_align(lock_text,LV_ALIGN_CENTER,0,30);show(v->lock,false);
    v->toast=label(v->root,"",&font_control_small,0xFFFFFF);
    lv_obj_set_style_bg_color(v->toast,lv_color_hex(0x41424B),0);
    lv_obj_set_style_bg_opa(v->toast,LV_OPA_COVER,0);
    lv_obj_set_style_radius(v->toast,20,0);
    lv_obj_set_style_pad_all(v->toast,14,0);lv_obj_set_width(v->toast,340);
    lv_obj_align(v->toast,LV_ALIGN_BOTTOM_MID,0,-25);show(v->toast,false);
    show(v->root,false);
    return true;
}
void watch_control_view_tile(watch_control_view_t *v,unsigned action,bool active,const char *s)
{
    if(action>=WC_TILE_COUNT)return;
    (void)s;
    lv_obj_set_style_bg_color(v->tiles[action],lv_color_hex(active?accent(action):0xD1DCF3),0);
    lv_obj_set_style_bg_opa(v->tiles[action],active?235:42,0);
    lv_obj_set_style_border_opa(v->tiles[action],active?70:45,0);
}
void watch_control_view_status(watch_control_view_t *v,bool connected)
{
    lv_obj_t *signal=lv_obj_get_child(v->connection,0);
    lv_obj_set_style_image_recolor(signal,lv_color_hex(connected?0x76F2AF:0xBEC8D8),0);
}
void watch_control_view_detail(watch_control_view_t *v,int kind,const char *title,
                               const char *value,const char *body,int slider_value)
{
    v->detail_kind=kind;show(v->detail,kind>=0);
    if(kind<0)return;
    text(v->detail_title,title);text(v->detail_value,value);text(v->detail_body,body);
    lv_obj_align(v->detail_title,LV_ALIGN_TOP_MID,0,90);
    lv_obj_align(v->detail_value,LV_ALIGN_TOP_MID,0,146);
    show(v->slider,kind==WC_BRIGHTNESS||kind==WC_VOLUME);
    if(kind==WC_BRIGHTNESS)lv_slider_set_range(v->slider,10,100);
    else lv_slider_set_range(v->slider,0,100);
    if(!lv_obj_has_state(v->slider,LV_STATE_PRESSED))
        lv_slider_set_value(v->slider,slider_value,LV_ANIM_OFF);
    for(unsigned i=0;i<4;++i)show(v->choices[i],kind==WC_TIMER);
}
void watch_control_view_torch(watch_control_view_t *v,unsigned mode)
{
    show(v->torch,mode!=0);
    lv_obj_set_style_bg_color(v->torch,lv_color_hex(mode==2?0xFF180C:0xFFFFFF),0);
    lv_obj_set_style_text_color(v->torch_hint,lv_color_hex(mode==2?0x700000:0x5A5A5A),0);
}
void watch_control_view_lock(watch_control_view_t *v,bool locked){show(v->lock,locked);}
void watch_control_view_toast(watch_control_view_t *v,const char *s)
{
    if(s&&*s){text(v->toast,s);show(v->toast,true);lv_obj_move_to_index(v->toast,-1);}
    else show(v->toast,false);
}
