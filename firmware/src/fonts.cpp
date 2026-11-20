#include "fonts.h"
#include <cstring>

extern "C" const uint8_t murmur_fonts[];

namespace {
struct Face { uint32_t table, count; };
struct __attribute__((packed)) Glyph {
    uint16_t code;
    uint32_t offset;
    uint8_t advance, width, height;
    int8_t left, bottom;
    uint8_t padding;
};
static_assert(sizeof(Glyph) == 12, "Glyph format must match pack_fonts.py");
const int sizes[] = {13,14,16,17,18,20,34,56,40};
lv_font_t fonts[sizeof(sizes)/sizeof(*sizes)]{};

const Glyph *lookup(const lv_font_t *font, uint32_t code) {
    auto face = static_cast<const Face *>(font->dsc);
    auto glyphs = reinterpret_cast<const Glyph *>(murmur_fonts + face->table);
    uint32_t lo = 0, hi = face->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (glyphs[mid].code < code) lo = mid + 1;
        else hi = mid;
    }
    if (lo < face->count && glyphs[lo].code == code) return glyphs + lo;
    return code == 0x25A1 ? nullptr : lookup(font, 0x25A1);
}
bool describe(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t code, uint32_t) {
    const Glyph *g = lookup(font, code);
    if (!g) return false;
    out->adv_w = g->advance;
    out->box_w = g->width;
    out->box_h = g->height;
    out->ofs_x = g->left;
    if (font==&fonts[6] && code>='0' && code<='9') {
        const int cell=lookup(font,'0')->advance;
        out->adv_w=cell;
        out->ofs_x+=(cell-g->advance)/2;
    }
    out->ofs_y = g->bottom;
    out->bpp = 4;
    out->is_placeholder = false;
    return true;
}
const uint8_t *bitmap(const lv_font_t *font, uint32_t code) {
    const Glyph *g = lookup(font, code);
    return g ? murmur_fonts + g->offset : nullptr;
}
}

const lv_font_t *fontFor(int size) {
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(*sizes); ++i) {
        if (size != sizes[i]) continue;
        auto &font = fonts[i];
        font.get_glyph_dsc = describe;
        font.get_glyph_bitmap = bitmap;
        font.line_height = size + (size+3)/4;
        font.base_line = (size+3)/4;
        font.dsc = reinterpret_cast<const Face *>(murmur_fonts) + i;
        return &font;
    }
    return LV_FONT_DEFAULT;
}
