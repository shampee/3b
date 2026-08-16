#include "base.h"

#if !COMPILER_TCC
Thread_Local static Context* tls_ctx = NULL;
#endif
static Context*              g_ctx   = NULL;

u32 sign32     = 0x80000000;
u32 exponent32 = 0x7f800000;
u32 mantissa32 = 0x007FFFFF;

f32 big_golden32   = 1.61803398875f;
f32 small_golden32 = 0.61803398875f;

f32 pi32 = 3.1415926535897f;

f64 machine_epsilon64 = 4.94065645841247e-324;

u64 max_u64 = 0xffffffffffffffffull;
u32 max_u32 = 0xffffffff;
u16 max_u16 = 0xffff;
u8  max_u8  = 0xff;

i64 max_i64 = (i64)0x7fffffffffffffffll;
i32 max_i32 = (i32)0x7fffffff;
i16 max_i16 = (i16)0x7fff;
i8  max_i8  =  (i8)0x7f;

i64 min_i64 = (i64)0x8000000000000000ll;
i32 min_i32 = (i32)0x80000000;
i16 min_i16 = (i16)0x8000;
i8  min_i8  =  (i8)0x80;

u8 integer_symbols[16] = {
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
};

u8 integer_symbol_reverse[128] = {
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};

u8 base64[64] = {
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
  'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
  'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
  '_', '$',
};

u8 base64_reverse[128] = {
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0x3F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
  0xFF,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,
  0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0xFF,0xFF,0xFF,0xFF,0x3E,
  0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
  0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0xFF,0xFF,0xFF,0xFF,0xFF,
};

u16
safe_cast_u16(u32 x) {
  AssertAlways(x <= max_u16);
  u16 result = (u16)x;
  return result;
}

u32
safe_cast_u32(u64 x) {
  AssertAlways(x <= max_u32);
  u32 result = (u32)x;
  return result;
}

i32
safe_cast_i32(i64 x) {
  AssertAlways(x <= max_i32);
  i32 result = (i32)x;
  return result;
}

////////////////////////////////
//~ Scalar Math Ops

f32
mix_1f32(f32 a, f32 b, f32 t) {
  f32 c = (a + (b - a) * Clamp(0.f, t, 1.f));
  return c;
}

f64
mix_1f64(f64 a, f64 b, f64 t) {
  f64 c = (a + (b - a) * Clamp(0.0, t, 1.0));
  return c;
}

////////////////////////////////
//~ Vector Ops
Vec2f32 vec_2f32(f32 x, f32 y)                         {Vec2f32 v = {x, y}; return v;}
Vec2f32 add_2f32(Vec2f32 a, Vec2f32 b)                 {Vec2f32 c = {a.x+b.x, a.y+b.y}; return c;}
Vec2f32 sub_2f32(Vec2f32 a, Vec2f32 b)                 {Vec2f32 c = {a.x-b.x, a.y-b.y}; return c;}
Vec2f32 mul_2f32(Vec2f32 a, Vec2f32 b)                 {Vec2f32 c = {a.x*b.x, a.y*b.y}; return c;}
Vec2f32 div_2f32(Vec2f32 a, Vec2f32 b)                 {Vec2f32 c = {a.x/b.x, a.y/b.y}; return c;}
Vec2f32 scale_2f32(Vec2f32 v, f32 s)                   {Vec2f32 c = {v.x*s, v.y*s}; return c;}
f32 dot_2f32(Vec2f32 a, Vec2f32 b)                     {f32 c = a.x*b.x + a.y*b.y; return c;}
f32 length_squared_2f32(Vec2f32 v)                     {f32 c = v.x*v.x + v.y*v.y; return c;}
f32 length_2f32(Vec2f32 v)                             {f32 c = sqrt_f32(v.x*v.x + v.y*v.y); return c;}
Vec2f32 normalize_2f32(Vec2f32 v)                      {v = scale_2f32(v, 1.f/length_2f32(v)); return v;}
Vec2f32 mix_2f32(Vec2f32 a, Vec2f32 b, f32 t)          {Vec2f32 c = {mix_1f32(a.x, b.x, t), mix_1f32(a.y, b.y, t)}; return c;}
Vec2f32 lerp_2f32(Vec2f32 a, Vec2f32 b, f32 t)         {Vec2f32 c = {0}; scale_2f32(add_2f32(a, sub_2f32(b, a)), t); return c;}

Vec2i64 vec_2i64(i64 x, i64 y)                         {Vec2i64 v = {x, y}; return v;}
Vec2i64 add_2i64(Vec2i64 a, Vec2i64 b)                 {Vec2i64 c = {a.x+b.x, a.y+b.y}; return c;}
Vec2i64 sub_2i64(Vec2i64 a, Vec2i64 b)                 {Vec2i64 c = {a.x-b.x, a.y-b.y}; return c;}
Vec2i64 mul_2i64(Vec2i64 a, Vec2i64 b)                 {Vec2i64 c = {a.x*b.x, a.y*b.y}; return c;}
Vec2i64 div_2i64(Vec2i64 a, Vec2i64 b)                 {Vec2i64 c = {a.x/b.x, a.y/b.y}; return c;}
Vec2i64 scale_2i64(Vec2i64 v, i64 s)                   {Vec2i64 c = {v.x*s, v.y*s}; return c;}
i64 dot_2i64(Vec2i64 a, Vec2i64 b)                     {i64 c = a.x*b.x + a.y*b.y; return c;}
i64 length_squared_2i64(Vec2i64 v)                     {i64 c = v.x*v.x + v.y*v.y; return c;}
i64 length_2i64(Vec2i64 v)                             {i64 c = (i64)sqrt_f64((f64)(v.x*v.x + v.y*v.y)); return c;}
Vec2i64 mix_2i64(Vec2i64 a, Vec2i64 b, f32 t)          {Vec2i64 c = {(i64)mix_1f32((f32)a.x, (f32)b.x, t), (i64)mix_1f32((f32)a.y, (f32)b.y, t)}; return c;}

Vec2i32 vec_2i32(i32 x, i32 y)                         {Vec2i32 v = {x, y}; return v;}
Vec2i32 add_2i32(Vec2i32 a, Vec2i32 b)                 {Vec2i32 c = {a.x+b.x, a.y+b.y}; return c;}
Vec2i32 sub_2i32(Vec2i32 a, Vec2i32 b)                 {Vec2i32 c = {a.x-b.x, a.y-b.y}; return c;}
Vec2i32 mul_2i32(Vec2i32 a, Vec2i32 b)                 {Vec2i32 c = {a.x*b.x, a.y*b.y}; return c;}
Vec2i32 div_2i32(Vec2i32 a, Vec2i32 b)                 {Vec2i32 c = {a.x/b.x, a.y/b.y}; return c;}
Vec2i32 scale_2i32(Vec2i32 v, i32 s)                   {Vec2i32 c = {v.x*s, v.y*s}; return c;}
i32 dot_2i32(Vec2i32 a, Vec2i32 b)                     {i32 c = a.x*b.x + a.y*b.y; return c;}
i32 length_squared_2i32(Vec2i32 v)                     {i32 c = v.x*v.x + v.y*v.y; return c;}
i32 length_2i32(Vec2i32 v)                             {i32 c = (i32)sqrt_f32((f32)v.x*(f32)v.x + (f32)v.y*(f32)v.y); return c;}
Vec2i32 mix_2i32(Vec2i32 a, Vec2i32 b, f32 t)          {Vec2i32 c = {(i32)mix_1f32((f32)a.x, (f32)b.x, t), (i32)mix_1f32((f32)a.y, (f32)b.y, t)}; return c;}

Vec2i16 vec_2i16(i16 x, i16 y)                         {Vec2i16 v = {x, y}; return v;}
Vec2i16 add_2i16(Vec2i16 a, Vec2i16 b)                 {Vec2i16 c = {(i16)(a.x+b.x), (i16)(a.y+b.y)}; return c;}
Vec2i16 sub_2i16(Vec2i16 a, Vec2i16 b)                 {Vec2i16 c = {(i16)(a.x-b.x), (i16)(a.y-b.y)}; return c;}
Vec2i16 mul_2i16(Vec2i16 a, Vec2i16 b)                 {Vec2i16 c = {(i16)(a.x*b.x), (i16)(a.y*b.y)}; return c;}
Vec2i16 div_2i16(Vec2i16 a, Vec2i16 b)                 {Vec2i16 c = {(i16)(a.x/b.x), (i16)(a.y/b.y)}; return c;}
Vec2i16 scale_2i16(Vec2i16 v, i16 s)                   {Vec2i16 c = {(i16)(v.x*s), (i16)(v.y*s)}; return c;}
i16 dot_2i16(Vec2i16 a, Vec2i16 b)                     {i16 c = a.x*b.x + a.y*b.y; return c;}
i16 length_squared_2i16(Vec2i16 v)                     {i16 c = v.x*v.x + v.y*v.y; return c;}
i16 length_2i16(Vec2i16 v)                             {i16 c = (i16)sqrt_f32((f32)(v.x*v.x + v.y*v.y)); return c;}
Vec2i16 mix_2i16(Vec2i16 a, Vec2i16 b, f32 t)          {Vec2i16 c = {(i16)mix_1f32((f32)a.x, (f32)b.x, t), (i16)mix_1f32((f32)a.y, (f32)b.y, t)}; return c;}

