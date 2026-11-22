#pragma once
#include <lvgl.h>
#include <cstdint>

// Drawing primitives, palette and motion shared by every screen. Colours and timings
// follow prototype/index.html and docs/ui-spec.md.
namespace ui {
constexpr uint32_t bg=0x0b0c0e, surface=0x1c1f24, line=0x262a30, ink=0xf4f3ef,
    muted=0x9ca0a8, dim=0x6b7079, yellow=0xffd23f, red=0xff5c4d, due=0xffb067, syncBlue=0x6cb6ff;

extern bool en;                          // Screen language: true for English, false for Chinese.
const char *tr(const char *zh,const char *english);

// New objects go into panelRoot if set, else pageLayer, else the active screen.
extern lv_obj_t *panelRoot, *pageLayer;
lv_obj_t *box(int x,int y,int w,int h,uint32_t color,int radius=0,uint32_t border=0,int thickness=0,
              lv_obj_t *parent=nullptr);
lv_obj_t *label(int x,int y,int width,const char *s,int size,uint32_t color=ink,
                lv_text_align_t align=LV_TEXT_ALIGN_LEFT,lv_label_long_mode_t mode=LV_LABEL_LONG_CLIP,
                lv_obj_t *parent=nullptr);
lv_obj_t *center(int y,const char *s,int size,uint32_t color=ink); // Wrapped, centred, 8 px side margins.
void dots(int x,int y,int count,int selected);
void symbol(int x,int y,int w,const char *glyph,lv_obj_t *parent=nullptr); // LVGL built-in symbol glyph.

enum class Motion { None, Left, Right, Up, EdgeLeft, EdgeRight };
void translate(lv_obj_t *obj,int start,int end,bool vertical,int ms);
void fade(lv_obj_t *obj,lv_opa_t from,int ms);
int enter(lv_obj_t *obj,Motion motion);  // Screen entrance; returns its duration in ms.
void bounce(lv_obj_t *obj,int direction); // Edge bounce, direction -1 (left) or 1 (right).
}
