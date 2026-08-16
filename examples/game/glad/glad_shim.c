#include "glad_shim.h"
#include "gl.h"
#include <SDL3/SDL.h>

int
b3_glad_load_gl(void) {
  return gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
}