Vec3f32 vec_3f32(f32 x, f32 y, f32 z)                  {Vec3f32 v = {x, y, z}; return v;}
Vec3f32 add_3f32(Vec3f32 a, Vec3f32 b)                 {Vec3f32 c = {a.x+b.x, a.y+b.y, a.z+b.z}; return c;}
Vec3f32 sub_3f32(Vec3f32 a, Vec3f32 b)                 {Vec3f32 c = {a.x-b.x, a.y-b.y, a.z-b.z}; return c;}
Vec3f32 mul_3f32(Vec3f32 a, Vec3f32 b)                 {Vec3f32 c = {a.x*b.x, a.y*b.y, a.z*b.z}; return c;}
Vec3f32 div_3f32(Vec3f32 a, Vec3f32 b)                 {Vec3f32 c = {a.x/b.x, a.y/b.y, a.z/b.z}; return c;}
Vec3f32 scale_3f32(Vec3f32 v, f32 s)                   {Vec3f32 c = {v.x*s, v.y*s, v.z*s}; return c;}
f32 dot_3f32(Vec3f32 a, Vec3f32 b)                     {f32 c = a.x*b.x + a.y*b.y + a.z*b.z; return c;}
f32 length_squared_3f32(Vec3f32 v)                     {f32 c = v.x*v.x + v.y*v.y + v.z*v.z; return c;}
f32 length_3f32(Vec3f32 v)                             {f32 c = sqrt_f32(v.x*v.x + v.y*v.y + v.z*v.z); return c;}
Vec3f32 normalize_3f32(Vec3f32 v)                      {v = scale_3f32(v, 1.f/length_3f32(v)); return v;}
Vec3f32 mix_3f32(Vec3f32 a, Vec3f32 b, f32 t)          {Vec3f32 c = {mix_1f32(a.x, b.x, t), mix_1f32(a.y, b.y, t), mix_1f32(a.z, b.z, t)}; return c;}
Vec3f32 cross_3f32(Vec3f32 a, Vec3f32 b)               {Vec3f32 c = {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; return c;}
Vec3f32 xform_3f32(Vec3f32 v, Mat3x3f32 m)
{
  Vec3f32 result;
  result.x = v.x*m.v[0][0] + v.y*m.v[1][0] + v.z*m.v[2][0];
  result.y = v.x*m.v[0][1] + v.y*m.v[1][1] + v.z*m.v[2][1];
  result.z = v.x*m.v[0][2] + v.y*m.v[1][2] + v.z*m.v[2][2];
  return result;
}

Vec3i32 vec_3i32(i32 x, i32 y, i32 z)                  {Vec3i32 v = {x, y, z}; return v;}
Vec3i32 add_3i32(Vec3i32 a, Vec3i32 b)                 {Vec3i32 c = {a.x+b.x, a.y+b.y, a.z+b.z}; return c;}
Vec3i32 sub_3i32(Vec3i32 a, Vec3i32 b)                 {Vec3i32 c = {a.x-b.x, a.y-b.y, a.z-b.z}; return c;}
Vec3i32 mul_3i32(Vec3i32 a, Vec3i32 b)                 {Vec3i32 c = {a.x*b.x, a.y*b.y, a.z*b.z}; return c;}
Vec3i32 div_3i32(Vec3i32 a, Vec3i32 b)                 {Vec3i32 c = {a.x/b.x, a.y/b.y, a.z/b.z}; return c;}
Vec3i32 scale_3i32(Vec3i32 v, i32 s)                   {Vec3i32 c = {v.x*s, v.y*s, v.z*s}; return c;}
i32 dot_3i32(Vec3i32 a, Vec3i32 b)                     {i32 c = a.x*b.x + a.y*b.y + a.z*b.z; return c;}
i32 length_squared_3i32(Vec3i32 v)                     {i32 c = v.x*v.x + v.y*v.y + v.z*v.z; return c;}
i32 length_3i32(Vec3i32 v)                             {i32 c = (i32)sqrt_f32((f32)(v.x*v.x + v.y*v.y + v.z*v.z)); return c;}
Vec3i32 mix_3i32(Vec3i32 a, Vec3i32 b, f32 t)          {Vec3i32 c = {(i32)mix_1f32((f32)a.x, (f32)b.x, t), (i32)mix_1f32((f32)a.y, (f32)b.y, t), (i32)mix_1f32((f32)a.z, (f32)b.z, t)}; return c;}
Vec3i32 cross_3i32(Vec3i32 a, Vec3i32 b)               {Vec3i32 c = {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; return c;}

Vec4f32 vec_4f32(f32 x, f32 y, f32 z, f32 w)           {Vec4f32 v = {x, y, z, w}; return v;}
Vec4f32 add_4f32(Vec4f32 a, Vec4f32 b)                 {Vec4f32 c = {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; return c;}
Vec4f32 sub_4f32(Vec4f32 a, Vec4f32 b)                 {Vec4f32 c = {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; return c;}
Vec4f32 mul_4f32(Vec4f32 a, Vec4f32 b)                 {Vec4f32 c = {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}; return c;}
Vec4f32 div_4f32(Vec4f32 a, Vec4f32 b)                 {Vec4f32 c = {a.x/b.x, a.y/b.y, a.z/b.z, a.w/b.w}; return c;}
Vec4f32 scale_4f32(Vec4f32 v, f32 s)                   {Vec4f32 c = {v.x*s, v.y*s, v.z*s, v.w*s}; return c;}
f32 dot_4f32(Vec4f32 a, Vec4f32 b)                     {f32 c = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; return c;}
f32 length_squared_4f32(Vec4f32 v)                     {f32 c = v.x*v.x + v.y*v.y + v.z*v.z + v.w*v.w; return c;}
f32 length_4f32(Vec4f32 v)                             {f32 c = sqrt_f32(v.x*v.x + v.y*v.y + v.z*v.z + v.w*v.w); return c;}
Vec4f32 normalize_4f32(Vec4f32 v)                      {v = scale_4f32(v, 1.f/length_4f32(v)); return v;}
Vec4f32 mix_4f32(Vec4f32 a, Vec4f32 b, f32 t)          {Vec4f32 c = {mix_1f32(a.x, b.x, t), mix_1f32(a.y, b.y, t), mix_1f32(a.z, b.z, t), mix_1f32(a.w, b.w, t)}; return c;}

Vec4i32 vec_4i32(i32 x, i32 y, i32 z, i32 w)           {Vec4i32 v = {x, y, z, w}; return v;}
Vec4i32 add_4i32(Vec4i32 a, Vec4i32 b)                 {Vec4i32 c = {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; return c;}
Vec4i32 sub_4i32(Vec4i32 a, Vec4i32 b)                 {Vec4i32 c = {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; return c;}
Vec4i32 mul_4i32(Vec4i32 a, Vec4i32 b)                 {Vec4i32 c = {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}; return c;}
Vec4i32 div_4i32(Vec4i32 a, Vec4i32 b)                 {Vec4i32 c = {a.x/b.x, a.y/b.y, a.z/b.z, a.w/b.w}; return c;}
Vec4i32 scale_4i32(Vec4i32 v, i32 s)                   {Vec4i32 c = {v.x*s, v.y*s, v.z*s, v.w*s}; return c;}
i32 dot_4i32(Vec4i32 a, Vec4i32 b)                     {i32 c = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; return c;}
i32 length_squared_4i32(Vec4i32 v)                     {i32 c = v.x*v.x + v.y*v.y + v.z*v.z + v.w*v.w; return c;}
i32 length_4i32(Vec4i32 v)                             {i32 c = (i32)sqrt_f32((f32)(v.x*v.x + v.y*v.y + v.z*v.z + v.w*v.w)); return c;}
Vec4i32 mix_4i32(Vec4i32 a, Vec4i32 b, f32 t)          {Vec4i32 c = {(i32)mix_1f32((f32)a.x, (f32)b.x, t), (i32)mix_1f32((f32)a.y, (f32)b.y, t), (i32)mix_1f32((f32)a.z, (f32)b.z, t), (i32)mix_1f32((f32)a.w, (f32)b.w, t)}; return c;}

////////////////////////////////
//~ Matrix Ops
Mat3x3f32 mat_3x3f32(f32 diagonal) {
  Mat3x3f32 result = {0};
  result.v[0][0] = diagonal;
  result.v[1][1] = diagonal;
  result.v[2][2] = diagonal;
  return result;
}

Mat3x3f32 make_translate_3x3f32(Vec2f32 delta) {
  Mat3x3f32 mat = mat_3x3f32(1.f);
  mat.v[2][0] = delta.x;
  mat.v[2][1] = delta.y;
  return mat;
}

Mat3x3f32 make_scale_3x3f32(Vec2f32 scale) {
  Mat3x3f32 mat = mat_3x3f32(1.f);
  mat.v[0][0] = scale.x;
  mat.v[1][1] = scale.y;
  return mat;
}

Mat3x3f32 mul_3x3f32(Mat3x3f32 a, Mat3x3f32 b) {
  Mat3x3f32 c = {0};
  for (int j = 0; j < 3; j += 1) {
    for (int i = 0; i < 3; i += 1) {
      c.v[i][j] = (a.v[0][j] * b.v[i][0] + a.v[1][j] * b.v[i][1] +
                   a.v[2][j] * b.v[i][2]);
    }
  }
  return c;
}

Mat4x4f32 mat_4x4f32(f32 diagonal) {
  Mat4x4f32 result = {0};
  result.v[0][0] = diagonal;
  result.v[1][1] = diagonal;
  result.v[2][2] = diagonal;
  result.v[3][3] = diagonal;
  return result;
}

Mat4x4f32 make_translate_4x4f32(Vec3f32 delta) {
  Mat4x4f32 result = mat_4x4f32(1.f);
  result.v[3][0] = delta.x;
  result.v[3][1] = delta.y;
  result.v[3][2] = delta.z;
  return result;
}

Mat4x4f32 make_scale_4x4f32(Vec3f32 scale) {
  Mat4x4f32 result = mat_4x4f32(1.f);
  result.v[0][0] = scale.x;
  result.v[1][1] = scale.y;
  result.v[2][2] = scale.z;
  return result;
}

Mat4x4f32 make_perspective_4x4f32(f32 fov, f32 aspect_ratio, f32 near_z,
                                  f32 far_z) {
  Mat4x4f32 result = mat_4x4f32(1.f);
  f32 tan_theta_over_2 = tan_f32(fov / 2);
  result.v[0][0] = 1.f / tan_theta_over_2;
  result.v[1][1] = aspect_ratio / tan_theta_over_2;
  result.v[2][3] = 1.f;
  result.v[2][2] = -(near_z + far_z) / (near_z - far_z);
  result.v[3][2] = (2.f * near_z * far_z) / (near_z - far_z);
  result.v[3][3] = 0.f;
  return result;
}

Mat4x4f32 make_orthographic_4x4f32(f32 left, f32 right, f32 bottom, f32 top,
                                   f32 near_z, f32 far_z) {
  Mat4x4f32 result = mat_4x4f32(1.f);

  result.v[0][0] = 2.f / (right - left);
  result.v[1][1] = 2.f / (top - bottom);
  result.v[2][2] = 2.f / (far_z - near_z);
  result.v[3][3] = 1.f;

  result.v[3][0] = (left + right) / (left - right);
  result.v[3][1] = (bottom + top) / (bottom - top);
  result.v[3][2] = (near_z + far_z) / (near_z - far_z);

  return result;
}

Mat4x4f32 make_look_at_4x4f32(Vec3f32 eye, Vec3f32 center, Vec3f32 up) {
  Mat4x4f32 result;
  Vec3f32 f = normalize_3f32(sub_3f32(eye, center));
  Vec3f32 s = normalize_3f32(cross_3f32(f, up));
  Vec3f32 u = cross_3f32(s, f);
  result.v[0][0] = s.x;
  result.v[0][1] = u.x;
  result.v[0][2] = -f.x;
  result.v[0][3] = 0.0f;
  result.v[1][0] = s.y;
  result.v[1][1] = u.y;
  result.v[1][2] = -f.y;
  result.v[1][3] = 0.0f;
  result.v[2][0] = s.z;
  result.v[2][1] = u.z;
  result.v[2][2] = -f.z;
  result.v[2][3] = 0.0f;
  result.v[3][0] = -dot_3f32(s, eye);
  result.v[3][1] = -dot_3f32(u, eye);
  result.v[3][2] = dot_3f32(f, eye);
  result.v[3][3] = 1.0f;
  return result;
}

Mat4x4f32 make_rotate_4x4f32(Vec3f32 axis, f32 turns) {
  Mat4x4f32 result = mat_4x4f32(1.f);
  axis = normalize_3f32(axis);
  f32 sin_theta = sin_f32(turns);
  f32 cos_theta = cos_f32(turns);
  f32 cos_value = 1.f - cos_theta;
  result.v[0][0] = (axis.x * axis.x * cos_value) + cos_theta;
  result.v[0][1] = (axis.x * axis.y * cos_value) + (axis.z * sin_theta);
  result.v[0][2] = (axis.x * axis.z * cos_value) - (axis.y * sin_theta);
  result.v[1][0] = (axis.y * axis.x * cos_value) - (axis.z * sin_theta);
  result.v[1][1] = (axis.y * axis.y * cos_value) + cos_theta;
  result.v[1][2] = (axis.y * axis.z * cos_value) + (axis.x * sin_theta);
  result.v[2][0] = (axis.z * axis.x * cos_value) + (axis.y * sin_theta);
  result.v[2][1] = (axis.z * axis.y * cos_value) - (axis.x * sin_theta);
  result.v[2][2] = (axis.z * axis.z * cos_value) + cos_theta;
  return result;
}

Mat4x4f32 mul_4x4f32(Mat4x4f32 a, Mat4x4f32 b) {
  Mat4x4f32 c = {0};
  for (int j = 0; j < 4; j += 1) {
    for (int i = 0; i < 4; i += 1) {
      c.v[i][j] = (a.v[0][j] * b.v[i][0] + a.v[1][j] * b.v[i][1] +
                   a.v[2][j] * b.v[i][2] + a.v[3][j] * b.v[i][3]);
    }
  }
  return c;
}

Mat4x4f32 scale_4x4f32(Mat4x4f32 m, f32 scale) {
  for (int j = 0; j < 4; j += 1) {
    for (int i = 0; i < 4; i += 1) {
      m.v[i][j] *= scale;
    }
  }
  return m;
}

Mat4x4f32 inverse_4x4f32(Mat4x4f32 m) {
  f32 coef00 = m.v[2][2] * m.v[3][3] - m.v[3][2] * m.v[2][3];
  f32 coef02 = m.v[1][2] * m.v[3][3] - m.v[3][2] * m.v[1][3];
  f32 coef03 = m.v[1][2] * m.v[2][3] - m.v[2][2] * m.v[1][3];
  f32 coef04 = m.v[2][1] * m.v[3][3] - m.v[3][1] * m.v[2][3];
  f32 coef06 = m.v[1][1] * m.v[3][3] - m.v[3][1] * m.v[1][3];
  f32 coef07 = m.v[1][1] * m.v[2][3] - m.v[2][1] * m.v[1][3];
  f32 coef08 = m.v[2][1] * m.v[3][2] - m.v[3][1] * m.v[2][2];
  f32 coef10 = m.v[1][1] * m.v[3][2] - m.v[3][1] * m.v[1][2];
  f32 coef11 = m.v[1][1] * m.v[2][2] - m.v[2][1] * m.v[1][2];
  f32 coef12 = m.v[2][0] * m.v[3][3] - m.v[3][0] * m.v[2][3];
  f32 coef14 = m.v[1][0] * m.v[3][3] - m.v[3][0] * m.v[1][3];
  f32 coef15 = m.v[1][0] * m.v[2][3] - m.v[2][0] * m.v[1][3];
  f32 coef16 = m.v[2][0] * m.v[3][2] - m.v[3][0] * m.v[2][2];
  f32 coef18 = m.v[1][0] * m.v[3][2] - m.v[3][0] * m.v[1][2];
  f32 coef19 = m.v[1][0] * m.v[2][2] - m.v[2][0] * m.v[1][2];
  f32 coef20 = m.v[2][0] * m.v[3][1] - m.v[3][0] * m.v[2][1];
  f32 coef22 = m.v[1][0] * m.v[3][1] - m.v[3][0] * m.v[1][1];
  f32 coef23 = m.v[1][0] * m.v[2][1] - m.v[2][0] * m.v[1][1];

  Vec4f32 fac0 = {coef00, coef00, coef02, coef03};
  Vec4f32 fac1 = {coef04, coef04, coef06, coef07};
  Vec4f32 fac2 = {coef08, coef08, coef10, coef11};
  Vec4f32 fac3 = {coef12, coef12, coef14, coef15};
  Vec4f32 fac4 = {coef16, coef16, coef18, coef19};
  Vec4f32 fac5 = {coef20, coef20, coef22, coef23};

  Vec4f32 vec0 = {m.v[1][0], m.v[0][0], m.v[0][0], m.v[0][0]};
  Vec4f32 vec1 = {m.v[1][1], m.v[0][1], m.v[0][1], m.v[0][1]};
  Vec4f32 vec2 = {m.v[1][2], m.v[0][2], m.v[0][2], m.v[0][2]};
  Vec4f32 vec3 = {m.v[1][3], m.v[0][3], m.v[0][3], m.v[0][3]};

  Vec4f32 inv0 = add_4f32(sub_4f32(mul_4f32(vec1, fac0), mul_4f32(vec2, fac1)),
                          mul_4f32(vec3, fac2));
  Vec4f32 inv1 = add_4f32(sub_4f32(mul_4f32(vec0, fac0), mul_4f32(vec2, fac3)),
                          mul_4f32(vec3, fac4));
  Vec4f32 inv2 = add_4f32(sub_4f32(mul_4f32(vec0, fac1), mul_4f32(vec1, fac3)),
                          mul_4f32(vec3, fac5));
  Vec4f32 inv3 = add_4f32(sub_4f32(mul_4f32(vec0, fac2), mul_4f32(vec1, fac4)),
                          mul_4f32(vec2, fac5));

  Vec4f32 sign_a = {+1, -1, +1, -1};
  Vec4f32 sign_b = {-1, +1, -1, +1};

  Mat4x4f32 inverse;
  for (u32 i = 0; i < 4; i += 1) {
    inverse.v[0][i] = inv0.v[i] * sign_a.v[i];
    inverse.v[1][i] = inv1.v[i] * sign_b.v[i];
    inverse.v[2][i] = inv2.v[i] * sign_a.v[i];
    inverse.v[3][i] = inv3.v[i] * sign_b.v[i];
  }

  Vec4f32 row0 = {inverse.v[0][0], inverse.v[1][0], inverse.v[2][0],
                  inverse.v[3][0]};
  Vec4f32 m0 = {m.v[0][0], m.v[0][1], m.v[0][2], m.v[0][3]};
  Vec4f32 dot0 = mul_4f32(m0, row0);
  f32 dot1 = (dot0.x + dot0.y) + (dot0.z + dot0.w);

  f32 one_over_det = 1 / dot1;

  return scale_4x4f32(inverse, one_over_det);
}

Mat4x4f32 derotate_4x4f32(Mat4x4f32 mat) {
  Vec3f32 scale = {
      length_3f32(v3f32(mat.v[0][0], mat.v[0][1], mat.v[0][2])),
      length_3f32(v3f32(mat.v[1][0], mat.v[1][1], mat.v[1][2])),
      length_3f32(v3f32(mat.v[2][0], mat.v[2][1], mat.v[2][2])),
  };
  mat.v[0][0] = scale.x;
  mat.v[1][0] = 0.f;
  mat.v[2][0] = 0.f;
  mat.v[0][1] = 0.f;
  mat.v[1][1] = scale.y;
  mat.v[2][1] = 0.f;
  mat.v[0][2] = 0.f;
  mat.v[1][2] = 0.f;
  mat.v[2][2] = scale.z;
  return mat;
}

Mat4x4f32 transpose_4x4f32(Mat4x4f32 mat) {
  Mat4x4f32 result = {{
      mat.v[0][0],
      mat.v[1][0],
      mat.v[2][0],
      mat.v[3][0],
      mat.v[0][1],
      mat.v[1][1],
      mat.v[2][1],
      mat.v[3][1],
      mat.v[0][2],
      mat.v[1][2],
      mat.v[2][2],
      mat.v[3][2],
      mat.v[0][3],
      mat.v[1][3],
      mat.v[2][3],
      mat.v[3][3],
  }};
  return result;
}

////////////////////////////////
//~ Range Ops

Rng1u32
rng_1u32(u32 min, u32 max) {
  Rng1u32 r = {min, max};
  if (r.min > r.max) {
    Swap(u32, r.min, r.max);
  }
  return r;
}
Rng1u32
shift_1u32(Rng1u32 r, u32 x) {
  r.min += x;
  r.max += x;
  return r;
}
Rng1u32
pad_1u32(Rng1u32 r, u32 x) {
  r.min -= x;
  r.max += x;
  return r;
}
u32
center_1u32(Rng1u32 r) {
  u32 c = (r.min + r.max) / 2;
  return c;
}
b32
contains_1u32(Rng1u32 r, u32 x) {
  b32 c = (r.min <= x && x < r.max);
  return c;
}
u32
dim_1u32(Rng1u32 r) {
  u32 c = ((r.max > r.min) ? (r.max - r.min) : 0);
  return c;
}
Rng1u32
union_1u32(Rng1u32 a, Rng1u32 b) {
  Rng1u32 c = {Min(a.min, b.min), Max(a.max, b.max)};
  return c;
}
Rng1u32
intersect_1u32(Rng1u32 a, Rng1u32 b) {
  Rng1u32 c = {Max(a.min, b.min), Min(a.max, b.max)};
  return c;
}
u32
clamp_1u32(Rng1u32 r, u32 v) {
  v = Clamp(r.min, v, r.max);
  return v;
}

Rng1i32
rng_1i32(i32 min, i32 max) {
  Rng1i32 r = {min, max};
  if (r.min > r.max) {
    Swap(i32, r.min, r.max);
  }
  return r;
}
Rng1i32
shift_1i32(Rng1i32 r, i32 x) {
  r.min += x;
  r.max += x;
  return r;
}
Rng1i32
pad_1i32(Rng1i32 r, i32 x) {
  r.min -= x;
  r.max += x;
  return r;
}
i32
center_1i32(Rng1i32 r) {
  i32 c = (r.min + r.max) / 2;
  return c;
}
b32
contains_1i32(Rng1i32 r, i32 x) {
  b32 c = (r.min <= x && x < r.max);
  return c;
}
i32
dim_1i32(Rng1i32 r) {
  i32 c = ((r.max > r.min) ? (r.max - r.min) : 0);
  return c;
}
Rng1i32
union_1i32(Rng1i32 a, Rng1i32 b) {
  Rng1i32 c = {Min(a.min, b.min), Max(a.max, b.max)};
  return c;
}
Rng1i32
intersect_1i32(Rng1i32 a, Rng1i32 b) {
  Rng1i32 c = {Max(a.min, b.min), Min(a.max, b.max)};
  return c;
}
i32
clamp_1i32(Rng1i32 r, i32 v) {
  v = Clamp(r.min, v, r.max);
  return v;
}

Rng1u64
rng_1u64(u64 min, u64 max) {
  Rng1u64 r = {min, max};
  if (r.min > r.max) {
    Swap(u64, r.min, r.max);
  }
  return r;
}
Rng1u64
shift_1u64(Rng1u64 r, u64 x) {
  r.min += x;
  r.max += x;
  return r;
}
Rng1u64
pad_1u64(Rng1u64 r, u64 x) {
  r.min -= x;
  r.max += x;
  return r;
}
u64
center_1u64(Rng1u64 r) {
  u64 c = (r.min + r.max) / 2;
  return c;
}
b32
contains_1u64(Rng1u64 r, u64 x) {
  b32 c = (r.min <= x && x < r.max);
  return c;
}
u64
dim_1u64(Rng1u64 r) {
  u64 c = ((r.max > r.min) ? (r.max - r.min) : 0);
  return c;
}
Rng1u64
union_1u64(Rng1u64 a, Rng1u64 b) {
  Rng1u64 c = {Min(a.min, b.min), Max(a.max, b.max)};
  return c;
}
Rng1u64
intersect_1u64(Rng1u64 a, Rng1u64 b) {
  Rng1u64 c = {Max(a.min, b.min), Min(a.max, b.max)};
  return c;
}
u64
clamp_1u64(Rng1u64 r, u64 v) {
  v = Clamp(r.min, v, r.max);
  return v;
}

Rng1i64
rng_1i64(i64 min, i64 max) {
  Rng1i64 r = {min, max};
  if (r.min > r.max) {
    Swap(i64, r.min, r.max);
  }
  return r;
}
Rng1i64
shift_1i64(Rng1i64 r, i64 x) {
  r.min += x;
  r.max += x;
  return r;
}
Rng1i64
pad_1i64(Rng1i64 r, i64 x) {
  r.min -= x;
  r.max += x;
  return r;
}
i64
center_1i64(Rng1i64 r) {
  i64 c = (r.min + r.max) / 2;
  return c;
}
b32
contains_1i64(Rng1i64 r, i64 x) {
  b32 c = (r.min <= x && x < r.max);
  return c;
}
i64
dim_1i64(Rng1i64 r) {
  i64 c = ((r.max > r.min) ? (r.max - r.min) : 0);
  return c;
}
Rng1i64
union_1i64(Rng1i64 a, Rng1i64 b) {
  Rng1i64 c = {Min(a.min, b.min), Max(a.max, b.max)};
  return c;
}
Rng1i64
intersect_1i64(Rng1i64 a, Rng1i64 b) {
  Rng1i64 c = {Max(a.min, b.min), Min(a.max, b.max)};
  return c;
}
i64
clamp_1i64(Rng1i64 r, i64 v) {
  v = Clamp(r.min, v, r.max);
  return v;
}

Rng1f32
rng_1f32(f32 min, f32 max) {
  Rng1f32 r = {min, max};
  if (r.min > r.max) {
    Swap(f32, r.min, r.max);
  }
  return r;
}
Rng1f32
shift_1f32(Rng1f32 r, f32 x) {
  r.min += x;
  r.max += x;
  return r;
}
Rng1f32
pad_1f32(Rng1f32 r, f32 x) {
  r.min -= x;
  r.max += x;
  return r;
}
f32
center_1f32(Rng1f32 r) {
  f32 c = (r.min + r.max) / 2;
  return c;
}
b32
contains_1f32(Rng1f32 r, f32 x) {
  b32 c = (r.min <= x && x < r.max);
  return c;
}
f32
dim_1f32(Rng1f32 r) {
  f32 c = ((r.max > r.min) ? (r.max - r.min) : 0);
  return c;
}
Rng1f32
union_1f32(Rng1f32 a, Rng1f32 b) {
  Rng1f32 c = {Min(a.min, b.min), Max(a.max, b.max)};
  return c;
}
Rng1f32
intersect_1f32(Rng1f32 a, Rng1f32 b) {
  Rng1f32 c = {Max(a.min, b.min), Min(a.max, b.max)};
  return c;
}
f32
clamp_1f32(Rng1f32 r, f32 v) {
  v = Clamp(r.min, v, r.max);
  return v;
}

Rng2i16
rng_2i16(Vec2i16 min, Vec2i16 max) {
  Rng2i16 r = {min, max};
  return r;
}
Rng2i16
shift_2i16(Rng2i16 r, Vec2i16 x) {
  r.min = add_2i16(r.min, x);
  r.max = add_2i16(r.max, x);
  return r;
}
Rng2i16
pad_2i16(Rng2i16 r, i16 x) {
  Vec2i16 xv = {x, x};
  r.min      = sub_2i16(r.min, xv);
  r.max      = add_2i16(r.max, xv);
  return r;
}
Vec2i16
center_2i16(Rng2i16 r) {
  Vec2i16 c = {(i16)((r.min.x + r.max.x) / 2), (i16)((r.min.y + r.max.y) / 2)};
  return c;
}
b32
contains_2i16(Rng2i16 r, Vec2i16 x) {
  b32 c = (r.min.x <= x.x && x.x < r.max.x && r.min.y <= x.y && x.y < r.max.y);
  return c;
}
Vec2i16
dim_2i16(Rng2i16 r) {
  Vec2i16 dim = {(i16)(((r.max.x > r.min.x) ? (r.max.x - r.min.x) : 0)),
                 (i16)(((r.max.y > r.min.y) ? (r.max.y - r.min.y) : 0))};
  return dim;
}
Rng2i16
union_2i16(Rng2i16 a, Rng2i16 b) {
  Rng2i16 c;
  c.p0.x = Min(a.min.x, b.min.x);
  c.p0.y = Min(a.min.y, b.min.y);
  c.p1.x = Max(a.max.x, b.max.x);
  c.p1.y = Max(a.max.y, b.max.y);
  return c;
}
Rng2i16
intersect_2i16(Rng2i16 a, Rng2i16 b) {
  Rng2i16 c;
  c.p0.x = Max(a.min.x, b.min.x);
  c.p0.y = Max(a.min.y, b.min.y);
  c.p1.x = Min(a.max.x, b.max.x);
  c.p1.y = Min(a.max.y, b.max.y);
  return c;
}
Vec2i16
clamp_2i16(Rng2i16 r, Vec2i16 v) {
  v.x = Clamp(r.min.x, v.x, r.max.x);
  v.y = Clamp(r.min.y, v.y, r.max.y);
  return v;
}

Rng2i32
rng_2i32(Vec2i32 min, Vec2i32 max) {
  Rng2i32 r = {min, max};
  return r;
}
Rng2i32
shift_2i32(Rng2i32 r, Vec2i32 x) {
  r.min = add_2i32(r.min, x);
  r.max = add_2i32(r.max, x);
  return r;
}
Rng2i32
pad_2i32(Rng2i32 r, i32 x) {
  Vec2i32 xv = {x, x};
  r.min      = sub_2i32(r.min, xv);
  r.max      = add_2i32(r.max, xv);
  return r;
}
Vec2i32
center_2i32(Rng2i32 r) {
  Vec2i32 c = {(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2};
  return c;
}
b32
contains_2i32(Rng2i32 r, Vec2i32 x) {
  b32 c = (r.min.x <= x.x && x.x < r.max.x && r.min.y <= x.y && x.y < r.max.y);
  return c;
}
Vec2i32
dim_2i32(Rng2i32 r) {
  Vec2i32 dim = {((r.max.x > r.min.x) ? (r.max.x - r.min.x) : 0),
                 ((r.max.y > r.min.y) ? (r.max.y - r.min.y) : 0)};
  return dim;
}
Rng2i32
union_2i32(Rng2i32 a, Rng2i32 b) {
  Rng2i32 c;
  c.p0.x = Min(a.min.x, b.min.x);
  c.p0.y = Min(a.min.y, b.min.y);
  c.p1.x = Max(a.max.x, b.max.x);
  c.p1.y = Max(a.max.y, b.max.y);
  return c;
}
Rng2i32
intersect_2i32(Rng2i32 a, Rng2i32 b) {
  Rng2i32 c;
  c.p0.x = Max(a.min.x, b.min.x);
  c.p0.y = Max(a.min.y, b.min.y);
  c.p1.x = Min(a.max.x, b.max.x);
  c.p1.y = Min(a.max.y, b.max.y);
  return c;
}
Vec2i32
clamp_2i32(Rng2i32 r, Vec2i32 v) {
  v.x = Clamp(r.min.x, v.x, r.max.x);
  v.y = Clamp(r.min.y, v.y, r.max.y);
  return v;
}

Rng2i64
rng_2i64(Vec2i64 min, Vec2i64 max) {
  Rng2i64 r = {min, max};
  return r;
}
Rng2i64
shift_2i64(Rng2i64 r, Vec2i64 x) {
  r.min = add_2i64(r.min, x);
  r.max = add_2i64(r.max, x);
  return r;
}
Rng2i64
pad_2i64(Rng2i64 r, i64 x) {
  Vec2i64 xv = {x, x};
  r.min      = sub_2i64(r.min, xv);
  r.max      = add_2i64(r.max, xv);
  return r;
}
Vec2i64
center_2i64(Rng2i64 r) {
  Vec2i64 c = {(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2};
  return c;
}
b32
contains_2i64(Rng2i64 r, Vec2i64 x) {
  b32 c = (r.min.x <= x.x && x.x < r.max.x && r.min.y <= x.y && x.y < r.max.y);
  return c;
}
Vec2i64
dim_2i64(Rng2i64 r) {
  Vec2i64 dim = {((r.max.x > r.min.x) ? (r.max.x - r.min.x) : 0),
                 ((r.max.y > r.min.y) ? (r.max.y - r.min.y) : 0)};
  return dim;
}
Rng2i64
union_2i64(Rng2i64 a, Rng2i64 b) {
  Rng2i64 c;
  c.p0.x = Min(a.min.x, b.min.x);
  c.p0.y = Min(a.min.y, b.min.y);
  c.p1.x = Max(a.max.x, b.max.x);
  c.p1.y = Max(a.max.y, b.max.y);
  return c;
}
Rng2i64
intersect_2i64(Rng2i64 a, Rng2i64 b) {
  Rng2i64 c;
  c.p0.x = Max(a.min.x, b.min.x);
  c.p0.y = Max(a.min.y, b.min.y);
  c.p1.x = Min(a.max.x, b.max.x);
  c.p1.y = Min(a.max.y, b.max.y);
  return c;
}
Vec2i64
clamp_2i64(Rng2i64 r, Vec2i64 v) {
  v.x = Clamp(r.min.x, v.x, r.max.x);
  v.y = Clamp(r.min.y, v.y, r.max.y);
  return v;
}

Rng2f32
rng_2f32(Vec2f32 min, Vec2f32 max) {
  Rng2f32 r = {min, max};
  return r;
}
Rng2f32
shift_2f32(Rng2f32 r, Vec2f32 x) {
  r.min = add_2f32(r.min, x);
  r.max = add_2f32(r.max, x);
  return r;
}
Rng2f32
pad_2f32(Rng2f32 r, f32 x) {
  Vec2f32 xv = {x, x};
  r.min      = sub_2f32(r.min, xv);
  r.max      = add_2f32(r.max, xv);
  return r;
}
Vec2f32
center_2f32(Rng2f32 r) {
  Vec2f32 c = {(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2};
  return c;
}
b32
contains_2f32(Rng2f32 r, Vec2f32 x) {
  b32 c = (r.min.x <= x.x && x.x < r.max.x && r.min.y <= x.y && x.y < r.max.y);
  return c;
}
Vec2f32
dim_2f32(Rng2f32 r) {
  Vec2f32 dim = {((r.max.x > r.min.x) ? (r.max.x - r.min.x) : 0),
                 ((r.max.y > r.min.y) ? (r.max.y - r.min.y) : 0)};
  return dim;
}
Rng2f32
union_2f32(Rng2f32 a, Rng2f32 b) {
  Rng2f32 c;
  c.p0.x = Min(a.min.x, b.min.x);
  c.p0.y = Min(a.min.y, b.min.y);
  c.p1.x = Max(a.max.x, b.max.x);
  c.p1.y = Max(a.max.y, b.max.y);
  return c;
}
Rng2f32
intersect_2f32(Rng2f32 a, Rng2f32 b) {
  Rng2f32 c;
  c.p0.x = Max(a.min.x, b.min.x);
  c.p0.y = Max(a.min.y, b.min.y);
  c.p1.x = Min(a.max.x, b.max.x);
  c.p1.y = Min(a.max.y, b.max.y);
  return c;
}
Vec2f32
clamp_2f32(Rng2f32 r, Vec2f32 v) {
  v.x = Clamp(r.min.x, v.x, r.max.x);
  v.y = Clamp(r.min.y, v.y, r.max.y);
  return v;
}

////////////////////////////////
//~ List Type Functions

void
rng1u64_list_push(Arena* arena, Rng1u64List* list, Rng1u64 rng) {
  Rng1u64Node* n = push_one(arena, Rng1u64Node);
  MemoryCopyStruct(&n->v, &rng);
  SLLQueuePush(list->first, list->last, n);
  list->count += 1;
}

void
rng1u64_list_concat(Rng1u64List* list, Rng1u64List* to_concat) {
  if (to_concat->first) {
    if (list->first) {
      list->last->next = to_concat->first;
      list->last       = to_concat->last;
    } else {
      list->first = to_concat->first;
      list->last  = to_concat->last;
    }
    MemoryZeroStruct(to_concat);
  }
}

Rng1u64Array
rng1u64_array_from_list(Arena* arena, Rng1u64List* list) {
  Rng1u64Array arr = {0};
  arr.count        = list->count;
  arr.v            = push_array(arena, Rng1u64, arr.count);
  u64 idx          = 0;
  for (Rng1u64Node* n = list->first; n != 0; n = n->next) {
    arr.v[idx] = n->v;
    idx += 1;
  }
  return arr;
}

u64
rng_1u64_array_bsearch(Rng1u64Array arr, u64 value) {
  if (arr.count > 0 && arr.v[0].min < value &&
      value < arr.v[arr.count - 1].max) {
    u64 l = 0;
    u64 r = arr.count - 1;
    for (; l <= r;) {
      u64 m = l + (r - l) / 2;
      if (contains_1u64(arr.v[m], value)) {
        return m;
      } else if (arr.v[m].min < value) {
        l = m + 1;
      } else {
        r = m - 1;
      }
    }
  } else if (arr.count == 1 && contains_1u64(arr.v[0], value)) {
    return 0;
  }
  return max_u64;
}

void
rng1i64_list_push(Arena* arena, Rng1i64List* list, Rng1i64 rng) {
  Rng1i64Node* n = push_one(arena, Rng1i64Node);
  MemoryCopyStruct(&n->v, &rng);
  SLLQueuePush(list->first, list->last, n);
  list->count += 1;
}

Rng1i64Array
rng1i64_array_from_list(Arena* arena, Rng1i64List* list) {
  Rng1i64Array arr = {0};
  arr.count        = list->count;
  arr.v            = push_array(arena, Rng1i64, arr.count);
  u64 idx          = 0;
  for (Rng1i64Node* n = list->first; n != 0; n = n->next) {
    arr.v[idx] = n->v;
    idx += 1;
  }
  return arr;
}


// Scratch uses a virtual memory backend.
// Perm uses a virtual memory backend with a default capacity of 512MB.
void
ctx_init(Context* ctx, u64 scratch_size) {
  assert(ctx && "Context is NULL");
  assert(scratch_size > 0 && "Scratch size is zero");
  MemoryZeroStruct(ctx);

  ctx->scratch = arena_create_vm(scratch_size);
  ctx->perm    = arena_create_vm(MB(512));
  ctx_enter(ctx);
}

// Scratch uses a virtual memory backend.
// Perm uses a heap backend.
// If perm_size is too small, fall back to the minimum arena block size
void
ctx_init_with_perm(Context* ctx, u64 scratch_size, u64 perm_size) {
  assert(ctx && "Context is NULL");
  assert(scratch_size > 0 && "Scratch size is zero");
  assert(perm_size > 0 && "Perm size is zero");
  MemoryZeroStruct(ctx);

  ctx->scratch = arena_create_vm(scratch_size);
  ctx->perm    = arena_create_heap(perm_size);
  ctx_enter(ctx);
}

void
ctx_enter(Context* ctx) {
#if !COMPILER_TCC
  if (ctx) {
    tls_ctx = ctx;
  } else {
    if (!tls_ctx) tls_ctx = g_ctx;
  }
#else
  g_ctx = ctx;
#endif
}

void
ctx_leave_and_reset(void) {
  Context* ctx = ctx_current();
  if (!ctx) return;

  arena_reset(&ctx->scratch);
#if !COMPILER_TCC
  tls_ctx = NULL;
#else
  g_ctx = NULL;
#endif
}

void
ctx_leave_and_preserve(void) {
  Context* ctx = ctx_current();
  if (!ctx) return;
#if !COMPILER_TCC
  tls_ctx = NULL;
#else
  g_ctx = NULL;
#endif
}

void
ctx_leave_and_release(void) {
  Context* ctx = ctx_current();
  if (!ctx) return;
  arena_release(&ctx->scratch);
#if !COMPILER_TCC
  tls_ctx = NULL;
#else
  g_ctx = NULL;
#endif
}

void
ctx_free(void) {
  Context* ctx = ctx_current();
  if (!ctx) return;
  arena_destroy(&ctx->scratch);
  arena_destroy(&ctx->perm);
#if !COMPILER_TCC
  tls_ctx = NULL;
#else
  g_ctx = NULL;
#endif
}

void ctx_set_global(Context* ctx) {
  if (!ctx) printf("ctx_set_global(): Setting global context to NULL\n");
  g_ctx = ctx;
}

Context*
ctx_global(void) {
  return g_ctx;
}

Context*
ctx_current(void) {
#if !COMPILER_TCC
  return tls_ctx ? tls_ctx : g_ctx;
#else
  return g_ctx;
#endif
}

Arena*
ctx_scratch(void) {
#if !COMPILER_TCC
  ThreadContext* tctx = tctx_current();
  if (tctx) return tctx_scratch();
#endif
  Context* ctx = ctx_current();
  if (ctx) return &ctx->scratch;
  if (g_ctx) return &g_ctx->scratch;

  return NULL;
}

Arena*
ctx_perm(void) {
#if !COMPILER_TCC
  ThreadContext* tctx = tctx_current();
  if (tctx) return &tctx->perm;
#endif
  Context* ctx = ctx_current();
  if (ctx) return &ctx->perm;
  if (g_ctx) return &g_ctx->perm;

  return NULL;
}


b32
char_is_space(u8 c) {
  return (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f' || c == '\v');
}

b32
char_is_upper(u8 c) {
  return ('A' <= c && c <= 'Z');
}

b32
char_is_lower(u8 c) {
  return ('a' <= c && c <= 'z');
}

b32
char_is_alpha(u8 c) {
  return (char_is_upper(c) || char_is_lower(c));
}

b32
char_is_slash(u8 c) {
  return (c == '/' || c == '\\');
}

b32
char_is_digit(u8 c, u32 base) {
  b32 result = 0;
  if (0 < base && base <= 16 && c < 0x80) {
    u8 val = integer_symbol_reverse[c];
    if (val < base) {
      result = 1;
    }
  }
  return result;
}

u8
char_to_lower(u8 c) {
  if (char_is_upper(c)) {
    c += ('a' - 'A');
  }
  return c;
}

u8
char_to_upper(u8 c) {
  if (char_is_lower(c)) {
    c += ('A' - 'a');
  }
  return c;
}

u8
char_to_correct_slash(u8 c) {
  if (char_is_slash(c)) {
    c = '/';
  }
  return c;
}

u64
cstring8_length(u8* c) {
  u8* p = c;
  for (; *p != 0; p += 1);
  return (p - c);
}

u64
cstring16_length(u16* c) {
  u16* p = c;
  for (; *p != 0; p += 1);
  return (p - c);
}

u64
cstring32_length(u32* c) {
  u32* p = c;
  for (; *p != 0; p += 1);
  return (p - c);
}

void
ss_init(Arena* a, SmallString* s) {
  sv_u8_init(&s->v);
  sv_u8_push(a, &s->v, 0);
}

u64
ss_len(SmallString* s) {
  return (s->v.count == 0 ? 0 : s->v.count - 1);
}

u8*
ss_cstr(SmallString* s) {
  return s->v.items;
}

void
ss_clear(Arena* a, SmallString* s) {
  s->v.count = 0;
  sv_u8_push(a, &s->v, 0);
}

void
ss_push_u8(Arena* a, SmallString* s, u8 c) {
  // overwrite null terminator
  if (s->v.count > 0) s->v.count--;

  sv_u8_push(a, &s->v, c);
  sv_u8_push(a, &s->v, 0); // restore null terminator
}

void
ss_append(Arena* a, SmallString* s, String8 str) {
  // overwrite null terminator
  if (s->v.count > 0) s->v.count--;

  foreach_index(i, str.size) {
    sv_u8_push(a, &s->v, str.str[i]);
  }

  sv_u8_push(a, &s->v, 0); // restore null terminator
}

void
ss_append_cstr(Arena* a, SmallString* s, const char* cstr) {
  if (s->v.count > 0) s->v.count--;

  while (*cstr) {
    sv_u8_push(a, &s->v, (u8)*cstr++);
  }

  sv_u8_push(a, &s->v, 0);
}

b32
ss_match(SmallString a, SmallString b, StringMatchFlags flags) {
  b32 result = 0;
  if (a.v.count == b.v.count && flags == 0) {
    result = MemoryMatch(a.v.items, b.v.items, b.v.count);
  } else if (a.v.count == b.v.count || (flags & StringMatchFlag_RightSideSloppy)) {
    b32 case_insensitive  = (flags & StringMatchFlag_CaseInsensitive);
    b32 slash_insensitive = (flags & StringMatchFlag_SlashInsensitive);
    u64 size              = Min(a.v.count, b.v.count);
    result                = 1;
    foreach_index(i, size) {
      u8 at = a.v.items[i];
      u8 bt = b.v.items[i];
      if (case_insensitive) {
        at = char_to_upper(at);
        bt = char_to_upper(bt);
      }
      if (slash_insensitive) {
        at = char_to_correct_slash(at);
        bt = char_to_correct_slash(bt);
      }
      if (at != bt) {
        result = 0;
        break;
      }
    }
  }
  return result;
}

String8
ss_str8(SmallString* s) {
  return str8(s->v.items, ss_len(s));
}

void
ss_free(SmallString* s) {
  sv_u8_free(&s->v);
}

String8
str8(u8* str, u64 size) {
  String8 result = { str, size };
  return result;
}

String8
str8_range(u8* first, u8* one_past_last) {
  String8 result = { first, (u64)(one_past_last - first) };
  return result;
}

String8
str8_zero(void) {
  String8 result = { 0 };
  return result;
}

String16
str16(u16* str, u64 size) {
  String16 result = { str, size };
  return result;
}

String16
str16_range(u16* first, u16* one_past_last) {
  String16 result = { first, (u64)(one_past_last - first) };
  return result;
}

String16
str16_zero(void) {
  String16 result = { 0 };
  return result;
}

String32
str32(u32* str, u64 size) {
  String32 result = { str, size };
  return result;
}

String32
str32_range(u32* first, u32* one_past_last) {
  String32 result = { first, (u64)(one_past_last - first) };
  return result;
}

String32
str32_zero(void) {
  String32 result = { 0 };
  return result;
}

String8
str8_cstring(char* c) {
  String8 result = { (u8*)c, cstring8_length((u8*)c) };
  return result;
}

String16
str16_cstring(u16* c) {
  String16 result = { (u16*)c, cstring16_length((u16*)c) };
  return result;
}

String32
str32_cstring(u32* c) {
  String32 result = { (u32*)c, cstring32_length((u32*)c) };
  return result;
}

b32
str8_match(String8 a, String8 b, StringMatchFlags flags) {
  b32 result = 0;
  if (a.size == b.size && flags == 0) {
    result = MemoryMatch(a.str, b.str, b.size);
  } else if (a.size == b.size || (flags & StringMatchFlag_RightSideSloppy)) {
    b32 case_insensitive  = (flags & StringMatchFlag_CaseInsensitive);
    b32 slash_insensitive = (flags & StringMatchFlag_SlashInsensitive);
    u64 size              = Min(a.size, b.size);
    result                = 1;
    foreach_index(i, size) {
      u8 at = a.str[i];
      u8 bt = b.str[i];
      if (case_insensitive) {
        at = char_to_upper(at);
        bt = char_to_upper(bt);
      }
      if (slash_insensitive) {
        at = char_to_correct_slash(at);
        bt = char_to_correct_slash(bt);
      }
      if (at != bt) {
        result = 0;
        break;
      }
    }
  }
  return result;
}

u64
str8_find_needle(String8 string, u64 start_pos, String8 needle,
                 StringMatchFlags flags) {
  u8* p           = string.str + start_pos;
  u64 stop_offset = Max(string.size + 1, needle.size) - needle.size;
  u8* stop_p      = string.str + stop_offset;
  if (needle.size > 0) {
    u8*              string_opl     = string.str + string.size;
    String8          needle_tail    = str8_skip(needle, 1);
    StringMatchFlags adjusted_flags = flags | StringMatchFlag_RightSideSloppy;
    u8               needle_first_char_adjusted = needle.str[0];
    if (adjusted_flags & StringMatchFlag_CaseInsensitive) {
      needle_first_char_adjusted = char_to_upper(needle_first_char_adjusted);
    }
    for (; p < stop_p; p += 1) {
      u8 haystack_char_adjusted = *p;
      if (adjusted_flags & StringMatchFlag_CaseInsensitive) {
        haystack_char_adjusted = char_to_upper(haystack_char_adjusted);
      }
      if (haystack_char_adjusted == needle_first_char_adjusted) {
        if (str8_match(str8_range(p + 1, string_opl), needle_tail,
                       adjusted_flags)) {
          break;
        }
      }
    }
  }
  u64 result = string.size;
  if (p < stop_p) {
    result = (u64)(p - string.str);
  }
  return result;
}

u64
str8_find_needle_reverse(String8 string, u64 start_pos, String8 needle,
                         StringMatchFlags flags) {
  u64 result = 0;
  foreach_index_reverse(i, string.size-start_pos-needle.size) {
    String8 haystack = str8_substr(string, rng_1u64(i, i + needle.size));
    if (str8_match(haystack, needle, flags)) {
      result = (u64)i + needle.size;
      break;
    }
  }
  return result;
}

b32
str8_starts_with(String8 string, String8 start, StringMatchFlags flags) {
  String8 prefix   = str8_prefix(string, start.size);
  b32     is_match = str8_match(start, prefix, flags);
  return is_match;
}

b32
str8_ends_with(String8 string, String8 end, StringMatchFlags flags) {
  String8 postfix  = str8_postfix(string, end.size);
  b32     is_match = str8_match(end, postfix, flags);
  return is_match;
}

String8
str8_substr(String8 str, Rng1u64 range) {
  range.min = ClampTop(range.min, str.size);
  range.max = ClampTop(range.max, str.size);
  str.str += range.min;
  str.size = dim_1u64(range);
  return str;
}

String8
str8_prefix(String8 str, u64 size) {
  str.size = ClampTop(size, str.size);
  return str;
}

String8
str8_skip(String8 str, u64 amt) {
  amt = ClampTop(amt, str.size);
  str.str += amt;
  str.size -= amt;
  return str;
}

String8
str8_postfix(String8 str, u64 size) {
  size     = ClampTop(size, str.size);
  str.str  = (str.str + str.size) - size;
  str.size = size;
  return str;
}

String8
str8_chop(String8 str, u64 amt) {
  amt = ClampTop(amt, str.size);
  str.size -= amt;
  return str;
}

String8
str8_skip_chop_whitespace(String8 string) {
  u8* first = string.str;
  u8* opl   = first + string.size;
  for (; first < opl; first += 1) {
    if (!char_is_space(*first)) {
      break;
    }
  }
  for (; opl > first;) {
    opl -= 1;
    if (!char_is_space(*opl)) {
      opl += 1;
      break;
    }
  }
  String8 result = str8_range(first, opl);
  return result;
}

String8
str8_skip_chop_slashes(String8 string) {
  u8* first = string.str;
  u8* opl   = first + string.size;
  for (; first < opl; first += 1) {
    if (!char_is_slash(*first)) {
      break;
    }
  }
  for (; opl > first;) {
    opl -= 1;
    if (!char_is_slash(*opl)) {
      opl += 1;
      break;
    }
  }
  String8 result = str8_range(first, opl);
  return result;
}

// How much of one string `str8_dbg` will actually print. Escaping can quadruple
// a byte's width on the way out (`\xNN`), so an uncapped dump of a large string
// is megabytes of stderr; what got cut is always reported rather than silently
// dropped.
#define STR8_DEBUG_MAX_BYTES 256

// Backs the `str8_dbg(s)` macro: prints one String8 to stderr with the source
// location and the literal expression that produced it.
//
// Deliberately does NOT go through str8_fmt/str8_varg's plain "%.*s", because
// the whole point of this function is to be pointed at a String8 that is
// malformed, half-built, or not text at all -- and that "%.*s" has three ways
// to make a debugging session worse than no output at all:
//
//   - a NULL `str` with a nonzero `size` dereferences NULL, so the aid you
//     reached for to find the bug is what crashes;
//   - `str8_varg`'s `(int)` cast turns a size past INT_MAX negative, and a
//     negative precision means "no precision at all" to printf, which then
//     runs off the end of the string hunting a terminator that a String8 is
//     under no obligation to have;
//   - raw bytes reach the terminal as-is, so a binary blob can emit escape
//     sequences that garble every line around it.
//
// Hence: NULL is reported instead of printed, output is capped, and anything
// outside printable ASCII is escaped.
String8
str8_debug(const char* file, i32 line, const char* name, String8 s) {
  fprintf(stderr, "[%s:%d] %s = ", file, line, name);

  if (s.str == NULL) {
    // A zero String8 is a normal, valid value, so it reads as <null> rather
    // than as an error; a NULL pointer with a nonzero size is a corrupt one,
    // and saying so beats printing nothing at all.
    fprintf(stderr, "<null> (len=%llu)\n", s.size);
    return s;
  }

  u64 shown = s.size < STR8_DEBUG_MAX_BYTES ? s.size : STR8_DEBUG_MAX_BYTES;
  fputc('"', stderr);
  for (u64 i = 0; i < shown; i += 1) {
    u8 c = s.str[i];
    switch (c) {
      case '\n': fputs("\\n", stderr); break;
      case '\t': fputs("\\t", stderr); break;
      case '\r': fputs("\\r", stderr); break;
      case '"':  fputs("\\\"", stderr); break;
      case '\\': fputs("\\\\", stderr); break;
      default:
        if (c >= 0x20 && c < 0x7f) fputc((char)c, stderr);
        else                       fprintf(stderr, "\\x%02x", (unsigned)c);
        break;
    }
  }
  fputc('"', stderr);

  if (shown < s.size) fprintf(stderr, " ...+%llu more", s.size - shown);
  fprintf(stderr, " (len=%llu)\n", s.size);
  return s;
}

String8
str8_cat(Arena* arena, String8 s1, String8 s2) {
  String8 str;
  str.size = s1.size + s2.size;
  str.str  = push_array(arena, u8, str.size + 1);
  MemoryCopy(str.str, s1.str, s1.size);
  MemoryCopy(str.str + s1.size, s2.str, s2.size);
  str.str[str.size] = 0;
  return str;
}

String8
str8_copy(Arena* arena, String8 s) {
  String8 str;
  str.size = s.size;
  str.str  = push_array(arena, u8, str.size + 1);
  MemoryCopy(str.str, s.str, s.size);
  str.str[str.size] = 0;
  return str;
}

char*
cstring_str8(Arena* arena, String8 s) {
  char* out = push_array(arena, char, s.size + 1);
  MemoryCopy(out, s.str, s.size);
  out[s.size] = 0;
  return out;
}

String8
str8fv(Arena* arena, char* fmt, va_list args) {
  va_list args2;
  va_copy(args2, args);
  // vsnprintf returns NEGATIVE on an output error (an encoding error, per C99);
  // the `+ 1` used to wrap that to 0 in a u32, so nothing was allocated and the
  // second call's own negative return became a huge u64 `.size`, whose
  // `result.str[result.size] = 0` wrote far past the arena. Mirrored in the
  // generated runtime's bbb_str8fv (runtime/bbb_string.c).
  int needed = vsnprintf(0, 0, fmt, args);
  if (needed < 0) { va_end(args2); return (String8){0}; }

  u32     needed_bytes = (u32)needed + 1;
  String8 result       = { 0 };
  result.str           = push_array(arena, u8, needed_bytes);
  result.size          = (u64)needed; // the sizing call above already measured it; re-reading the
                                      // second call's return would reintroduce the same signed wrap
  vsnprintf((char*)result.str, needed_bytes, fmt, args2);
  result.str[result.size] = 0;
  va_end(args2);
  return result;
}

String8
str8f(Arena* arena, char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 result = str8fv(arena, fmt, args);
  va_end(args);
  return result;
}

i64
sign_from_str8(String8 string, String8* string_tail) {
  // count negative signs
  u64 neg_count = 0;
  u64 i         = 0;
  for (; i < string.size; i += 1) {
    if (string.str[i] == '-') {
      neg_count += 1;
    } else if (string.str[i] != '+') {
      break;
    }
  }

  // output part of string after signs
  *string_tail = str8_skip(string, i);

  // output integer sign
  i64 sign = (neg_count & 1) ? -1 : +1;
  return (sign);
}

b32
str8_is_integer(String8 string, u32 radix) {
  b32 result = 0;
  if (string.size > 0) {
    if (1 < radix && radix <= 16) {
      result = 1;
      foreach_index(i, string.size) {
        u8 c = string.str[i];
        if (!(c < 0x80) || integer_symbol_reverse[c] >= radix) {
          result = 0;
          break;
        }
      }
    }
  }
  return result;
}

u64
u64_from_str8(String8 string, u32 radix) {
  u64 x = 0;
  if (1 < radix && radix <= 16) {
    foreach_index(i, string.size) {
      x *= radix;
      x += integer_symbol_reverse[string.str[i] & 0x7F];
    }
  }
  return (x);
}

i64
i64_from_str8(String8 string, u32 radix) {
  i64 sign = sign_from_str8(string, &string);
  i64 x    = (i64)u64_from_str8(string, radix) * sign;
  return (x);
}

u32
u32_from_str8(String8 string, u32 radix) {
  u64 x64 = u64_from_str8(string, radix);
  u32 x32 = safe_cast_u32(x64);
  return x32;
}

i32
i32_from_str8(String8 string, u32 radix) {
  i64 x64 = i64_from_str8(string, radix);
  i32 x32 = safe_cast_i32(x64);
  return x32;
}

b32
try_u64_from_str8_c_rules(String8 string, u64* x) {
  u64 radix, prefix_size;
  // hex
  if (str8_match(str8_prefix(string, 2), str8_lit("0x"),
                 StringMatchFlag_CaseInsensitive)) {
    radix = 0x10, prefix_size = 2;
  }
  // binary
  else if (str8_match(str8_prefix(string, 2), str8_lit("0b"),
                      StringMatchFlag_CaseInsensitive)) {
    radix = 2, prefix_size = 2;
  }
  // octal
  else if (str8_match(str8_prefix(string, 1), str8_lit("0"),
                      StringMatchFlag_CaseInsensitive)
           && string.size > 1) {
    radix = 010, prefix_size = 1;
  }
  // decimal
  else {
    radix = 10, prefix_size = 0;
  }

  String8 integer    = str8_skip(string, prefix_size);
  b32     is_integer = str8_is_integer(integer, radix);
  if (is_integer) {
    *x = u64_from_str8(integer, radix);
  }

  return is_integer;
}

b32
try_i64_from_str8_c_rules(String8 string, i64* x) {
  String8 string_tail = { 0 };
  i64     sign        = sign_from_str8(string, &string_tail);
  u64     x_u64       = 0;
  b32     is_integer  = try_u64_from_str8_c_rules(string_tail, &x_u64);
  *x                  = x_u64 * sign;
  return is_integer;
}

String8
str8_from_memory_size(Arena* arena, u64 size) {
  String8 result;

  if (size < KB(1)) {
    result = str8f(arena, "%llu Bytes", size);
  } else if (size < MB(1)) {
    result = str8f(arena, "%llu.%02llu KiB", size / KB(1),
                   ((size * 100) / KB(1)) % 100);
  } else if (size < GB(1)) {
    result = str8f(arena, "%llu.%02llu MiB", size / MB(1),
                   ((size * 100) / MB(1)) % 100);
  } else if (size < TB(1)) {
    result = str8f(arena, "%llu.%02llu GiB", size / GB(1),
                   ((size * 100) / GB(1)) % 100);
  } else {
    result = str8f(arena, "%llu.%02llu TiB", size / TB(1),
                   ((size * 100) / TB(1)) % 100);
  }

  return result;
}

String8
str8_from_count(Arena* arena, u64 count) {
  String8 result;

  if (count < 1 * 1000) {
    result = str8f(arena, "%llu", count);
  } else if (count < 1000000) {
    u64 frac = ((count * 100) / 1000) % 100;
    if (frac > 0) {
      result = str8f(arena, "%llu.%02lluK", count / 1000, frac);
    } else {
      result = str8f(arena, "%lluK", count / 1000);
    }
  } else if (count < 1000000000) {
    u64 frac = ((count * 100) / 1000000) % 100;
    if (frac > 0) {
      result = str8f(arena, "%llu.%02lluM", count / 1000000, frac);
    } else {
      result = str8f(arena, "%lluM", count / 1000000);
    }
  } else {
    u64 frac = ((count * 100) * 1000000000) % 100;
    if (frac > 0) {
      result = str8f(arena, "%llu.%02lluB", count / 1000000000, frac);
    } else {
      result = str8f(arena, "%lluB", count / 1000000000, frac);
    }
  }

  return result;
}

String8
str8_from_bits_u32(Arena* arena, u32 x) {
  u8 c0 = 'a' + ((x >> 28) & 0xf);
  u8 c1 = 'a' + ((x >> 24) & 0xf);
  u8 c2 = 'a' + ((x >> 20) & 0xf);
  u8 c3 = 'a' + ((x >> 16) & 0xf);
  u8 c4 = 'a' + ((x >> 12) & 0xf);
  u8 c5 = 'a' + ((x >> 8) & 0xf);
  u8 c6 = 'a' + ((x >> 4) & 0xf);
  u8 c7 = 'a' + ((x >> 0) & 0xf);

  String8 result =
    str8f(arena, "%c%c%c%c%c%c%c%c", c0, c1, c2, c3, c4, c5, c6, c7);
  return result;
}

String8
str8_from_bits_u64(Arena* arena, u64 x) {
  u8 c0 = 'a' + ((x >> 60) & 0xf);
  u8 c1 = 'a' + ((x >> 56) & 0xf);
  u8 c2 = 'a' + ((x >> 52) & 0xf);
  u8 c3 = 'a' + ((x >> 48) & 0xf);
  u8 c4 = 'a' + ((x >> 44) & 0xf);
  u8 c5 = 'a' + ((x >> 40) & 0xf);
  u8 c6 = 'a' + ((x >> 36) & 0xf);
  u8 c7 = 'a' + ((x >> 32) & 0xf);
  u8 c8 = 'a' + ((x >> 28) & 0xf);
  u8 c9 = 'a' + ((x >> 24) & 0xf);
  u8 ca = 'a' + ((x >> 20) & 0xf);
  u8 cb = 'a' + ((x >> 16) & 0xf);
  u8 cc = 'a' + ((x >> 12) & 0xf);
  u8 cd = 'a' + ((x >> 8) & 0xf);
  u8 ce = 'a' + ((x >> 4) & 0xf);
  u8 cf = 'a' + ((x >> 0) & 0xf);

  String8 result = str8f(arena, "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c", c0, c1, c2,
                                  c3, c4, c5, c6, c7, c8, c9, ca, cb, cc, cd, ce, cf);
  return result;
}

String8
str8_from_u64(Arena* arena, u64 _u64, u32 radix,
                       u8 min_digits, u8 digit_group_separator) {
  String8 result = { 0 };
  {
    String8 prefix = { 0 };
    switch (radix) {
    case 16: {
      prefix = str8_lit("0x");
    } break;
    case 8: {
      prefix = str8_lit("0o");
    } break;
    case 2: {
      prefix = str8_lit("0b");
    } break;
    }

    u8 digit_group_size = 3;
    switch (radix) {
    default: break;
    case 2 :
    case 8 :
    case 16: {
      digit_group_size = 4;
    } break;
    }

    u64 needed_leading_0s = 0;
    {
      u64 needed_digits = 1;
      {
        u64 u64_reduce = _u64;
        for (;;) {
          u64_reduce /= radix;
          if (u64_reduce == 0) {
            break;
          }
          needed_digits += 1;
        }
      }
      needed_leading_0s =
        (min_digits > needed_digits) ? min_digits - needed_digits : 0;
      u64 needed_separators = 0;
      if (digit_group_separator != 0) {
        needed_separators =
          (needed_digits + needed_leading_0s) / digit_group_size;
        if (needed_separators > 0
            && (needed_digits + needed_leading_0s) % digit_group_size == 0) {
          needed_separators -= 1;
        }
      }
      result.size =
        prefix.size + needed_leading_0s + needed_separators + needed_digits;
      result.str              = push_array(arena, u8, result.size + 1);
      result.str[result.size] = 0;
    }

    {
      u64 u64_reduce             = _u64;
      u64 digits_until_separator = digit_group_size;
      foreach_index(idx, result.size) {
        if (digits_until_separator == 0 && digit_group_separator != 0) {
          result.str[result.size - idx - 1] = digit_group_separator;
          digits_until_separator            = digit_group_size + 1;
        } else {
          result.str[result.size - idx - 1] =
            char_to_lower(integer_symbols[u64_reduce % radix]);
          u64_reduce /= radix;
        }
        digits_until_separator -= 1;
        if (u64_reduce == 0) {
          break;
        }
      }
      foreach_index(leading_0_idx, needed_leading_0s) {
        result.str[prefix.size + leading_0_idx] = '0';
      }
    }

    if (prefix.size != 0) {
      MemoryCopy(result.str, prefix.str, prefix.size);
    }
  }
  return result;
}

String8
str8_from_i64(Arena* arena, i64 _i64, u32 radix,
                       u8 min_digits,u8 digit_group_separator) {
  String8 result = { 0 };
  if (_i64 < 0) {
    String8 numeric_part = str8_from_u64(arena, (u64)(-_i64), radix,
                                         min_digits, digit_group_separator);
    result               = str8f(arena, "-%S", numeric_part);
  } else {
    result = str8_from_u64(arena, (u64)_i64, radix, min_digits,
                           digit_group_separator);
  }
  return result;
}

String8Node*
str8_list_push_node(String8List* list, String8Node* node) {
  SLLQueuePush(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += node->string.size;
  return node;
}

String8Node*
str8_list_push_node_set_string(String8List* list, String8Node* node,
                               String8 string) {
  SLLQueuePush(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += string.size;
  node->string = string;
  return node;
}

String8Node*
str8_list_push_node_front(String8List* list, String8Node* node) {
  SLLQueuePushFront(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += node->string.size;
  return node;
}

String8Node*
str8_list_push_node_front_set_string(String8List* list, String8Node* node,
                                     String8 string) {
  SLLQueuePushFront(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += string.size;
  node->string = string;
  return node;
}

String8Node*
str8_list_push(Arena* arena, String8List* list, String8 string) {
  String8Node* node = push_one(arena, String8Node);
  str8_list_push_node_set_string(list, node, string);
  return node;
}

String8Node*
str8_list_pushf(Arena* arena, String8List* list, char* fmt, ...) {
  va_list args, args2;
  va_start(args, fmt);
  va_copy(args2, args);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) { va_end(args2); return NULL; }
  char* buf = push_array(arena, char, (u64)needed + 1);
  vsnprintf(buf, (u64)needed + 1, fmt, args2);
  va_end(args2);
  return str8_list_push(arena, list, str8((u8*)buf, (u64)needed));
}

String8Node*
str8_list_push_front(Arena* arena, String8List* list, String8 string) {
  String8Node* node = push_one(arena, String8Node);
  str8_list_push_node_front_set_string(list, node, string);
  return node;
}

void
str8_list_concat_in_place(String8List* list, String8List* to_push) {
  if (to_push->node_count != 0) {
    if (list->last) {
      list->node_count += to_push->node_count;
      list->total_size += to_push->total_size;
      list->last->next  = to_push->first;
      list->last        = to_push->last;
    } else {
      *list = *to_push;
    }
    MemoryZeroStruct(to_push);
  }
}

String8List
str8_list_copy(Arena* arena, String8List* list) {
  String8List result = {0};
  foreach_in_list_ptr(String8Node*, node, list) {
    String8Node* new_node = push_one(arena, String8Node);
    String8 new_string = str8_copy(arena, node->string);
    str8_list_push_node_set_string(&result, new_node, new_string);
  }
  return result;
}

String8List
str8_split_into_list(Arena* arena, String8 string, u8* split_chars,
                              u64 split_char_count, StringSplitFlags flags) {
  String8List list = { 0 };

  b32 keep_empties = (flags & StringSplitFlag_KeepEmpties);

  u8* ptr = string.str;
  u8* opl = string.str + string.size;
  for (; ptr < opl;) {
    u8* first = ptr;
    for (; ptr < opl; ptr += 1) {
      u8  c        = *ptr;
      b32 is_split = 0;
      foreach_index(i, split_char_count) {
        if (split_chars[i] == c) {
          is_split = 1;
          break;
        }
      }
      if (is_split) {
        break;
      }
    }

    String8 string = str8_range(first, ptr);
    if (keep_empties || string.size > 0) {
      str8_list_push(arena, &list, string);
    }
    ptr += 1;
  }

  return (list);
}

String8List
str8_split_by_string_chars(Arena* arena, String8 string,
                                    String8 split_chars, StringSplitFlags flags) {
  String8List list =
    str8_split_into_list(arena, string, split_chars.str, split_chars.size, flags);
  return list;
}

String8List
str8_list_split_by_string_chars(Arena* arena, String8List list,
                                         String8 split_chars, StringSplitFlags flags) {
  String8List result = { 0 };
  foreach_in_list(String8Node*, node, list) {
    String8List split =
      str8_split_by_string_chars(arena, node->string, split_chars, flags);
    str8_list_concat_in_place(&result, &split);
  }
  return result;
}

String8
str8_list_join(Arena* arena, String8List* list,
                        StringJoin* optional_params) {
  StringJoin join = { 0 };
  if (optional_params != 0) {
    MemoryCopyStruct(&join, optional_params);
  }

  u64 sep_count = 0;
  if (list->node_count > 0) {
    sep_count = list->node_count - 1;
  }

  String8 result;
  result.size = join.pre.size + join.post.size + sep_count * join.sep.size
                + list->total_size;
  u8* ptr = result.str = push_array(arena, u8, result.size + 1);

  MemoryCopy(ptr, join.pre.str, join.pre.size);
  ptr += join.pre.size;
  foreach_in_list_ptr(String8Node*, node, list) {
    MemoryCopy(ptr, node->string.str, node->string.size);
    ptr += node->string.size;
    if (node->next != 0) {
      MemoryCopy(ptr, join.sep.str, join.sep.size);
      ptr += join.sep.size;
    }
  }
  MemoryCopy(ptr, join.post.str, join.post.size);
  ptr += join.post.size;

  *ptr = 0;

  return result;
}

void
str8_list_from_flags(Arena* arena, String8List* list, u32 flags,
                              String8* flag_string_table, u32 flag_string_count) {
  foreach_index(i, flag_string_count) {
    u32 flag = (1 << i);
    if (flags & flag) {
      str8_list_push(arena, list, flag_string_table[i]);
    }
  }
}

String8Array
str8_array_zero(void) {
  String8Array result = { 0 };
  return result;
}

String8Array
str8_array_from_list(Arena* arena, String8List* list) {
  String8Array array;
  array.count = list->node_count;
  array.v     = push_array(arena, String8, array.count);
  u64 idx     = 0;
  foreach_in_list_ptr(String8Node*, node, list) {
    array.v[idx] = node->string;
    idx += 1;
  }
  return array;
}

String8Array*
str8_array_from_list_arr(Arena* arena, String8List* lists, u64 count) {
  String8Array* result = push_array(arena, String8Array, count);
  foreach_index(idx, count) {
    result[idx] = str8_array_from_list(arena, &lists[idx]);
  }
  return result;
}

String8Array
str8_array_reserve(Arena* arena, u64 count) {
  String8Array arr;
  arr.count = 0;
  arr.v     = push_array(arena, String8, count);
  return arr;
}

String8Array
str8_array_copy(Arena* arena, String8Array array) {
  String8Array result = { 0 };
  result.count        = array.count;
  result.v            = push_array(arena, String8, result.count);
  foreach_index(idx, result.count) {
    result.v[idx] = str8_copy(arena, array.v[idx]);
  }
  return result;
}


String8Vector
str8_vector_zero(void) {
  SVString8 zero = {0};
  sv_str8_init(&zero);
  return (String8Vector){ .v = zero, .total_size = 0 };
}

void
str8_vector_init(String8Vector* sv) {
  sv_str8_init(&sv->v);
  sv->total_size = 0;
}

String8Vector
str8_vector_copy(Arena* arena, String8Vector* sv) {
  String8Vector result = {0};
  result.v = sv->v;
  return result;
}

void
str8_vector_reserve(Arena* arena, String8Vector* sv, u64 count) {
  sv_str8_reserve(arena, &sv->v, count);
}

void
str8_vector_push(Arena* arena, String8Vector* sv, String8 string) {
  sv_str8_push(arena, &sv->v, string);
  sv->total_size += string.size;
}

void
str8_vector_reset(String8Vector* sv) {
  sv_str8_reset(&sv->v);
}

void
str8_vector_free(String8Vector* sv) {
  sv_str8_free(&sv->v);
  sv->total_size = 0;
}

String8
str8_vector_get(String8Vector* sv, u64 index) {
  return sv_str8_get(&sv->v, index);
}

void
str8_vector_set(String8Vector* sv, u64 index, String8 val) {
  sv->total_size -= sv->v.items[index].size;
  sv->total_size += val.size;
  sv_str8_set(&sv->v, index, val);
}

b32
str8_vector_is_inline(String8Vector* sv) {
  return sv_str8_using_inline(&sv->v);
}

void
str8_split_into_vector(Arena* arena,
                                String8Vector* out,
                                String8 string,
                                u8* split_chars,
                                u64 split_char_count,
                                StringSplitFlags flags) {
  str8_vector_init(out);

  b32 keep_empties = (flags & StringSplitFlag_KeepEmpties);

  u8* ptr = string.str;
  u8* opl = string.str + string.size;
  for (; ptr < opl;) {
    u8* first = ptr;
    for (; ptr < opl; ptr += 1) {
      u8 c = *ptr;
      b32 is_split = false;
      foreach_index(i, split_char_count) {
        if (split_chars[i] == c) {
          is_split = true;
          break;
        }
      }
      if (is_split) {
        break;
      }
    }

    String8 string = str8_range(first, ptr);
    if (keep_empties || string.size > 0) {
      str8_vector_push(arena, out, string);
    }
    ptr += 1;
  }
}

String8
str8_vector_join(Arena* arena,
                          String8Vector* sv,
                          StringJoin* optional_params) {
  StringJoin join = { 0 };
  if (optional_params != 0) {
    MemoryCopyStruct(&join, optional_params);
  }

  u64 sep_count = 0;
  if (sv->v.count > 0) {
    sep_count = sv->v.count - 1;
  }

  String8 result;
  result.size = join.pre.size + join.post.size + sep_count * join.sep.size + sv->total_size;
  u8* ptr = result.str = push_array(arena, u8, result.size + 1);

  MemoryCopy(ptr, join.pre.str, join.pre.size);
  ptr += join.pre.size;
  foreach_index(i, sv->v.count) {
    MemoryCopy(ptr, sv->v.items[i].str, sv->v.items[i].size);
    ptr += sv->v.items[i].size;
    if (i+1 < sv->v.count) {
      MemoryCopy(ptr, join.sep.str, join.sep.size);
      ptr += join.sep.size;
    }
  }
  MemoryCopy(ptr, join.post.str, join.post.size);
  ptr += join.post.size;

  *ptr = 0;

  return result;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <time.h>

static LARGE_INTEGER qpc_freq;
static int           qpc_inited = 0;

static void
ensure_qpc_init(void) {
  if (!qpc_inited) {
    QueryPerformanceFrequency(&qpc_freq);
    qpc_inited = 1;
  }
}

Time
time_wall(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);

  // Windows FILETIME = 100-ns ticks since Jan 1 1601 (UTC).
  // Convert to a 64-bit integer.
  u64 windows_time =
    ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

  // Convert to Unix epoch (Jan 1 1970):
  // Difference is 11644473600 seconds, or 11644473600 * 1e9 =
  // 11644473600000000000 ns
  const u64 UNIX_EPOCH_IN_WINDOWS_NS = 11644473600000000000ULL;

  u64 ns_since_1601 = windows_time * 100ULL; // 100 ns → ns
  u64 ns_since_unix = ns_since_1601 - UNIX_EPOCH_IN_WINDOWS_NS;

  return (Time){ .nsec = (i64)ns_since_unix };
}

Time
time_now(void) {
  /* Use QPC as best-available high-res clock for both monotonic and "now"
   * semantics */
  ensure_qpc_init();
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  /* convert ticks to nanoseconds */
  long double ticks = (long double)v.QuadPart;
  long double ns    = ticks * (1e9L / (long double)qpc_freq.QuadPart);
  return (Time){ .nsec = (i64)ns };
}

void
sleep_for(Duration d) {
  if (d <= 0) return;
  i64 ms = d / Millisecond;
  if (ms <= 0) {
    ms = 1; // Clamp up to 1 ms for now
  }
  Sleep((DWORD)ms);
}

/* Windows we use monotonic QPC-based times as well */
void
sleep_until(Time t) {
  while (1) {
    Duration rem = time_until(t);
    if (rem <= 0) return;
    i64 ms = rem / Millisecond;
    if (ms > 0) {
      Sleep((DWORD)ms);
    } else {
      Sleep(0);
    }
  }
}

struct timespec
duration_to_timespec(Duration d) {
  struct timespec ts = { 0, 0 };
  if (d <= 0) return ts;
  ts.tv_sec  = (time_t)(d / Second);
  ts.tv_nsec = (long)(d % Second);
  return ts;
}

#elif defined(__APPLE__)
#include <errno.h>
#include <mach/mach_time.h>
#include <time.h>
#include <unistd.h>

/* macOS mach_absolute_time -> nanoseconds */
static mach_timebase_info_data_t mtb;
static int                       mtb_inited = 0;
static void
ensure_mtb(void) {
  if (!mtb_inited) {
    mach_timebase_info(&mtb);
    mtb_inited = 1;
  }
}

Time
time_wall(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    printf("time_wall(): clock_gettime failed");
    return (Time){ .nsec = -1 };
  }
  return (Time){ .nsec = (i64)ts.tv_sec * Second + ts.tv_nsec };
}
Time
time_now(void) {
  ensure_mtb();
  u64 t = mach_absolute_time();
  /* convert to ns using timebase (numer/denom) */
  u64 ns = t * (u64)mtb.numer / (u64)mtb.denom;
  return (Time){ .nsec = (i64)ns };
}

struct timespec
duration_to_timespec(Duration d) {
  struct timespec ts = { 0, 0 };
  if (d <= 0) return ts;
  ts.tv_sec  = (time_t)(d / Second);
  ts.tv_nsec = (i64)(d % Second);
  return ts;
}

void
sleep_for(Duration d) {
  struct timespec ts = duration_to_timespec(d);
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { /* */ }
}

void
sleep_until(Time t) {
  while (1) {
    Duration rem = time_until(t);
    if (rem <= 0) return;
    struct timespec ts = duration_to_timespec(rem);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { /* */ }
  }
}

#else // Linux/Posix
#include <errno.h>
#include <time.h>

Time
time_wall(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    printf("time_wall(): clock_gettime failed");
    return (Time){ .nsec = -1 };
  }
  return (Time){ .nsec = ts.tv_sec * Second + ts.tv_nsec };
}

Time
time_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    printf("time_now(): clock_gettime failed");
    return (Time){ .nsec = -1 };
  }
  return (Time){ .nsec = ts.tv_sec * Second + ts.tv_nsec };
}

static inline struct timespec
duration_to_timespec(Duration d) {
  struct timespec ts;

  if (d < 0) {
    // negative duration → zero sleep
    ts.tv_sec  = 0;
    ts.tv_nsec = 0;
    return ts;
  }

  ts.tv_sec  = d / Second;
  ts.tv_nsec = d % Second;

  return ts;
}

void
sleep_for(Duration d) {
  struct timespec ts = duration_to_timespec(d);
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    /**/
  }
}

void
sleep_until(Time t) {
  while (1) {
    Duration rem = time_until(t);
    if (rem <= 0) return;
    struct timespec ts = duration_to_timespec(rem);
    if (nanosleep(&ts, &ts) == 0 || errno != EINTR) { /* */ }
  }
}

#endif

Time
time_add(Time t, Duration d) {
  return (Time){ .nsec = t.nsec + d };
}

Time
time_sub(Time t, Duration d) {
  return (Time){ .nsec = t.nsec - d };
}

Duration
time_diff(Time start, Time end) {
  return Clamp(MIN_DURATION, end.nsec - start.nsec, MAX_DURATION);
}

Duration
time_since(Time start) {
  return time_diff(start, time_now());
}

Duration
time_until(Time end) {
  return time_diff(time_now(), end);
}

f64
duration_nanoseconds(Duration d) {
  return (f64)d;
}

f64
duration_microseconds(Duration d) {
  return duration_seconds(d) * 1e6;
}

f64
duration_milliseconds(Duration d) {
  return duration_seconds(d) * 1e3;
}

f64
duration_seconds(Duration d) {
  i64 sec  = d / Second;
  i64 nsec = d % Second;
  return (f64)sec + (f64)nsec / Second;
}

f64
duration_minutes(Duration d) {
  i64 min  = d / Minute;
  i64 nsec = d % Minute;
  return (f64)min + (f64)nsec / Minute;
}

f64
duration_hours(Duration d) {
  i64 hour = d / Hour;
  i64 nsec = d % Hour;
  return (f64)hour + (f64)nsec / Hour;
}

void
stopwatch_start(Stopwatch* stopwatch) {
  if (!stopwatch->running) {
    stopwatch->start_time = time_now();
    stopwatch->running    = true;
  }
}
void
stopwatch_stop(Stopwatch* stopwatch) {
  if (stopwatch->running) {
    stopwatch->accumulation += time_since(stopwatch->start_time);
    stopwatch->running = false;
  }
}

void
stopwatch_reset(Stopwatch* stopwatch) {
  stopwatch->accumulation = 0;
  stopwatch->running      = false;
}

Duration
stopwatch_duration(Stopwatch* stopwatch) {
  if (!stopwatch->running) {
    return stopwatch->accumulation;
  }
  return stopwatch->accumulation + time_since(stopwatch->start_time);
}

#if !COMPILER_TCC
extern Thread  os_thread_launch(ThreadFn* func, void* ptr);
extern b32     os_thread_join(Thread handle);
extern void    os_thread_detach(Thread handle);
extern Mutex   os_mutex_alloc(void);
extern void    os_mutex_release(Mutex mutex);
extern void    os_mutex_take(Mutex mutex);
extern void    os_mutex_drop(Mutex mutex);
extern CondVar os_cond_var_alloc(void);
extern void    os_cond_var_release(CondVar cv);
extern b32     os_cond_var_wait(CondVar cv, Mutex mutex, u64 duration_ns);
extern void    os_cond_var_signal(CondVar cv);
extern void    os_cond_var_broadcast(CondVar cv);
extern Barrier os_barrier_alloc(u64 count);
extern void    os_barrier_release(Barrier barrier);
extern void    os_barrier_wait(Barrier barrier);

Thread  thread_launch(ThreadFn* func, void* ptr) {return os_thread_launch(func, ptr);}
b32     thread_join(Thread handle) {return os_thread_join(handle);}
void    thread_detach(Thread handle) {os_thread_detach(handle);}
Mutex   mutex_alloc(void) {return os_mutex_alloc();}
void    mutex_release(Mutex mutex) {os_mutex_release(mutex);}
void    mutex_take(Mutex mutex) {os_mutex_take(mutex);}
void    mutex_drop(Mutex mutex) {os_mutex_drop(mutex);}
CondVar cond_var_alloc(void) {return os_cond_var_alloc();}
void    cond_var_release(CondVar cv) {os_cond_var_release(cv);}
b32     cond_var_wait(CondVar cv, Mutex mutex, u64 duration_ns) {return os_cond_var_wait(cv, mutex, duration_ns);}
void    cond_var_signal(CondVar cv) {os_cond_var_signal(cv);}
void    cond_var_broadcast(CondVar cv) {os_cond_var_broadcast(cv);}
Barrier barrier_alloc(u32 count) {return os_barrier_alloc(count);}
void    barrier_release(Barrier barrier) {os_barrier_release(barrier);}
void    barrier_wait(Barrier barrier) {os_barrier_wait(barrier);}

Thread_Local static ThreadContext* tls_tctx = NULL;

// One mutex guarding two condvars: g_phase_start_cv wakes lane threads when
// a new phase is dispatched (async_run_phase), g_phase_done_cv wakes
// async_phase_wait when the LAST lane finishes the current phase. Splitting
// them (rather than one cv doing double duty) is what lets async_phase_wait
// actually block on real completion instead of falling back to a polling
// loop -- see async_phase_wait below; a single shared mutex for both is the
// ordinary, safe pattern (two independent, very-short-held critical
// sections, never nested).
static CondVar g_phase_start_cv = { 0 };
static CondVar g_phase_done_cv  = { 0 };
static Mutex   g_phase_mutex    = { 0 };

static _Atomic b32 g_async_exit = false;

static _Atomic(AsyncPhaseFn*) g_phase_fn = NULL;

static _Atomic u64 g_phase_generation     = 0;
static _Atomic u64 g_completed_generation = 0;
static _Atomic u32 g_phase_done_count     = 0;

static struct {
  u32      lane_count;
  Barrier  barrier;
  u64*     broadcast_buffer;
  LaneCtx* lane_ctxs;
  Thread*  threads;
} g_threads;


void
tctx_init(ThreadContext* tctx, u64 scratch_size) {
  assert(tctx && "Context is NULL");
  assert(scratch_size > 0 && "Scratch size is zero");
  MemoryZeroStruct(tctx);

  tctx->scratch = arena_create_vm(scratch_size);
}

void
tctx_init_with_perm(ThreadContext* tctx, u64 scratch_size, u64 perm_size) {
  assert(tctx && "Context is NULL");
  assert(scratch_size > 0 && "Scratch size is zero");
  assert(perm_size > 0 && "Perm size is zero");
  MemoryZeroStruct(tctx);

  tctx->scratch = arena_create_vm(scratch_size);
  tctx->perm    = arena_create_heap(perm_size);
}

void
tctx_destroy(ThreadContext* tctx) {
  arena_destroy(&tctx->perm);
  arena_destroy(&tctx->scratch);
}

void
tctx_attach(ThreadContext* t) {
  tls_tctx = t;
}

void
tctx_detach(void) {
  tls_tctx = NULL;
}

ThreadContext*
tctx_current(void) {
  return tls_tctx;
}

Arena*
tctx_scratch(void) {
  assert(tls_tctx && "No thread context");
  return &tls_tctx->scratch;
}

Arena*
tctx_perm(void) {
  assert(tls_tctx && "No thread context");
  return &tls_tctx->perm;
}

void
supplement_thread_base_entry_point(void (*entry_point)(void* params), void* params) {
  ThreadContext tctx = { 0 };
  // Scratch is VM-backed (base.h's arena_create_vm), so its size here is a
  // hard RESERVE cap, not an initial size -- unlike perm (heap-backed,
  // grows via chained blocks regardless of the size passed here), a
  // long-lived worker thread (e.g. a lane thread servicing many
  // async_run_phase calls back to back, see lane_thread_main) can't just
  // grow past a too-small reserve once picked. MB(4) costs nothing up
  // front (virtual reservation, lazily committed) but leaves real headroom
  // for scratch-heavy work done on the same thread over its lifetime,
  // e.g. codegen's c_mangle_name/ctx_scratch() calls when rendering many
  // top-level items across one lane-parallel codegen phase (see
  // cg_program_parallel in codegen.c) -- KB(4) overflowed almost
  // immediately under that workload.
  tctx_init_with_perm(&tctx, MB(4), KB(4));
  tctx_attach(&tctx);
  entry_point(params);
  tctx_destroy(&tctx);
  tctx_detach();
}

LaneCtx
lane_enter(LaneCtx lane_ctx) {
  ThreadContext* tctx = tctx_current();
  assert(tctx && "lane_enter called without ThreadContext");
  LaneCtx prev = tctx->lane;
  tctx->lane   = lane_ctx;
  return prev;
}

void
lane_barrier_wait(void* broadcast_ptr, u64 broadcast_size, u64 broadcast_lane_idx) {

  u64 broadcast_size_clamped = Min(broadcast_size, sizeof(lane_broadcast()));

  if (broadcast_ptr && lane_idx() == broadcast_lane_idx) {
    MemoryCopy(lane_broadcast(), broadcast_ptr, broadcast_size_clamped);
  }

  barrier_wait(tctx_current()->lane.barrier);

  if (broadcast_ptr && lane_idx() != broadcast_lane_idx) {
    MemoryCopy(broadcast_ptr, lane_broadcast(), broadcast_size_clamped);
  }

  if (broadcast_ptr) {
    barrier_wait(tctx_current()->lane.barrier);
  }
}

void
lane_reduce_sum_u64(u64* ptr, u32 dst_lane_idx) {
  ThreadContext* tctx = tctx_current();

  // shared points to the first u64 in the broadcast buffer
  u64* shared = lane_broadcast();

  // each lane writes its local value
  shared[lane_idx()] = *ptr;
  barrier_wait(tctx->lane.barrier);

  // destination lane reduces
  if (lane_idx() == dst_lane_idx) {
    u64 sum = 0;
    foreach_index(i, lane_count()) sum += shared[i];
    *ptr = sum;
  }
  barrier_wait(tctx->lane.barrier);
}

void
lane_leave(LaneCtx prev) {
  tctx_current()->lane = prev;
}

Rng1u64
lane_range(u64 work_count) {
  u64 lanes = lane_count();
  u64 idx   = lane_idx();

  assert(idx < lanes);

  u64 base = work_count / lanes;
  u64 rem  = work_count % lanes;

  u64 start = Min(idx * base + Min(idx, rem), work_count);
  u64 size  = base + (idx < rem);
  if (start + size > work_count) size = work_count - start;

  return r1u64(start, start + size);
}

void
lane_thread_main(void* ptr) {
  LaneCtx* ctx = (LaneCtx*)ptr;
  lane_enter(*ctx);

  u64 last_seen_generation = 0;

  for (;;) {
    mutex_scope(g_phase_mutex) {
      while (atomic_load(&g_phase_generation) == last_seen_generation
             && !atomic_load(&g_async_exit)) {
        cond_var_wait(g_phase_start_cv, g_phase_mutex, Second);
      }
    }

    if (atomic_load(&g_async_exit)) break;

    last_seen_generation = atomic_load(&g_phase_generation);

    AsyncPhaseFn* func = atomic_load(&g_phase_fn);
    if (func) func();

    u32 done = atomic_fetch_add(&g_phase_done_count, 1) + 1;
    if (done == g_threads.lane_count) {
      // Held while both storing the completion generation AND broadcasting
      // -- async_phase_wait takes the SAME mutex around its own check+wait,
      // which is what makes this race-free: a waiter either (a) hasn't
      // taken the mutex yet, so it'll see the new g_completed_generation
      // once it does, or (b) is already inside cond_var_wait, which
      // atomically released the mutex and will be woken by the broadcast
      // -- there's no window where the store happens and a waiter is
      // "in between" its check and its wait with the old value latched.
      mutex_scope(g_phase_mutex) {
        atomic_store(&g_completed_generation, last_seen_generation);
        cond_var_broadcast(g_phase_done_cv);
      }
    }
  }
}

void
async_threads_init(void) {
  u64 num_async_threads = Max(1, os_get_core_count() - 1);

  g_phase_start_cv = cond_var_alloc();
  g_phase_done_cv  = cond_var_alloc();
  g_phase_mutex    = mutex_alloc();

  g_threads.lane_count       = num_async_threads;
  g_threads.barrier          = barrier_alloc(num_async_threads);
  g_threads.lane_ctxs        = push_array(os_arena(), LaneCtx, num_async_threads);
  g_threads.threads          = push_array(os_arena(), Thread, num_async_threads);
  g_threads.broadcast_buffer = push_array(os_arena(), u64, num_async_threads);

  foreach_index(i, num_async_threads) {
    LaneCtx* lane = &g_threads.lane_ctxs[i];

    lane->lane_index     = i;
    lane->lane_count     = g_threads.lane_count;
    lane->barrier        = g_threads.barrier;
    lane->broadcast      = (u64*)&g_threads.broadcast_buffer[0];
    g_threads.threads[i] = thread_launch(lane_thread_main, lane);
  }
}

void
async_threads_shutdown(void) {
  atomic_store(&g_async_exit, true);
  mutex_scope(g_phase_mutex) { cond_var_broadcast(g_phase_start_cv); }

  foreach_index(i, g_threads.lane_count) { thread_join(g_threads.threads[i]); }

  barrier_release(g_threads.barrier);
  cond_var_release(g_phase_start_cv);
  cond_var_release(g_phase_done_cv);
  mutex_release(g_phase_mutex);
}

u64
async_run_phase(AsyncPhaseFn* func) {
  u64 prev_gen = atomic_load(&g_phase_generation);
  assert(atomic_load(&g_completed_generation) == prev_gen
         && "async_run_phase called before previous phase completed");

  atomic_store(&g_phase_fn, func);
  atomic_store(&g_phase_done_count, 0);

  u64 new_gen = prev_gen + 1;
  atomic_store(&g_phase_generation, new_gen);

  mutex_scope(g_phase_mutex) { cond_var_broadcast(g_phase_start_cv); }

  return new_gen;
}

b32
async_phase_done(u64 generation) {
  return atomic_load(&g_completed_generation) >= generation;
}

// Blocks until `generation` actually completes -- woken directly by the
// last lane's completion broadcast (see lane_thread_main), not a polling
// loop. The Second timeout is a pure safety net (matches lane_thread_main's
// own wait), never the normal wakeup path; a lost-wakeup bug here would
// show up as ~1s stalls, not silent incorrectness, since the predicate is
// re-checked every time regardless of why cond_var_wait returned.
void
async_phase_wait(u64 generation) {
  mutex_scope(g_phase_mutex) {
    while (!async_phase_done(generation)) {
      cond_var_wait(g_phase_done_cv, g_phase_mutex, Second);
    }
  }
}

OS_State g_os_state = { 0 };

static u8 arena_buffer[KB(4)];
static u8 entity_buffer[KB(4)];

void
os_state_init(void) {
  g_os_state.arena =
    arena_create_heap_with_buffer(arena_buffer, sizeof(arena_buffer));
  g_os_state.entity_arena =
    arena_create_heap_with_buffer(entity_buffer, sizeof(entity_buffer));
}

void
os_state_free(void) {
  arena_destroy(&g_os_state.arena);
  arena_destroy(&g_os_state.entity_arena);
}

Arena*
os_arena(void) {
  return &g_os_state.arena;
}

#if defined(_WIN32)

u64
os_get_page_size(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
}

u32
os_get_core_count(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (u32)si.dwNumberOfProcessors : 1;
}

OS_Entity*
os_entity_alloc(OS_EntityKind kind) {
  OS_Entity* entity = 0;
  defer(AcquireSRWLockExclusive(&g_os_state.entity_mutex),
        ReleaseSRWLockExclusive(&g_os_state.entity_mutex)) {
    entity = g_os_state.entity_free;
    if (entity) {
      SLLStackPop(g_os_state.entity_free);
    } else {
      assert(
        g_os_state.entity_arena.backend
        && "entity arena is not setup. did you forget to do os_state_init()?");
      entity = push_one(&g_os_state.entity_arena, OS_Entity);
    }
  }
  MemoryZeroStruct(entity);
  entity->kind = kind;
  return entity;
}

void
os_entity_release(OS_Entity* entity) {
  defer(AcquireSRWLockExclusive(&g_os_state.entity_mutex),
        ReleaseSRWLockExclusive(&g_os_state.entity_mutex)) {
    SLLStackPush(g_os_state.entity_free, entity);
  }
}

extern void supplement_thread_base_entry_point(void (*entry_point)(void* params), void* params);
DWORD WINAPI
os_thread_entry_point(LPVOID ptr) {
  OS_Entity* entity = (OS_Entity*)ptr;
  ThreadFn*  func       = entity->thread.func;
  void*      thread_ptr = entity->thread.ptr;
  supplement_thread_base_entry_point(func, thread_ptr);
  return 0;
}

Thread
os_thread_launch(ThreadFn* func, void* ptr) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_Thread);
  entity->thread.func = func;
  entity->thread.ptr  = ptr;

  entity->thread.handle = CreateThread(NULL, 0, os_thread_entry_point, entity, 0, NULL);
  if (!entity->thread.handle) {
    os_entity_release(entity);
    entity = NULL;
  }

  Thread handle = {(u64)entity};
  return handle;
}

// Assumes handle is actually pointing to something
b32
os_thread_join(Thread handle) {
  OS_Entity* entity = (OS_Entity*)handle.v[0];
  assert(entity->kind == OS_EntityKind_Thread);
  b32 join_result = WaitForSingleObject(entity->thread.handle, INFINITE) == WAIT_OBJECT_0;
  CloseHandle(entity->thread.handle);
  os_entity_release(entity);
  return join_result;
}

// Assumes handle is actually pointing to something
void
os_thread_detach(Thread handle) {
  OS_Entity* entity = (OS_Entity*)handle.v[0];
  // No pthread_detach equivalent needed -- a Windows thread keeps running
  // independently of any handle to it; CloseHandle here just releases OUR
  // reference (a refcounted kernel object), same "don't need to join"
  // effect as pthread_detach without an explicit detach call.
  CloseHandle(entity->thread.handle);
  os_entity_release(entity);
}

Mutex
os_mutex_alloc(void) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_Mutex);
  InitializeSRWLock(&entity->mutex_handle);
  Mutex  handle = { (u64)entity };
  return handle;
}

void
os_mutex_release(Mutex mutex) {
  // SRWLOCK needs no explicit destroy (unlike pthread_mutex_t) -- just
  // return the entity to the pool.
  OS_Entity* entity = (OS_Entity*)mutex.v[0];
  os_entity_release(entity);
}

void
os_mutex_take(Mutex mutex) {
  OS_Entity* entity = (OS_Entity*)mutex.v[0];
  AcquireSRWLockExclusive(&entity->mutex_handle);
}

void
os_mutex_drop(Mutex mutex) {
  OS_Entity* entity = (OS_Entity*)mutex.v[0];
  ReleaseSRWLockExclusive(&entity->mutex_handle);
}

CondVar
os_cond_var_alloc(void) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_CondVar);
  InitializeConditionVariable(&entity->cond_handle);
  CondVar handle = { (u64)entity };
  return handle;
}

void
os_cond_var_release(CondVar cv) {
  // CONDITION_VARIABLE needs no explicit destroy either.
  OS_Entity* entity = (OS_Entity*)cv.v[0];
  os_entity_release(entity);
}

b32
os_cond_var_wait(CondVar cv, Mutex mutex, u64 duration_ns) {
  OS_Entity* cv_entity    = (OS_Entity*)cv.v[0];
  OS_Entity* mutex_entity = (OS_Entity*)mutex.v[0];
  DWORD      timeout_ms   = (DWORD)(duration_ns / 1000000ull);
  BOOL ok = SleepConditionVariableSRW(&cv_entity->cond_handle, &mutex_entity->mutex_handle, timeout_ms, 0);
  return ok != 0;
}

void
os_cond_var_signal(CondVar cv) {
  OS_Entity* cv_entity = (OS_Entity*)cv.v[0];
  WakeConditionVariable(&cv_entity->cond_handle);
}

void
os_cond_var_broadcast(CondVar cv) {
  OS_Entity* cv_entity = (OS_Entity*)cv.v[0];
  WakeAllConditionVariable(&cv_entity->cond_handle);
}

Barrier
os_barrier_alloc(u64 count) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_Barrier);
  if (entity) {
    InitializeSRWLock(&entity->barrier.mutex);
    InitializeConditionVariable(&entity->barrier.cond);
    entity->barrier.count = (u32)count;
  }
  Barrier result = { IntFromPtr(entity) };
  return result;
}

