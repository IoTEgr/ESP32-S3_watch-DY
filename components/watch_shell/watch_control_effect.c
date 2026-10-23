/* Snapshot-backed glass background and row-dependent convergence.
 * Shared with the native preview. No panel transfers or sensor-task work. */
#include "watch_control_view.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/lv_area_private.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif

static const unsigned tile_order[WC_TILE_COUNT]={
    WC_WIFI,WC_AIRPLANE,WC_BATTERY,WC_HAPTIC,WC_TORCH,WC_MUTE,
    WC_BRIGHTNESS,WC_VOLUME,WC_WRIST,WC_KEEP_AWAKE,WC_LOCK,WC_TIMER,
    WC_SCREEN_OFF,WC_SETTINGS
};
static void hold_layer(lv_obj_t *layer,bool *had_local,lv_style_value_t *local)
{
    *had_local=lv_obj_get_local_style_prop(layer,LV_STYLE_OPA_LAYERED,local,0)==LV_STYLE_RES_FOUND;
    lv_obj_set_style_opa_layered(layer,LV_OPA_TRANSP,0);
}
static void restore_layer(lv_obj_t *layer,bool had_local,lv_style_value_t local)
{
    if(!layer || !lv_obj_is_valid(layer))return;
    if(had_local)lv_obj_set_style_opa_layered(layer,local.num,0);
    else lv_obj_remove_local_style_prop(layer,LV_STYLE_OPA_LAYERED,0);
    lv_obj_invalidate(layer);
}
static void *effect_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return lv_malloc(size);
#endif
}
static void effect_free(void *pointer)
{
#ifdef ESP_PLATFORM
    heap_caps_free(pointer);
#else
    lv_free(pointer);
#endif
}
static int clamp_index(int value,int count)
{
    return value<0?0:value>=count?count-1:value;
}
static void box_filter(const uint16_t *source,uint16_t *dest,int w,int h,bool vertical)
{
    /* Three separable radius-3 passes on a 1/5-size RGB image approximate a
     * Gaussian blur without a full-resolution RGBA scratch allocation. */
    const int radius=3;
    const int lines=vertical?w:h,count=vertical?h:w;
    for(int line=0;line<lines;++line) {
        unsigned sum[3]={0,0,0};
        for(int k=-radius;k<=radius;++k) {
            const int pos=clamp_index(k,count);
            const uint16_t *p=source+(vertical?pos*w+line:line*w+pos)*3;
            for(int c=0;c<3;++c)sum[c]+=p[c];
        }
        for(int pos=0;pos<count;++pos) {
            uint16_t *out=dest+(vertical?pos*w+line:line*w+pos)*3;
            for(int c=0;c<3;++c)out[c]=(uint16_t)((sum[c]+3)/7);
            const int old=clamp_index(pos-radius,count),next=clamp_index(pos+radius+1,count);
            const uint16_t *a=source+(vertical?old*w+line:line*w+old)*3;
            const uint16_t *b=source+(vertical?next*w+line:line*w+next)*3;
            for(int c=0;c<3;++c)sum[c]=sum[c]-a[c]+b[c];
        }
    }
}
static bool make_blur(const lv_draw_buf_t *source,lv_draw_buf_t *dest)
{
    const int step=5,w=source->header.w,h=source->header.h;
    const int sw=(w+step-1)/step,sh=(h+step-1)/step;
    const size_t bytes=(size_t)sw*sh*3*sizeof(uint16_t);
    uint16_t *small=effect_alloc(bytes),*scratch=effect_alloc(bytes);
    if(!small||!scratch){effect_free(small);effect_free(scratch);return false;}
    for(int y=0;y<sh;++y)for(int x=0;x<sw;++x) {
        unsigned r=0,g=0,b=0,count=0;
        for(int dy=0;dy<step && y*step+dy<h;++dy) {
            const uint16_t *row=(const uint16_t *)(source->data+(y*step+dy)*source->header.stride);
            for(int dx=0;dx<step && x*step+dx<w;++dx) {
                const unsigned p=row[x*step+dx];
                r+=((p>>11)&31)*255/31;g+=((p>>5)&63)*255/63;b+=(p&31)*255/31;++count;
            }
        }
        uint16_t *out=small+(y*sw+x)*3;
        out[0]=r/count;out[1]=g/count;out[2]=b/count;
    }
    for(int pass=0;pass<3;++pass) {
        box_filter(small,scratch,sw,sh,false);
        box_filter(scratch,small,sw,sh,true);
    }
    /* Keep interpolation's working set in internal task-stack memory. Reading
     * four scattered PSRAM texels for every output channel thrashed the cache. */
    uint16_t line0[sw*3],line1[sw*3],output[w];
    int cached_y0=-1,cached_y1=-1;
    for(int y=0;y<h;++y) {
        const int gy=y*256/step;
        const int y0=clamp_index(gy>>8,sh),y1=clamp_index((gy>>8)+1,sh),fy=gy&255;
        if(y0!=cached_y0){memcpy(line0,small+y0*sw*3,sw*3*sizeof(uint16_t));cached_y0=y0;}
        if(y1!=cached_y1){memcpy(line1,small+y1*sw*3,sw*3*sizeof(uint16_t));cached_y1=y1;}
        for(int x=0;x<w;++x) {
            const int gx=x*256/step;
            const int x0=clamp_index(gx>>8,sw),x1=clamp_index((gx>>8)+1,sw),fx=gx&255;
            unsigned c[3];
            for(int channel=0;channel<3;++channel) {
                const unsigned a=line0[x0*3+channel]*(256-fx)+line0[x1*3+channel]*fx;
                const unsigned b=line1[x0*3+channel]*(256-fx)+line1[x1*3+channel]*fx;
                const unsigned value=(a*(256-fy)+b*fy+32768)>>16;
                /* Neutral cool glass: retain the actual face's colors, with
                 * enough dimming for white glyphs on a pale watch face. */
                c[channel]=value*31/100+(channel==2?14:8);
            }
            output[x]=(uint16_t)(((c[0]>>3)<<11)|((c[1]>>2)<<5)|(c[2]>>3));
        }
        memcpy(dest->data+y*dest->header.stride,output,w*sizeof(uint16_t));
    }
    effect_free(small);effect_free(scratch);
    return true;
}
void watch_control_backdrop_restore(watch_control_view_t *v)
{
    if(!v)return;
    restore_layer(v->underlay,v->underlay_had_local_opa,v->underlay_local_opa);
    for(unsigned i=0;i<2;++i) {
        restore_layer(v->aux_layers[i],v->aux_had_local_opa[i],v->aux_local_opa[i]);
        v->aux_layers[i]=NULL;
    }
    v->underlay=NULL;
}
void watch_control_backdrop_hold(watch_control_view_t *v,lv_obj_t *screen)
{
    if(!v||!screen||!v->blur_pixels)return;
    watch_control_backdrop_restore(v);
    v->underlay=screen;
    hold_layer(screen,&v->underlay_had_local_opa,&v->underlay_local_opa);
    lv_display_t *display=lv_obj_get_display(screen);
    v->aux_layers[0]=lv_display_get_layer_bottom(display);
    v->aux_layers[1]=lv_display_get_layer_top(display);
    for(unsigned i=0;i<2;++i)hold_layer(v->aux_layers[i],&v->aux_had_local_opa[i],&v->aux_local_opa[i]);
}
void watch_control_view_release(watch_control_view_t *v)
{
    if(!v)return;
    if(v->background)lv_obj_add_flag(v->background,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(v->root,LV_OPA_COVER,0);
    watch_control_backdrop_restore(v);
    lv_image_cache_drop(&v->sharp_buffer);lv_image_cache_drop(&v->blur_buffer);
    effect_free(v->sharp_pixels);effect_free(v->blur_pixels);
    v->sharp_pixels=NULL;v->blur_pixels=NULL;
    for(unsigned i=0;i<WC_TILE_COUNT;++i) {
        effect_free(v->sprite_pixels[i]);v->sprite_pixels[i]=NULL;
        memset(&v->sprites[i],0,sizeof(v->sprites[i]));v->sprite_keys[i]=0;
        if(v->tiles[i])lv_obj_remove_local_style_prop(v->tiles[i],LV_STYLE_OPA_LAYERED,0);
    }
    v->sprites_ready=false;v->motion_raster=false;
    memset(&v->sharp_buffer,0,sizeof(v->sharp_buffer));
    memset(&v->blur_buffer,0,sizeof(v->blur_buffer));
}
bool watch_control_view_capture(watch_control_view_t *v,lv_obj_t *screen)
{
    if(!v||!screen)return false;
    watch_control_view_release(v);
    lv_obj_update_layout(v->root);
    lv_obj_update_layout(screen);
    const unsigned w=lv_obj_get_width(v->root),h=lv_obj_get_height(v->root);
    const uint32_t stride=lv_draw_buf_width_to_stride(w,LV_COLOR_FORMAT_RGB565);
    const size_t size=(size_t)stride*h;
    v->sharp_pixels=effect_alloc(size);v->blur_pixels=effect_alloc(size);
    if(!v->sharp_pixels||!v->blur_pixels)goto failed;
    if(lv_draw_buf_init(&v->sharp_buffer,w,h,LV_COLOR_FORMAT_RGB565,stride,v->sharp_pixels,size)!=LV_RESULT_OK ||
       lv_draw_buf_init(&v->blur_buffer,w,h,LV_COLOR_FORMAT_RGB565,stride,v->blur_pixels,size)!=LV_RESULT_OK)goto failed;
    #if WATCH_DEBUG_ENABLED
    const uint32_t snapshot_start=lv_tick_get();
    #endif
    if(lv_snapshot_take_to_draw_buf(screen,LV_COLOR_FORMAT_RGB565,&v->sharp_buffer)!=LV_RESULT_OK)goto failed;
    #if WATCH_DEBUG_ENABLED
    v->snapshot_ms=lv_tick_elaps(snapshot_start);
    const uint32_t blur_start=lv_tick_get();
    #endif
    if(!make_blur(&v->sharp_buffer,&v->blur_buffer))goto failed;
    #if WATCH_DEBUG_ENABLED
    v->blur_ms=lv_tick_elaps(blur_start);
    #endif
    lv_obj_remove_flag(v->background,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(v->root,LV_OPA_TRANSP,0);
    watch_control_backdrop_hold(v,screen);
    return true;
failed:
    watch_control_view_release(v);
    return false;
}
#if WATCH_DEBUG_ENABLED
static int64_t effect_now_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return (int64_t)lv_tick_get()*1000;
#endif
}
#endif
/* Direct writes must wait for all earlier asynchronous LVGL work on this
 * layer. Later child tasks are submitted only after this draw event returns.
 * Unlike lv_draw_wait_for_finish(), this also drains SW draw units. */
static bool prepare_layer(lv_layer_t *layer)
{
    if(layer->color_format!=LV_COLOR_FORMAT_RGB565)return false;
    while(layer->draw_task_head) {
        lv_draw_dispatch();
        if(layer->draw_task_head)lv_draw_dispatch_wait_for_request();
    }
    return lv_draw_layer_alloc_buf(layer)!=NULL;
}
void watch_control_view_draw_background(lv_event_t *event)
{
    watch_control_view_t *v=lv_event_get_user_data(event);
    if(!v->sharp_pixels)return;
    lv_layer_t *layer=lv_event_get_layer(event);
    lv_area_t coords,area;
    lv_obj_get_coords(v->background,&coords);
    if(!lv_area_intersect(&area,&coords,&layer->_clip_area))return;
    if(!prepare_layer(layer))return;
    #if WATCH_DEBUG_ENABLED
    const int64_t start=effect_now_us();
    #endif
    const unsigned alpha=(v->transition_progress*32+512)/1024;
    const int x=area.x1-coords.x1,w=lv_area_get_width(&area);
    /* Small internal chunks avoid alternating between three full PSRAM
     * streams. The destination is the DMA draw buffer, never a PSRAM frame. */
    uint16_t sharp[64],blur[64];
    for(int y=area.y1;y<=area.y2;++y) {
        const int sy=y-coords.y1;
        const uint16_t *a=(const uint16_t *)(v->sharp_buffer.data+sy*v->sharp_buffer.header.stride)+x;
        const uint16_t *b=(const uint16_t *)(v->blur_buffer.data+sy*v->blur_buffer.header.stride)+x;
        uint16_t *dest=(uint16_t *)(layer->draw_buf->data+(y-layer->buf_area.y1)*layer->draw_buf->header.stride)+(area.x1-layer->buf_area.x1);
        if(alpha==0 || alpha==32) {memcpy(dest,alpha?b:a,w*sizeof(uint16_t));continue;}
        for(int offset=0;offset<w;offset+=64) {
            const unsigned n=LV_MIN(64,w-offset);
            memcpy(sharp,a+offset,n*sizeof(uint16_t));memcpy(blur,b+offset,n*sizeof(uint16_t));
            for(unsigned i=0;i<n;++i) {
                const uint32_t s=(sharp[i]|((uint32_t)sharp[i]<<16))&0x07E0F81F;
                const uint32_t d=(blur[i]|((uint32_t)blur[i]<<16))&0x07E0F81F;
                const uint32_t mixed=((((d-s)*alpha)>>5)+s)&0x07E0F81F;
                dest[offset+i]=(uint16_t)(mixed|(mixed>>16));
            }
        }
    }
    #if WATCH_DEBUG_ENABLED
    v->background_us+=effect_now_us()-start;++v->raster_calls;
    #endif
}
static uint32_t sprite_key(lv_obj_t *tile)
{
    uint32_t key=lv_color_to_u32(lv_obj_get_style_bg_color(tile,0));
    key=key*16777619u+lv_obj_get_style_bg_opa(tile,0);
    lv_obj_t *child=lv_obj_get_child(tile,0);
    if(lv_obj_check_type(child,&lv_label_class)) {
        const unsigned char *text=(const unsigned char *)lv_label_get_text(child);
        while(*text)key=(key^*text++)*16777619u;
    }
    return key;
}
bool watch_control_view_prepare_motion(watch_control_view_t *v)
{
    if(v->motion_detached)return v->sprites_ready;
    v->sprite_prepare_ms=0;
    const uint32_t start=lv_tick_get();
    bool ready=true;
    const int scroll_y=lv_obj_get_scroll_y(v->list);
    for(unsigned slot=0;slot<WC_TILE_COUNT;++slot) {
        const unsigned i=tile_order[slot];
        const int final_y=82+(slot/2)*126-scroll_y;
        /* Dispersal moves off-screen rows even farther away. Snapshot only
         * capsules which can actually appear; fill other rows on demand. */
        if(final_y>=502 || final_y+112<=0)continue;
        lv_obj_t *tile=v->tiles[i];
        const uint32_t key=sprite_key(tile);
        if(v->sprite_pixels[i] && v->sprite_keys[i]==key)continue;
        const unsigned w=lv_obj_get_width(tile),h=lv_obj_get_height(tile);
        const uint32_t stride=lv_draw_buf_width_to_stride(w,LV_COLOR_FORMAT_ARGB8888);
        const size_t size=(size_t)stride*h;
        if(!v->sprite_pixels[i])v->sprite_pixels[i]=effect_alloc(size);
        if(!v->sprite_pixels[i] ||
           lv_draw_buf_init(&v->sprites[i],w,h,LV_COLOR_FORMAT_ARGB8888,stride,v->sprite_pixels[i],size)!=LV_RESULT_OK ||
           lv_snapshot_take_to_draw_buf(tile,LV_COLOR_FORMAT_ARGB8888,&v->sprites[i])!=LV_RESULT_OK) {ready=false;continue;}
        /* Prepack RGB565 channels into separate lanes. Alpha uses spare bits
         * 27..31 and 5..7, so drawing needs one multiply for all three color
         * channels, at the same 5-bit mix precision used by LVGL RGB565. */
        uint32_t words[64];
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;x+=64) {
            const unsigned n=LV_MIN(64,w-x);
            uint8_t *row=v->sprites[i].data+y*v->sprites[i].header.stride+x*4;
            memcpy(words,row,n*4);
            for(unsigned k=0;k<n;++k) {
                const uint32_t rgba=words[k],alpha=rgba>>24;
                const uint32_t rgb565=((rgba>>8)&0xf800)|((rgba>>5)&0x07e0)|((rgba>>3)&31);
                words[k]=((rgb565|(rgb565<<16))&0x07e0f81f)|((alpha&31)<<27)|(alpha&224);
            }
            memcpy(row,words,n*4);
        }
        v->sprites[i].header.cf=LV_COLOR_FORMAT_RAW;
        v->sprite_keys[i]=key;
    }
    v->sprite_prepare_ms=lv_tick_elaps(start);v->sprites_ready=ready;
    return ready;
}
void watch_control_view_draw_motion(lv_event_t *event)
{
    watch_control_view_t *v=lv_event_get_user_data(event);
    if(!v->motion_detached || !v->motion_raster || !v->tile_opa)return;
    lv_layer_t *layer=lv_event_get_layer(event);
    lv_area_t root,clip;
    lv_obj_get_coords(v->root,&root);
    if(!lv_area_intersect(&clip,&root,&layer->_clip_area))return;
    if(!prepare_layer(layer))return;
    #if WATCH_DEBUG_ENABLED
    const int64_t start=effect_now_us();
    #endif
    /* Read small sprite runs into internal memory, then blend onto the draw
     * buffer. No rounded masks, fonts, image decoders or draw tasks per tile. */
    uint32_t pixels[64];
    for(unsigned i=0;i<WC_TILE_COUNT;++i) {
        if(!v->sprite_pixels[i])continue;
        const lv_draw_buf_t *sprite=&v->sprites[i];
        lv_area_t bounds={root.x1+v->motion_x[i],root.y1+v->motion_y[i],0,0},area;
        bounds.x2=bounds.x1+sprite->header.w-1;bounds.y2=bounds.y1+sprite->header.h-1;
        if(!lv_area_intersect(&area,&bounds,&clip))continue;
        const int w=lv_area_get_width(&area);
        for(int y=area.y1;y<=area.y2;++y) {
            const uint8_t *source=sprite->data+(y-bounds.y1)*sprite->header.stride+(area.x1-bounds.x1)*4;
            uint16_t *dest=(uint16_t *)(layer->draw_buf->data+(y-layer->buf_area.y1)*layer->draw_buf->header.stride)+(area.x1-layer->buf_area.x1);
            for(int offset=0;offset<w;offset+=64) {
                const unsigned n=LV_MIN(64,w-offset);memcpy(pixels,source+offset*4,n*4);
                for(unsigned x=0;x<n;++x) {
                    const uint32_t p=pixels[x];
                    unsigned alpha=(p>>27)|(p&224);
                    if(v->tile_opa!=255)alpha=LV_OPA_MIX2(alpha,v->tile_opa);
                    if(!alpha)continue;
                    const uint32_t fg=p&0x07e0f81f;
                    if(alpha==255)dest[offset+x]=(uint16_t)(fg|(fg>>16));
                    else {
                        const uint32_t bg565=dest[offset+x];
                        const uint32_t bg=(bg565|(bg565<<16))&0x07e0f81f;
                        const uint32_t result=(((((fg-bg)*((alpha+4)>>3))>>5)+bg))&0x07e0f81f;
                        dest[offset+x]=(uint16_t)(result|(result>>16));
                    }
                }
            }
        }
    }
    #if WATCH_DEBUG_ENABLED
    v->sprites_us+=effect_now_us()-start;
    #endif
}
void watch_control_view_transition(watch_control_view_t *v,int progress)
{
    if(progress<0)progress=0;
    if(progress>1024)progress=1024;
    v->transition_progress=progress;
    const unsigned *order=tile_order;
    const int remaining=1024-progress;
    if(progress<1024 && !v->motion_detached) {
        v->motion_raster=watch_control_view_prepare_motion(v);
        if(v->motion_raster)for(unsigned i=0;i<WC_TILE_COUNT;++i)
            lv_obj_set_style_opa_layered(v->tiles[i],LV_OPA_TRANSP,0);
        v->motion_scroll_y=lv_obj_get_scroll_y(v->list);
        lv_obj_remove_flag(v->list,LV_OBJ_FLAG_SCROLLABLE);
        for(unsigned i=0;i<WC_TILE_COUNT;++i)lv_obj_set_parent(v->tiles[i],v->motion_layer);
        v->motion_detached=true;
    }
    const int scroll_y=v->motion_detached?v->motion_scroll_y:lv_obj_get_scroll_y(v->list);
    /* Keep the background anchored to the watch. Only the capsules move;
     * lower rows begin farther away from the upper-center convergence point. */
    lv_obj_set_pos(v->root,0,0);
    lv_obj_invalidate(v->background);
    lv_obj_set_style_opa(v->connection,(progress*255+512)/1024,0);
    lv_obj_set_style_opa(v->detail,(progress*255+512)/1024,0);
    const int tile_opa=progress<40?0:progress>=930?255:(progress-40)*255/890;
    v->tile_opa=tile_opa;
    for(unsigned slot=0;slot<WC_TILE_COUNT;++slot) {
        const int col=slot%2,row=slot/2;
        const int base_x=col*182,base_y=row*126;
        const int center_y=82+base_y+56-scroll_y;
        const int dx=(col?48:-48)*remaining/1024;
        const int dy=(center_y-190)*85*remaining/(100*1024);
        const int x=v->motion_detached?28+base_x+dx:base_x;
        const int y=v->motion_detached?82+base_y-scroll_y+dy:base_y;
        v->motion_x[order[slot]]=x;v->motion_y[order[slot]]=y;
        lv_obj_set_pos(v->tiles[order[slot]],x,y);
        lv_obj_set_style_opa(v->tiles[order[slot]],tile_opa,0);
    }
    lv_obj_set_scrollbar_mode(v->list,progress==1024?LV_SCROLLBAR_MODE_AUTO:LV_SCROLLBAR_MODE_OFF);
    if(progress==1024) {
        if(v->motion_detached) {
            for(unsigned slot=0;slot<WC_TILE_COUNT;++slot) {
                lv_obj_t *tile=v->tiles[order[slot]];
                lv_obj_remove_local_style_prop(tile,LV_STYLE_OPA_LAYERED,0);
                lv_obj_set_parent(tile,v->list);
                lv_obj_set_pos(tile,(slot%2)*182,(slot/2)*126);
            }
            lv_obj_update_layout(v->list);
            lv_obj_scroll_to_y(v->list,v->motion_scroll_y,LV_ANIM_OFF);
            v->motion_detached=false;v->motion_raster=false;
        }
        lv_obj_add_flag(v->list,LV_OBJ_FLAG_SCROLLABLE);
    }
}
