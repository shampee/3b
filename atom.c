#include "3b.h"

////////////////////////////////
//~ Numeric-atom classification
//
// A semantic concern, not a parsing one: the lexer and parser never look at
// this. A numeric-looking atom is i32 unless it contains a '.' or an exponent
// like `1e10`, which make it f32. An explicit annotation or a type suffix
// overrides that default.
//
// A `0x` prefix marks a hex integer, still i32 by default -- the prefix
// changes how the digits are read, not the resulting type. There is no hex
// float syntax, so atom_classify_numeric answers the hex case in full before
// the float scan runs. That is also what stops the hex digits `e` and `E` from
// being read as an exponent in `0x1e`.

// Optional sign, `0x`, then one or more hex digits. Nothing past that is
// validated: callers use this only to pick a base for strtoll, which rejects a
// malformed tail itself.
b32
atom_is_hex_literal(String8 atom) {
  u64 i = 0;
  if (atom.size > 0 && (atom.str[0] == '+' || atom.str[0] == '-')) i = 1;
  if (i + 2 > atom.size) return false;
  if (atom.str[i] != '0' || (atom.str[i + 1] != 'x' && atom.str[i + 1] != 'X')) return false;
  i += 2;
  if (i >= atom.size) return false; // "0x" alone isn't a literal
  for (; i < atom.size; i += 1) {
    u8 c = atom.str[i];
    b32 is_hex_digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!is_hex_digit) return false;
  }
  return true;
}

////////////////////////////////
//~ Numeric literal suffixes: 20i8, 20u8, 3.14f32, 3.14f64, ...
//
// A numeric atom may end in a type suffix -- i8 through u64, f32, f64 -- which
// pins its type instead of the i32/f32 default. No suffix is a suffix of
// another, so at most one can match.
//
// Hex is the one ambiguous case: `f` is a hex digit, so `0x1f32` could read as
// hex `0x1f32` or as hex `0x1` with an `f32` suffix. atom_classify_numeric
// resolves it by never trying a float suffix against a hex body. The language
// has no hex float literals, so nothing is lost.
static TypeKind
numeric_suffix_kind(String8 atom, u64* out_suffix_len) {
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("i64"), 0)) { *out_suffix_len = 3; return TypeKind_I64; }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("i32"), 0)) { *out_suffix_len = 3; return TypeKind_I32; }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("i16"), 0)) { *out_suffix_len = 3; return TypeKind_I16; }
  if (atom.size > 2 && str8_ends_with(atom, str8_lit("i8"),  0)) { *out_suffix_len = 2; return TypeKind_I8;  }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("u64"), 0)) { *out_suffix_len = 3; return TypeKind_U64; }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("u32"), 0)) { *out_suffix_len = 3; return TypeKind_U32; }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("u16"), 0)) { *out_suffix_len = 3; return TypeKind_U16; }
  if (atom.size > 2 && str8_ends_with(atom, str8_lit("u8"),  0)) { *out_suffix_len = 2; return TypeKind_U8;  }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("f32"), 0)) { *out_suffix_len = 3; return TypeKind_F32; }
  if (atom.size > 3 && str8_ends_with(atom, str8_lit("f64"), 0)) { *out_suffix_len = 3; return TypeKind_F64; }
  return TypeKind_Unresolved;
}