void
os_barrier_release(Barrier barrier) {
  // Neither SRWLOCK nor CONDITION_VARIABLE need explicit destroy.
  OS_Entity* entity = (OS_Entity*)PtrFromInt(barrier.v[0]);
  if (entity) os_entity_release(entity);
}

// Sense/generation-reversing barrier -- see this OS_Entity union member's
// own comment on why this isn't a native primitive. The LAST arrival
// resets `waiting` and bumps `generation`, waking every earlier arrival's
// condvar wait; each of those re-checks against the generation it
// originally observed, so a spurious wakeup (or a thread immediately
// reusing this same barrier for its NEXT wait) can never let it fall
// through early.
void
os_barrier_wait(Barrier barrier) {
  OS_Entity* entity = (OS_Entity*)PtrFromInt(barrier.v[0]);
  if (!entity) return;
  AcquireSRWLockExclusive(&entity->barrier.mutex);
  u32 gen = entity->barrier.generation;
  entity->barrier.waiting += 1;
  if (entity->barrier.waiting == entity->barrier.count) {
    entity->barrier.waiting = 0;
    entity->barrier.generation += 1;
    WakeAllConditionVariable(&entity->barrier.cond);
  } else {
    while (entity->barrier.generation == gen) {
      SleepConditionVariableSRW(&entity->barrier.cond, &entity->barrier.mutex, INFINITE, 0);
    }
  }
  ReleaseSRWLockExclusive(&entity->barrier.mutex);
}

