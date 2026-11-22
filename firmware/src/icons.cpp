#include "icons.h"

lv_obj_t *icon(lv_obj_t *parent, Icon which, int size, int x, int y, uint32_t color) {
    const lv_img_dsc_t *best = nullptr;
    int bestGap = 1 << 30;
    for (unsigned i = 0; i < iconImageCount; ++i) {
        const auto &entry = iconImages[i];
        const int gap = abs(entry.size - size);
        if (entry.icon == which && gap < bestGap) { best = entry.image; bestGap = gap; }
    }
    auto *img = lv_img_create(parent);
    if (best) lv_img_set_src(img, best);
    lv_obj_set_style_img_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_pos(img, x, y);
    return img;
}
