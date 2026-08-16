// cgltf.h is a single-header library: declarations and implementation share
// one file, with the implementation gated behind CGLTF_IMPLEMENTATION. That
// implementation needs exactly one TU -- this one -- the same role
// glad_impl.c plays for GLAD and stb_impl.c for stb_image. cgltf.c
// (generated from cgltf/cgltf.3b) only ever sees the declarations (see
// cgltf/cgltf.cfg.3b) and links against the real cgltf_* symbols this file
// emits.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
