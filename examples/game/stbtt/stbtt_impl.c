// stb_truetype.h is a single-header library: declarations and
// implementation share one file, with the implementation gated behind
// STB_TRUETYPE_IMPLEMENTATION. That implementation needs exactly one TU
// -- this one -- same role stb_impl.c plays for stb_image. stbtt.c
// (generated from stbtt/stbtt.3b) only sees stb_truetype_baked.h's
// trimmed declarations, but links against the real stbtt_* symbols this
// file emits from the FULL header -- their signatures are identical, so
// the linker doesn't care which header a caller saw.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
