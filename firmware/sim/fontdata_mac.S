// The real glyph pack, embedded for the host build (Mach-O syntax).
    .section __DATA,__const
    .p2align 2
    .globl _murmur_fonts
_murmur_fonts:
    .incbin FONTS_PATH
