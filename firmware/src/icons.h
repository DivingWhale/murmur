#pragma once
#include <lvgl.h>

// Stroke icons from the prototype, pre-rendered as tinted alpha bitmaps.
enum class Icon { Check, Tick, Sync, Wifi, WifiOff, Sun, Moon, Back, Lock, Atom, Warn, Saved };
struct IconImage { Icon icon; int size; const lv_img_dsc_t *image; };
extern const IconImage iconImages[];
extern const unsigned iconImageCount;

// Draws `which` at a generated pixel size (see tools/gen_icons.py) with its top-left at x, y.
lv_obj_t *icon(lv_obj_t *parent, Icon which, int size, int x, int y, uint32_t color);