#else // POSIX (Linux/Mac)

u64
os_get_page_size(void) {
  return sysconf(_SC_PAGESIZE);
}

u32
os_get_core_count(void) {
  // sysconf(_SC_NPROCESSORS_ONLN), not get_nprocs() -- the latter is a
  // glibc extension (<sys/sysinfo.h>), unavailable on macOS. sysconf
  // with this name is supported on both.
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (u32)n : 1;
}

OS_Entity*
os_entity_alloc(OS_EntityKind kind) {
  OS_Entity* entity = 0;
  defer(pthread_mutex_lock(&g_os_state.entity_mutex),
        pthread_mutex_unlock(&g_os_state.entity_mutex)) {
    entity = g_os_state.entity_free;
    if (entity) {
      SLLStackPop(g_os_state.entity_free);
    } else {
      assert(
        g_os_state.entity_arena.backend
        && "entity arena is not setup. did you forget to do os_state_init()?");
      entity = push_one(&g_os_state.entity_arena, OS_Entity);
    }
  }
  MemoryZeroStruct(entity);
  entity->kind = kind;
  return entity;
}

void
os_entity_release(OS_Entity* entity) {
  defer(pthread_mutex_lock(&g_os_state.entity_mutex),
        pthread_mutex_unlock(&g_os_state.entity_mutex)) {
    SLLStackPush(g_os_state.entity_free, entity);
  }
}