NumericAtomInfo
atom_classify_numeric(String8 atom) {
  NumericAtomInfo info = {0};
  if (atom.size == 0) return info;

  u64      suffix_len  = 0;
  TypeKind suffix_kind = numeric_suffix_kind(atom, &suffix_len);
  String8  body        = suffix_kind != TypeKind_Unresolved ? str8_chop(atom, suffix_len) : atom;
  b32      suffix_is_float = suffix_kind == TypeKind_F32 || suffix_kind == TypeKind_F64;

  // A hex body cannot take a float suffix, so if stripping one leaves
  // something that parses as hex, put it back and read the whole atom as one
  // hex integer: `0x1f32`, not `0x1` plus `f32`.
  if (suffix_is_float && atom_is_hex_literal(body)) {
    suffix_kind     = TypeKind_Unresolved;
    suffix_is_float = false;
    body            = atom;
  }

  if (atom_is_hex_literal(body)) {
    info.is_numeric   = true;
    info.is_hex       = true;
    info.explicit_type = suffix_kind;
    info.body         = body;
    return info;
  }

  u64 i = 0;
  if (body.size > 0 && (body.str[0] == '+' || body.str[0] == '-')) i = 1;
  b32 seen_digit = false;
  b32 seen_dot   = false;
  b32 seen_exp   = false;
  for (; i < body.size; i += 1) {
    u8 c = body.str[i];
    if (c >= '0' && c <= '9') { seen_digit = true; continue; }
    if (c == '.') { seen_dot = true; continue; }
    // Scientific notation: `1e10`, `1.5e-3`, `2E+8`. Always a float, as a '.'
    // is. A preceding digit is required so a bare `e10` stays an identifier.
    // The whole exponent tail is validated here rather than by continuing the
    // loop, so a malformed `1e` or `1e+` is rejected outright instead of
    // reading as `1`.
    if ((c == 'e' || c == 'E') && seen_digit) {
      u64 j = i + 1;
      if (j < body.size && (body.str[j] == '+' || body.str[j] == '-')) j += 1;
      if (j >= body.size) return info; // `1e` / `1e+` -- no exponent digits at all
      for (; j < body.size; j += 1) {
        if (body.str[j] < '0' || body.str[j] > '9') return info;
      }
      seen_exp = true;
      break; // the exponent tail is fully checked above -- nothing left to scan
    }
    return info; // stray character in the body -- not a numeric atom at all
  }
  if (!seen_digit) return info;
  // An integer suffix on a body that can only be a float -- `3.14i32` --
  // is rejected rather than silently truncated.
  if ((seen_dot || seen_exp) && suffix_kind != TypeKind_Unresolved && !suffix_is_float) return info;

  info.is_numeric   = true;
  info.is_float     = seen_dot || seen_exp || suffix_is_float;
  info.explicit_type = suffix_kind;
  info.body         = body;
  return info;
}

b32
atom_looks_numeric(String8 atom) {
  return atom_classify_numeric(atom).is_numeric;
}

b32
atom_is_float_literal(String8 atom) {
  return atom_classify_numeric(atom).is_float;
}

////////////////////////////////
//~ Reading a classified integer atom into a value
//
// strtoll/strtoull answer an out-of-range literal with the nearest
// representable value and errno == ERANGE, so calling them bare turns
// `99999999999999999999999` into i64's maximum and carries on -- an array
// count, an enum value or a plain literal silently becomes a number the
// source never mentioned. Everything that reads an integer atom goes through
// these two, which report the range failure instead of clamping into it.
//
// Both take the classified `body` (suffix already stripped) rather than the
// raw atom, so a trailing character means a malformed literal and not an
// ignored suffix -- hence the endptr check.

b32
atom_parse_i64(String8 body, b32 is_hex, i64* out) {
  const char* c    = cstr_from_str8_temp(body);
  char*       endp = NULL;
  errno            = 0;
  long long   v    = strtoll(c, &endp, is_hex ? 16 : 10);
  if (errno == ERANGE || endp == c || *endp != '\0') return false;
  *out = (i64)v;
  return true;
}

// A leading `-` is NOT a failure here: strtoull wraps it around modulo 2^64
// without setting ERANGE, which is exactly how `-1u64` is meant to read.
b32
atom_parse_u64(String8 body, b32 is_hex, u64* out) {
  const char*        c    = cstr_from_str8_temp(body);
  char*              endp = NULL;
  errno                   = 0;
  unsigned long long v    = strtoull(c, &endp, is_hex ? 16 : 10);
  if (errno == ERANGE || endp == c || *endp != '\0') return false;
  *out = (u64)v;
  return true;
}
