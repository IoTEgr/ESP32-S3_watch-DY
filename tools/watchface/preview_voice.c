#include "xiaozhi_voice_view.h"
#include "xiaozhi_wake_config.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(voice_preview_font);
static int actions;
static void action(void *context) { ++*(int *)context; }
static bool basic_missing_name(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                               uint32_t cp, uint32_t next)
{
    if(cp==0x5609 || cp==0x73C8) return false;
    return lv_font_get_glyph_dsc_fmt_txt(font,out,cp,next);
}

int verify_voice(lv_display_t *display, void (*save_image)(const char *))
{
    lv_obj_t *screen=lv_screen_active();
    lv_obj_clean(screen);
    xiaozhi_voice_view_t v;
    if(!xiaozhi_voice_view_create(screen,&v,&voice_preview_font,action,&actions))return 90;
    /* Match the production common font: 嘉 exists, 珈 does not. Verify the
     * actual subtitle font chain, rather than just the small wake hint. */
    lv_font_glyph_dsc_t name_glyph;
    if(!lv_font_get_glyph_dsc(&voice_preview_font,&name_glyph,0x5609,0))return 103;
    if(lv_font_get_glyph_dsc(&voice_preview_font,&name_glyph,0x73C8,0))return 104;
    const lv_font_t *subtitle_font=lv_obj_get_style_text_font(v.subtitle,0);
    if(!lv_font_get_glyph_dsc(subtitle_font,&name_glyph,0x5609,0) || name_glyph.is_placeholder)return 105;
    if(name_glyph.resolved_font!=&v.conversation_font)return 106;
    if(!lv_font_get_glyph_dsc(subtitle_font,&name_glyph,0x73C8,0) || name_glyph.is_placeholder || !name_glyph.box_w)return 107;
    if(name_glyph.resolved_font!=&v.conversation_fallback)return 108;
    if(subtitle_font->line_height!=voice_preview_font.line_height ||
       subtitle_font->base_line!=voice_preview_font.base_line || voice_preview_font.fallback)return 109;
    if(strcmp(lv_label_get_text(v.hint),XIAOZHI_WAKE_HINT_TEXT))return 100;
    const lv_font_t *hint_font=lv_obj_get_style_text_font(v.hint,0);
    for(const char *p=XIAOZHI_WAKE_HINT_TEXT;*p;) {
        uint32_t cp=(unsigned char)*p++;
        if(cp>=0xE0 && cp<0xF0){cp=(cp&15)<<12;cp|=((unsigned char)*p++&63)<<6;cp|=(unsigned char)*p++&63;}
        else if(cp>=0xC0 && cp<0xE0){cp=(cp&31)<<6;cp|=(unsigned char)*p++&63;}
        lv_font_glyph_dsc_t glyph;
        if(!lv_font_get_glyph_dsc(hint_font,&glyph,cp,0))return 101;
    }
    xiaozhi_voice_view_status(&v,"待命",XIAOZHI_VOICE_IDLE);
    lv_refr_now(display);
    save_image("output/watchface/xiaozhi-idle.ppm");
    xiaozhi_voice_view_status(&v,"正在准备小嘉",XIAOZHI_VOICE_IDLE);
    lv_refr_now(display);save_image("output/watchface/xiaozhi-preparing.ppm");
    xiaozhi_voice_view_status(&v,"待命",XIAOZHI_VOICE_IDLE);
    if(strcmp(lv_label_get_text(v.hint),XIAOZHI_WAKE_HINT_TEXT))return 102;
    if(lv_color_to_u32(lv_obj_get_style_bg_color(v.root,0))!=lv_color_to_u32(lv_color_black()))return 91;
    lv_obj_update_layout(v.root);
    if(lv_obj_get_y(v.orb)+lv_obj_get_height(v.orb)>=lv_obj_get_y(v.status))return 92;
    lv_obj_send_event(v.orb,LV_EVENT_CLICKED,NULL);
    if(actions!=1)return 93;

    const lv_image_dsc_t *first=lv_image_get_src(v.orb);
    xiaozhi_voice_view_status(&v,"正在聆听",XIAOZHI_VOICE_LISTENING);
    lv_tick_inc(160);lv_timer_handler();lv_refr_now(display);
    if(first==lv_image_get_src(v.orb))return 94;
    save_image("output/watchface/xiaozhi-listening.ppm");
    xiaozhi_voice_view_status(&v,"待命",XIAOZHI_VOICE_IDLE);
    if(strcmp(lv_label_get_text(v.hint),XIAOZHI_WAKE_HINT_TEXT))return 102;
    xiaozhi_voice_view_status(&v,"正在回答",XIAOZHI_VOICE_SPEAKING);
    xiaozhi_voice_view_chat(&v,"小嘉小嘉，小珈小珈");
    lv_refr_now(display);save_image("output/watchface/xiaozhi-name-subtitle.ppm");
    xiaozhi_voice_view_chat(&v,"今天宜昌天气晴，适合出去走走。");
    lv_refr_now(display);
    save_image("output/watchface/xiaozhi-speaking.ppm");
    xiaozhi_voice_view_status(&v,"等待设备激活",XIAOZHI_VOICE_IDLE);
    xiaozhi_voice_view_activation(&v,"123456","请在小智控制面板添加设备并输入激活码。",true);
    lv_refr_now(display);
    if(!lv_obj_has_flag(v.hint,LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(v.subtitle,LV_OBJ_FLAG_HIDDEN))return 95;
    save_image("output/watchface/xiaozhi-activation.ppm");
    xiaozhi_voice_view_activation(&v,"","",false);
    if(lv_obj_has_flag(v.subtitle,LV_OBJ_FLAG_HIDDEN))return 96;
    xiaozhi_voice_view_chat(&v,"");
    xiaozhi_voice_view_status(&v,"服务异常",XIAOZHI_VOICE_UNAVAILABLE);
    first=lv_image_get_src(v.orb);
    lv_tick_inc(500);lv_timer_handler();
    if(first!=lv_image_get_src(v.orb))return 97;
    lv_refr_now(display);save_image("output/watchface/xiaozhi-error.ppm");
    xiaozhi_voice_view_destroy(&v);

    lv_font_t basic=voice_preview_font;
    basic.get_glyph_dsc=basic_missing_name;
    if(!xiaozhi_voice_view_create(screen,&v,&basic,action,&actions))return 110;
    subtitle_font=lv_obj_get_style_text_font(v.subtitle,0);
    if(!lv_font_get_glyph_dsc(subtitle_font,&name_glyph,0x5609,0) || name_glyph.is_placeholder ||
       name_glyph.resolved_font!=&v.conversation_fallback)return 111;
    if(!lv_font_get_glyph_dsc(subtitle_font,&name_glyph,0x73C8,0) || name_glyph.is_placeholder)return 112;
    xiaozhi_voice_view_destroy(&v);

    lv_mem_monitor_t before,after;
    lv_mem_monitor(&before);
    for(unsigned i=0;i<30;++i) {
        xiaozhi_voice_view_create(screen,&v,&voice_preview_font,action,&actions);
        xiaozhi_voice_view_status(&v,"正在思考",XIAOZHI_VOICE_THINKING);
        lv_tick_inc(120);lv_timer_handler();lv_refr_now(display);
        /* Screen owners may delete the parent before the view is released. */
        if(i%2)lv_obj_clean(screen);
        xiaozhi_voice_view_destroy(&v);
        if(v.timer || v.root)return 98;
    }
    lv_mem_monitor(&after);
    if(after.free_size+1024<before.free_size)return 99;
    puts("VOICE_UI=PASS (subtitle U+5609/U+73C8 glyph fallback, primary metrics, basic-font fallback, black background, orb/status bounds, animation, tap action, activation/subtitle isolation, error pause, 30 parent-delete/recreate cycles)");
    return 0;
}