extern void supplement_thread_base_entry_point(void (*entry_point)(void* params), void* params);
void*
os_thread_entry_point(void* ptr) {
  OS_Entity* entity = (OS_Entity*)ptr;
  ThreadFn*  func       = entity->thread.func;
  void*      thread_ptr = entity->thread.ptr;
  supplement_thread_base_entry_point(func, thread_ptr);
  return 0;
}

Thread
os_thread_launch(ThreadFn* func, void* ptr) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_Thread);
  entity->thread.func = func;
  entity->thread.ptr  = ptr;

  if (pthread_create(&entity->thread.handle, 0, os_thread_entry_point, entity)
      != 0) {
    os_entity_release(entity);
    entity = NULL;
  }

  Thread handle = {(u64)entity};
  return handle;
}

// Assumes handle is actually pointing to something
b32
os_thread_join(Thread handle) {
  OS_Entity* entity = (OS_Entity*)handle.v[0];
  assert(entity->kind == OS_EntityKind_Thread);
  i32 join_result = pthread_join(entity->thread.handle, 0);
  os_entity_release(entity);
  return (join_result == 0);
}

// Assumes handle is actually pointing to something
void
os_thread_detach(Thread handle) {
  OS_Entity* entity = (OS_Entity*)handle.v[0];
  pthread_detach(entity->thread.handle);
  os_entity_release(entity);
}

