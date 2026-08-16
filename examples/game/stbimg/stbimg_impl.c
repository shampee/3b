// stb_image.h is a single-header library: declarations and implementation
// share one file, with the implementation gated behind
// STB_IMAGE_IMPLEMENTATION. That implementation needs exactly one TU --
// this one -- same role glad_impl.c plays for GLAD. stbimg.c (generated
// from stbimg/stbimg.3b) only ever sees the declarations (see stbimg/
// stbimg.cfg.3b) and links against the real stbi_* symbols this file
// emits -- the PACKAGE is `stbimg`, not `stbi`, precisely so its
// generated wrapper functions' own C names never collide with those
// real stbi_* symbols (see stbimg.cfg.3b's own comment).
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
