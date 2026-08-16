// Hand-written, NOT part of stb_truetype.h itself -- a trimmed re-
// declaration of just the "TEXTURE BAKING API" subset this project
// actually uses (stbtt_BakeFontBitmap/stbtt_GetBakedQuad and the two
// structs they share), so stbtt.cfg.3b's `3b translate` only ever has
// to look at these ~20 lines instead of stb_truetype.h's full ~5000-line,
// 118-function surface (most of which uses double-pointer out-params,
// opaque allocator callbacks, and internal-only structs the translator
// either can't handle cleanly or that this project has no use for).
//
// Legal in C: declaring a function here and defining it elsewhere (the
// real stb_truetype.h, compiled via stbtt_impl.c's
// STB_TRUETYPE_IMPLEMENTATION) is exactly what any library's public
// header/implementation split already does, as long as every signature
// below matches stb_truetype.h's own EXACTLY -- verified by hand against
// the vendored stb_truetype.h in this same directory (v1.26).
//
// If a later feature needs more of the real API (kerning, SDF glyphs,
// multi-font atlases via the newer Pack* functions), add its exact
// declaration here rather than pointing stbtt.cfg.3b at the full header.

typedef struct {
   unsigned short x0, y0, x1, y1; // coordinates of bbox in bitmap
   float xoff, yoff, xadvance;
} stbtt_bakedchar;

typedef struct {
   float x0, y0, s0, t0; // top-left
   float x1, y1, s1, t1; // bottom-right
} stbtt_aligned_quad;

int stbtt_BakeFontBitmap(const unsigned char *data, int offset,
                          float pixel_height,
                          unsigned char *pixels, int pw, int ph,
                          int first_char, int num_chars,
                          stbtt_bakedchar *chardata);

void stbtt_GetBakedQuad(const stbtt_bakedchar *chardata, int pw, int ph,
                         int char_index,
                         float *xpos, float *ypos,
                         stbtt_aligned_quad *q,
                         int opengl_fillrule);

void stbtt_GetScaledFontVMetrics(const unsigned char *fontdata, int index, float size,
                                  float *ascent, float *descent, float *lineGap);