Mutex
os_mutex_alloc(void) {
  OS_Entity* entity      = os_entity_alloc(OS_EntityKind_Mutex);
  i32        init_result = pthread_mutex_init(&entity->mutex_handle, 0);
  if (init_result != 0) {
    os_entity_release(entity);
    entity = 0;
  }
  Mutex  handle = { (u64)entity };
  return handle;
}

void
os_mutex_release(Mutex mutex) {
  OS_Entity* entity = (OS_Entity*)mutex.v[0];
  pthread_mutex_destroy(&entity->mutex_handle);
  os_entity_release(entity);
}

void
os_mutex_take(Mutex mutex) {
  OS_Entity* entity = (OS_Entity*)mutex.v[0];
  pthread_mutex_lock(&entity->mutex_handle);
}

void
os_mutex_drop(Mutex mutex) {
  OS_Entity* entity = (OS_Entity*)mutex.v[0];
  pthread_mutex_unlock(&entity->mutex_handle);
}

CondVar
os_cond_var_alloc(void) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_CondVar);
  // Default attr (no pthread_condattr_setclock(CLOCK_MONOTONIC)) --
  // Apple's pthread has never implemented that function at all (another
  // real POSIX gap on Darwin, like pthread_barrier_t above), so
  // os_cond_var_wait below measures its deadline against CLOCK_REALTIME
  // (the default a plain pthread_cond_t times out against) instead.
  i32 init_result = pthread_cond_init(&entity->cv.cond_handle, 0);
  if (init_result != 0) {
    os_entity_release(entity);
    entity = 0;
  }
  i32 init2_result = 0;
  if (entity) {
    pthread_mutex_init(&entity->cv.rwlock_mutex_handle, 0);
  }
  if (init2_result != 0) {
    pthread_cond_destroy(&entity->cv.cond_handle);
    os_entity_release(entity);
    entity = 0;
  }
  CondVar handle = { (u64)entity };
  return handle;
}

