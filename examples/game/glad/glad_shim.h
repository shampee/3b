#ifndef GAME_GLAD_SHIM_H
#define GAME_GLAD_SHIM_H

// Loads every OpenGL function pointer via GLAD. Must be called once, after
// the GL context is current, before any other gl/ function. Returns nonzero
// on success (matches gladLoadGL's own convention).
int b3_glad_load_gl(void);

#endif
