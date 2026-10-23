/* Tap the bottom overview to open the focused honeycomb, without animation. */
#include "watch_launcher_view.h"
#include "src/core/lv_obj_event_private.h"
#include <string.h>
LV_IMAGE_DECLARE(launcher_overview);
enum { FAVORITE_ICON_SIZE = 106 };
static void arrange(watch_launcher_view_t *v);
static void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
}
static void circular_hit(lv_event_t *e)
{
    lv_obj_t *obj=lv_event_get_current_target(e);
    lv_hit_test_info_t *hit=lv_event_get_hit_test_info(e);
    lv_area_t bounds;lv_obj_get_coords(obj,&bounds);
    const int dx=2*hit->point->x-bounds.x1-bounds.x2;
    const int dy=2*hit->point->y-bounds.y1-bounds.y2;
    const int diameter=lv_area_get_width(&bounds)-1;
    hit->res=dx*dx+dy*dy<=diameter*diameter;
}
static void launch_click(lv_event_t *e)
{
    watch_launcher_view_t *v=lv_event_get_user_data(e);
    int id=(int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if(!v->dragging && v->launch && id>=0)v->launch(v->context,id);
}
static void overview_click(lv_event_t *e)
{
    watch_launcher_view_t *v=lv_event_get_user_data(e);
    if(!v->dragging)watch_launcher_view_page(v,true,true);
}
static void touch(lv_event_t *e)
{
    watch_launcher_view_t *v=lv_event_get_user_data(e);
    lv_indev_t *input=lv_indev_active();
    if(!input)return;
    const lv_event_code_t code=lv_event_get_code(e);
    lv_point_t point;lv_indev_get_point(input,&point);
    if(code==LV_EVENT_PRESSED) {
        v->dragging=false;v->press=point;
    } else if(code==LV_EVENT_PRESSING) {
        /* A swipe never changes pages or turns into an icon click. The all-app
         * page still uses LVGL's normal scrolling. */
        if(LV_ABS(point.x-v->press.x)>12 || LV_ABS(point.y-v->press.y)>12)v->dragging=true;
    }
}
static void scrolled(lv_event_t *e)
{
    watch_launcher_view_t *v=lv_event_get_user_data(e);
    if(v->all_apps)arrange(v);
}
static void resize_icon(lv_obj_t *cell,int x,int y,int size)
{
    lv_obj_set_pos(cell,x-size/2,y-size/2);lv_obj_set_size(cell,size,size);
    lv_obj_t *image=lv_obj_get_child(cell,0);
    if(image) {lv_obj_set_size(image,size,size);lv_obj_center(image);}
}
static void arrange(watch_launcher_view_t *v)
{
    const int scroll=lv_obj_get_scroll_y(v->all);
    for(unsigned i=0;i<v->app_count;++i) {
        const int dx=v->centers[i][0]-205,dy=v->centers[i][1]-scroll-251;
        const int radial=LV_MIN(1024,(int64_t)dx*dx*1024/(185*185)+
            (int64_t)dy*dy*1024/(222*222));
        /* Lens compression keeps the smaller peripheral icons close together. */
        const int x=205+dx*(1024-radial*110/1024)/1024;
        const int y=251+dy*(1024-radial*110/1024)/1024;
        const int size=92-50*radial/1024;
        resize_icon(v->cells[i],x,y,size);
        lv_obj_set_flag(v->cells[i],LV_OBJ_FLAG_HIDDEN,y < -50 || y > 552);
    }
}
static lv_obj_t *icon(lv_obj_t *parent,const watch_launcher_app_t *app,
                      int x,int y,int size,watch_launcher_view_t *v)
{
    lv_obj_t *cell=lv_button_create(parent);plain(cell);
    lv_obj_set_pos(cell,x,y);lv_obj_set_size(cell,size,size);
    lv_obj_set_style_radius(cell,LV_RADIUS_CIRCLE,0);
    /* Assets already carry a circular alpha edge. A second rounded clip can
     * trim the last image row differently at partial-buffer boundaries. */
    lv_obj_set_style_bg_color(cell,lv_color_hex(0x31343D),0);
    lv_obj_set_style_bg_opa(cell,app->icon?LV_OPA_TRANSP:LV_OPA_COVER,0);
    lv_obj_add_flag(cell,LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(cell,LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(cell,circular_hit,LV_EVENT_HIT_TEST,NULL);
    lv_obj_set_user_data(cell,(void *)(intptr_t)app->id);
    lv_obj_add_event_cb(cell,launch_click,LV_EVENT_CLICKED,v);
    if(app->icon) {
        lv_obj_t *image=lv_image_create(cell);
        lv_obj_set_size(image,size,size);
        lv_image_set_src(image,app->icon);
        lv_image_set_inner_align(image,LV_IMAGE_ALIGN_STRETCH);
        lv_obj_center(image);
        lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(image,LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    if(app->id<0)lv_obj_add_state(cell,LV_STATE_DISABLED);
    return cell;
}
bool watch_launcher_view_create(lv_obj_t *parent,watch_launcher_view_t *v,
    const watch_launcher_app_t favorites[WATCH_LAUNCHER_FAVORITES],const watch_launcher_app_t *apps,unsigned count,
    watch_launcher_launch_cb launch,void *context)
{
    if(!parent||!v||!favorites||!apps||count>WATCH_LAUNCHER_MAX_APPS)return false;
    memset(v,0,sizeof(*v));v->launch=launch;v->context=context;v->app_count=count;
    if(!watch_control_backdrop_create(parent,&v->backdrop))return false;
    v->home=v->backdrop.root;
    lv_obj_remove_flag(v->home,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v->home,touch,LV_EVENT_ALL,v);
    /* ChatGPT sits at the center; five apps and overview form the hexagon. */
    const int centers[7][2]={{205,123},{94,187},{316,187},{94,315},{316,315},{205,251},{205,379}};
    for(unsigned i=0;i<WATCH_LAUNCHER_FAVORITES;++i)
        v->favorites[i]=icon(v->home,&favorites[i],
            centers[i][0]-FAVORITE_ICON_SIZE/2,centers[i][1]-FAVORITE_ICON_SIZE/2,FAVORITE_ICON_SIZE,v);
    v->overview=lv_button_create(v->home);plain(v->overview);
    lv_obj_set_size(v->overview,FAVORITE_ICON_SIZE,FAVORITE_ICON_SIZE);
    lv_obj_set_pos(v->overview,centers[6][0]-FAVORITE_ICON_SIZE/2,centers[6][1]-FAVORITE_ICON_SIZE/2);
    lv_obj_set_style_radius(v->overview,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(v->overview,lv_color_hex(0xCBD6EC),0);
    lv_obj_set_style_bg_opa(v->overview,65,0);
    lv_obj_add_event_cb(v->overview,overview_click,LV_EVENT_CLICKED,v);
    lv_obj_add_flag(v->overview,LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(v->overview,LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(v->overview,circular_hit,LV_EVENT_HIT_TEST,NULL);
    lv_obj_t *overview_image=lv_image_create(v->overview);
    lv_image_set_src(overview_image,&launcher_overview);
    lv_obj_set_size(overview_image,FAVORITE_ICON_SIZE,FAVORITE_ICON_SIZE);
    lv_image_set_inner_align(overview_image,LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(overview_image);
    lv_obj_remove_flag(overview_image,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overview_image,LV_OBJ_FLAG_GESTURE_BUBBLE);
    v->all=lv_obj_create(parent);plain(v->all);
    lv_obj_set_size(v->all,410,502);lv_obj_set_pos(v->all,0,0);
    lv_obj_set_style_bg_color(v->all,lv_color_black(),0);
    lv_obj_set_style_bg_opa(v->all,LV_OPA_COVER,0);
    lv_obj_add_flag(v->all,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(v->all,LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(v->all,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->all,LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(v->all,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v->all,touch,LV_EVENT_ALL,v);
    lv_obj_add_event_cb(v->all,scrolled,LV_EVENT_SCROLL,v);
    unsigned remaining=count,rows=0;
    while(remaining){unsigned capacity=rows%2?3:4;remaining-=LV_MIN(remaining,capacity);++rows;}
    const int height=rows?(int)(rows-1)*94+76:0;
    const int top=height<438?(502-height)/2:80;
    unsigned index=0;
    for(unsigned row=0;index<count;++row) {
        const unsigned n=LV_MIN(count-index,row%2?3:4);
        const int left=(410-((int)n*76+((int)n-1)*20))/2;
        for(unsigned col=0;col<n;++col,++index) {
            v->centers[index][0]=left+col*96+38;
            v->centers[index][1]=top+row*94+38;
            v->cells[index]=icon(v->all,&apps[index],left+col*96,top+row*94,76,v);
            lv_obj_add_flag(v->cells[index],LV_OBJ_FLAG_FLOATING);
        }
    }
    lv_obj_t *extent=lv_obj_create(v->all);plain(extent);
    lv_obj_remove_flag(extent,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(extent,204,0);
    lv_obj_set_size(extent,1,height<438?502:top+height+170);
    /* Keep favorites in nearby slots, with music at the focus. */
    bool assigned[WATCH_LAUNCHER_MAX_APPS]={false};
    for(unsigned f=0;f<WATCH_LAUNCHER_FAVORITES;++f) {
        unsigned item=count;
        for(unsigned i=0;i<count;++i)if(apps[i].id==favorites[f].id){item=i;break;}
        if(item==count)continue;
        const int tx=f?centers[f][0]:205,ty=f?centers[f][1]:251;
        int best_distance=0x7fffffff;unsigned best=item;
        for(unsigned i=0;i<count;++i)if(!assigned[i]) {
            const int dx=v->centers[i][0]-tx,dy=v->centers[i][1]-ty;
            if(dx*dx+dy*dy<best_distance){best_distance=dx*dx+dy*dy;best=i;}
        }
        int16_t swap[2];memcpy(swap,v->centers[item],sizeof(swap));
        memcpy(v->centers[item],v->centers[best],sizeof(swap));
        memcpy(v->centers[best],swap,sizeof(swap));assigned[item]=true;
    }
    arrange(v);
    return true;
}
void watch_launcher_view_page(watch_launcher_view_t *v,bool all_apps,bool reset_scroll)
{
    v->all_apps=all_apps;
    v->dragging=false;
    lv_obj_add_flag(v->all,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *surface=all_apps?v->all:v->home;
    lv_obj_add_flag(all_apps?v->home:v->all,LV_OBJ_FLAG_HIDDEN);
    if(all_apps&&reset_scroll)lv_obj_scroll_to_y(v->all,0,LV_ANIM_OFF);
    if(all_apps)arrange(v);
    lv_obj_remove_flag(surface,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(surface,-1);
    lv_obj_invalidate(surface);
}
void watch_launcher_view_hide(watch_launcher_view_t *v)
{
    if(!v)return;
    v->dragging=false;
    lv_obj_add_flag(v->home,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(v->all,LV_OBJ_FLAG_HIDDEN);
    watch_control_backdrop_restore(&v->backdrop);
}
lv_obj_t *watch_launcher_view_surface(watch_launcher_view_t *v)
{
    return v?(v->all_apps?v->all:v->home):NULL;
}
