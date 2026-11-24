#include "ui_kit.h"
#include "fonts.h"
#include <cmath>

namespace ui {
bool en=false;
lv_obj_t *panelRoot=nullptr, *pageLayer=nullptr;
const char *tr(const char *zh,const char *english) { return en ? english : zh; }

lv_obj_t *box(int x,int y,int w,int h,uint32_t color,int radius,uint32_t border,int thickness,
              lv_obj_t *parent) {
    auto *o=lv_obj_create(parent ? parent : (panelRoot?panelRoot:(pageLayer?pageLayer:lv_scr_act())));
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0);
    if (thickness) {
        lv_obj_set_style_border_color(o,lv_color_hex(border),0);
        lv_obj_set_style_border_width(o,thickness,0);
    }
    return o;
}
lv_obj_t *label(int x,int y,int width,const char *s,int size,uint32_t color,
                lv_text_align_t align,lv_label_long_mode_t mode,
                lv_obj_t *parent) {
    auto *o=lv_label_create(parent ? parent : (panelRoot?panelRoot:(pageLayer?pageLayer:lv_scr_act())));
    lv_obj_set_style_text_font(o,fontFor(size),0);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,align,0);
    lv_label_set_long_mode(o,mode);
    lv_obj_set_width(o,width);
    if (mode!=LV_LABEL_LONG_WRAP) lv_obj_set_height(o,fontFor(size)->line_height);
    lv_label_set_text(o,s);
    lv_obj_set_pos(o,x,y);
    return o;
}
lv_obj_t *center(int y,const char *s,int size,uint32_t color) {
    return label(8,y,224,s,size,color,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_WRAP);
}
void dots(int x,int y,int count,int selected) {
    for (int i=0;i<count;++i) box(x+i*9,y,5,5,i==selected?ink:dim,3);
}
void symbol(int x,int y,int w,const char *glyph,lv_obj_t *parent) {
    auto *o=label(x,y,w,glyph,14,ink,LV_TEXT_ALIGN_CENTER,LV_LABEL_LONG_CLIP,parent);
    lv_obj_set_style_text_font(o,LV_FONT_DEFAULT,0);
}
// Motion follows the prototype's CSS: every animation eases out.
void translate(lv_obj_t *obj,int start,int end,bool vertical,int ms) {
    lv_anim_t a;
    lv_anim_init(&a); lv_anim_set_var(&a,obj);
    if (vertical) lv_anim_set_exec_cb(&a,[](void *p,int32_t v){lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(p),v,0);});
    else lv_anim_set_exec_cb(&a,[](void *p,int32_t v){lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(p),v,0);});
    lv_anim_set_values(&a,start,end); lv_anim_set_time(&a,ms);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out); lv_anim_start(&a);
}
void fade(lv_obj_t *obj,lv_opa_t from,int ms) {
    lv_obj_set_style_opa(obj,from,0);
    lv_anim_t a;
    lv_anim_init(&a); lv_anim_set_var(&a,obj);
    lv_anim_set_exec_cb(&a,[](void *p,int32_t v){lv_obj_set_style_opa(static_cast<lv_obj_t*>(p),lv_opa_t(v),0);});
    lv_anim_set_values(&a,from,LV_OPA_COVER); lv_anim_set_time(&a,ms);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out); lv_anim_start(&a);
}
// A screen enters 40 px from its direction of travel while fading in from 20% (CSS pgL/pgR/pgU).
int enter(lv_obj_t *obj,Motion motion) {
    const bool vertical=motion==Motion::Up;
    const int ms=vertical?300:260;
    translate(obj,motion==Motion::Right?-40:40,0,vertical,ms);
    fade(obj,LV_OPA_20,ms);
    return ms;
}
// Edge bounce (CSS pgBL/pgBR): 30% -> 14 px toward the swipe, 65% -> 4 px back, then rest; 0.34 s.
void bounce(lv_obj_t *obj,int direction) {
    lv_anim_t a;
    lv_anim_init(&a); lv_anim_set_var(&a,obj);
    lv_anim_set_user_data(&a,reinterpret_cast<void*>(intptr_t(direction)));
    lv_anim_set_custom_exec_cb(&a,[](lv_anim_t *anim,int32_t t){
        const int dir=int(intptr_t(anim->user_data));
        auto ease=[](int32_t from,int32_t to,int32_t num,int32_t den){ // ease-out cubic per keyframe
            const float x=float(num)/den, e=1-(1-x)*(1-x)*(1-x);
            return int32_t(lroundf(from+(to-from)*e));
        };
        int32_t x;
        if (t<300) x=ease(0,14,t,300);
        else if (t<650) x=ease(14,-4,t-300,350);
        else x=ease(-4,0,t-650,350);
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(anim->var),dir*x,0);
    });
    lv_anim_set_values(&a,0,1000); lv_anim_set_time(&a,340);
    lv_anim_start(&a);
}
}