void
os_cond_var_release(CondVar cv) {
  OS_Entity* entity = (OS_Entity*)cv.v[0];
  pthread_cond_destroy(&entity->cv.cond_handle);
  pthread_mutex_destroy(&entity->cv.rwlock_mutex_handle);
  os_entity_release(entity);
}

b32
os_cond_var_wait(CondVar cv, Mutex mutex, u64 duration_ns) {
  OS_Entity*      cv_entity    = (OS_Entity*)cv.v[0];
  OS_Entity*      mutex_entity = (OS_Entity*)mutex.v[0];

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);

  struct timespec endt = now;
  u64 sec  = duration_ns / Second;
  u64 nsec = duration_ns % Second;
  endt.tv_sec  += sec;
  endt.tv_nsec += nsec;

  if (endt.tv_nsec >= Second) {
    endt.tv_sec  += 1;
    endt.tv_nsec -= Second;
  }

  i32 wait_result =
    pthread_cond_timedwait(&cv_entity->cv.cond_handle, &mutex_entity->mutex_handle, &endt);

  b32 result = (wait_result == 0);
  return result;
}

void
os_cond_var_signal(CondVar cv) {
  OS_Entity* cv_entity = (OS_Entity*)cv.v[0];
  pthread_cond_signal(&cv_entity->cv.cond_handle);
}

