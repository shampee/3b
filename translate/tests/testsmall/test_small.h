typedef struct GLXContext_t *GLXContext;
typedef unsigned int GLenum;
typedef struct { float x, y; } Vector2;
struct Player {
  Vector2 position;
  int health;
  struct { int a; int b; } nested;
};
extern int glGetError(void);
extern void glGenTextures(int n, unsigned int *textures);
extern void glVaradic(int n, ...);
#define GL_TEXTURE_2D 0x0DE1
#define GL_TRUE 1
#define GL_VERSION_STR "1.0"
#define GL_COMPUTE(x) ((x) + 1)