void
os_cond_var_broadcast(CondVar cv) {
  OS_Entity* cv_entity = (OS_Entity*)cv.v[0];
  pthread_cond_broadcast(&cv_entity->cv.cond_handle);
}

Barrier
os_barrier_alloc(u64 count) {
  OS_Entity* entity = os_entity_alloc(OS_EntityKind_Barrier);
  if (entity) {
    pthread_mutex_init(&entity->barrier.mutex, 0);
    pthread_cond_init(&entity->barrier.cond, 0);
    entity->barrier.count = (u32)count;
  }
  Barrier result = { IntFromPtr(entity) };
  return result;
}

void
os_barrier_release(Barrier barrier) {
  OS_Entity* entity = (OS_Entity*)PtrFromInt(barrier.v[0]);
  if (entity) {
    pthread_mutex_destroy(&entity->barrier.mutex);
    pthread_cond_destroy(&entity->barrier.cond);
    os_entity_release(entity);
  }
}

// Sense/generation-reversing barrier -- see this OS_Entity union member's
// own comment on why this isn't pthread_barrier_wait. The LAST arrival
// resets `waiting` and bumps `generation`, waking every earlier arrival's
// cond_wait; each of those re-checks against the generation it originally
// observed, so a spurious wakeup (or a thread immediately reusing this
// same barrier for its NEXT wait) can never let it fall through early.
void
os_barrier_wait(Barrier barrier) {
  OS_Entity* entity = (OS_Entity*)PtrFromInt(barrier.v[0]);
  if (!entity) return;
  pthread_mutex_lock(&entity->barrier.mutex);
  u32 gen = entity->barrier.generation;
  entity->barrier.waiting += 1;
  if (entity->barrier.waiting == entity->barrier.count) {
    entity->barrier.waiting = 0;
    entity->barrier.generation += 1;
    pthread_cond_broadcast(&entity->barrier.cond);
  } else {
    while (entity->barrier.generation == gen) {
      pthread_cond_wait(&entity->barrier.cond, &entity->barrier.mutex);
    }
  }
  pthread_mutex_unlock(&entity->barrier.mutex);
}

#endif
#endif

// Plain ANSI C stdio (fopen/fseek/ftell/fread), not a raw POSIX fd
// (open/fstat/read/close) -- identical on Linux/Mac/Windows with no
// platform branch at all. The one real cost: `ftell` returns a plain
// `long`, 32 bits under Windows' LLP64 model, capping a single
// `os_file_read` at ~2GB there (Linux/Mac's 64-bit `long` has no such
// cap) -- an acceptable trade for staying branch-free given what this
// actually reads in practice (see runtime/bbb_file.c's identical rewrite).
String8
os_file_read(Arena arena, String8 path) {
  char* cpath = cstring_str8(ctx_scratch(), path);
  FILE* f     = fopen(cpath, "rb");
  if (!f) return (String8){0};

  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return (String8){0}; }
  long file_size = ftell(f);
  if (file_size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return (String8){0};
  }
  u64 size = (u64)file_size;

  u8* dst        = push_array(&arena, u8, size);
  u64 total_read = fread(dst, 1, size, f);
  fclose(f);
  if (total_read != size) return (String8){0};

  return (String8){ .str = dst, .size = size };
}

// Fowler–Noll–Vo (FNV-1a) hash
static u64
str8_hash(String8 s) {
  u64 h = 1469598103934665603ull;
  foreach_index(i, s.size) {
    h ^= (u8)s.str[i];
    h *= 1099511628211ull;
  }
  return h;
}

void
hashtable_init(Arena* arena, HashTable* ht, u64 capacity) {
  ht->slots      = push_array(arena, HashSlot, capacity);
  ht->capacity   = capacity;
  ht->count      = 0;
  ht->tombstones = 0;
  foreach_index(i, capacity) {
    ht->slots[i].state = SlotState_Empty;
  }
}

// Forward declaration
static b32 hashtable_insert_internal(Arena* arena, HashTable* ht,
                                     String8 key, void* value,
                                     b32 overwrite);

// Resize to a new capacity (power of two suggested)
void
hashtable_resize(Arena* arena, HashTable* ht, u64 new_cap) {
  // allocate new slot array
  HashSlot* old_slots = ht->slots;
  u64       old_cap   = ht->capacity;

  HashSlot* new_slots = push_array(arena, HashSlot, new_cap);
  foreach_index(i, new_cap) {
    new_slots[i].state = SlotState_Empty;
  }

  ht->slots      = new_slots;
  ht->capacity   = new_cap;
  ht->count      = 0;
  ht->tombstones = 0;

  // re-insert old live entries
  foreach_index(i, old_cap) {
    if (old_slots[i].state == SlotState_Occupied) {
      hashtable_insert_internal(arena, ht, old_slots[i].key,
                                old_slots[i].value, false);
    }
  }
}

static b32
hashtable_insert_internal(Arena* arena, HashTable* ht, String8 key, void* value,
                          b32 overwrite) {
  if ((ht->count + ht->tombstones) * 2 >= ht->capacity) {
    hashtable_resize(arena, ht, ht->capacity * 2);
  }

  u64 h   = str8_hash(key);
  u64 idx = h % ht->capacity;

  i32 first_tombstone = -1;

  foreach_index(i, ht->capacity) {
    HashSlot* slot = &ht->slots[(idx + i) % ht->capacity];
    if (slot->state == SlotState_Empty) {
      if (first_tombstone >= 0) {
        slot = &ht->slots[first_tombstone];
        ht->tombstones--;
      }
      slot->key   = key;
      slot->value = value;
      slot->state = SlotState_Occupied;
      ht->count++;
      return true;
    } else if (slot->state == SlotState_Tombstone) {
      if (first_tombstone < 0) first_tombstone = (idx + i) % ht->capacity;
    } else if (slot->state == SlotState_Occupied && str8_match(slot->key, key, 0)) {
      if (overwrite) {
        slot->value = value;
        return true;
      } else {
        return false; // duplicate
      }
    }
  }
  return false;
}

b32
hashtable_insert(Arena* arena, HashTable* ht, String8 key, void* value, b32 overwrite) {
  return hashtable_insert_internal(arena, ht, key, value, overwrite);
}

void*
hashtable_lookup(HashTable* ht, String8 key) {
  u64 h   = str8_hash(key);
  u64 idx = h % ht->capacity;

  foreach_index(i, ht->capacity) {
    HashSlot* slot = &ht->slots[(idx + i) % ht->capacity];
    if (slot->state == SlotState_Empty) return NULL;
    if (slot->state == SlotState_Occupied && str8_match(slot->key, key, 0)) {
      return slot->value;
    }
  }
  return NULL;
}

b32
hashtable_remove(HashTable* ht, String8 key) {
  u64 h   = str8_hash(key);
  u64 idx = h % ht->capacity;

  foreach_index(i, ht->capacity) {
    HashSlot* slot = &ht->slots[(idx + i) % ht->capacity];
    if (slot->state == SlotState_Empty) return false;
    if (slot->state == SlotState_Occupied && str8_match(slot->key, key, 0)) {
      slot->state = SlotState_Tombstone;
      slot->value = NULL;
      ht->count--;
      ht->tombstones++;
      return true;
    }
  }
  return false;
}
