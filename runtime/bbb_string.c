////////////////////////////////
//~ Range helper (bbb_str8_substr's clamp math)

static bbb_Rng1u64
rng_1u64(u64 min, u64 max) {
  bbb_Rng1u64 r = { min, max };
  if (r.min > r.max) bbb_Swap(u64, r.min, r.max);
  return r;
}

static u64
dim_1u64(bbb_Rng1u64 r) {
  return (r.max > r.min) ? (r.max - r.min) : 0;
}

////////////////////////////////
//~ Character classification (needed by bbb_str8_match's flags)

static b32 char_is_lower(u8 c)         { return ('a' <= c && c <= 'z'); }
static b32 char_is_slash(u8 c)         { return (c == '/' || c == '\\'); }
static u8  char_to_upper(u8 c)         { return char_is_lower(c) ? (u8)(c + ('A' - 'a')) : c; }
static u8  char_to_correct_slash(u8 c) { return char_is_slash(c) ? (u8)'/' : c; }

static u64
cstring8_length(u8* c) {
  u8* p = c;
  for (; *p != 0; p += 1);
  return (u64)(p - c);
}

////////////////////////////////
//~ Strings

bbb_String8
bbb_str8(u8* str, u64 size) {
  return (bbb_String8){ str, size };
}

bbb_String8
bbb_str8_range(u8* first, u8* one_past_last) {
  return (bbb_String8){ first, (u64)(one_past_last - first) };
}

bbb_String8
bbb_str8_cstring(char* c) {
  return (bbb_String8){ (u8*)c, cstring8_length((u8*)c) };
}

// FNV-1a -- same algorithm the compiler's own internal (String8-only,
// compile-time) hash table uses, exposed here for generated programs so a
// `(Map string V)`/`(Set string)` instantiation's monomorphized hash table
// (see bbb_hashtable.h) can call it.
u64
bbb_str8_hash(bbb_String8 s) {
  u64 h = 1469598103934665603ull;
  bbb_foreach_index(i, s.size) {
    h ^= (u8)s.str[i];
    h *= 1099511628211ull;
  }
  return h;
}

b32
bbb_str8_match(bbb_String8 a, bbb_String8 b, bbb_StringMatchFlags flags) {
  b32 result = 0;
  if (a.size == b.size && flags == 0) {
    result = bbb_MemoryMatch(a.str, b.str, b.size);
  } else if (a.size == b.size || (flags & bbb_StringMatchFlag_RightSideSloppy)) {
    b32 case_insensitive  = (flags & bbb_StringMatchFlag_CaseInsensitive);
    b32 slash_insensitive = (flags & bbb_StringMatchFlag_SlashInsensitive);
    u64 size              = bbb_Min(a.size, b.size);
    result                = 1;
    bbb_foreach_index(i, size) {
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

i32
bbb_str8_compare(bbb_String8 a, bbb_String8 b) {
  u64 size = bbb_Min(a.size, b.size);
  bbb_foreach_index(i, size) {
    if (a.str[i] != b.str[i]) return (i32)a.str[i] - (i32)b.str[i];
  }
  if (a.size != b.size) return a.size < b.size ? -1 : 1;
  return 0;
}

bbb_String8
bbb_str8_prefix(bbb_String8 str, u64 size) {
  str.size = bbb_ClampTop(size, str.size);
  return str;
}

bbb_String8
bbb_str8_skip(bbb_String8 str, u64 amt) {
  amt = bbb_ClampTop(amt, str.size);
  str.str += amt;
  str.size -= amt;
  return str;
}

bbb_String8
bbb_str8_postfix(bbb_String8 str, u64 size) {
  size     = bbb_ClampTop(size, str.size);
  str.str  = (str.str + str.size) - size;
  str.size = size;
  return str;
}

bbb_String8
bbb_str8_chop(bbb_String8 str, u64 amt) {
  amt = bbb_ClampTop(amt, str.size);
  str.size -= amt;
  return str;
}

bbb_String8
bbb_str8_substr(bbb_String8 str, bbb_Rng1u64 range) {
  range.min = bbb_ClampTop(range.min, str.size);
  range.max = bbb_ClampTop(range.max, str.size);
  str.str += range.min;
  str.size = dim_1u64(range);
  return str;
}

u64
bbb_str8_find_needle(bbb_String8 string, u64 start_pos, bbb_String8 needle, bbb_StringMatchFlags flags) {
  u8* p           = string.str + start_pos;
  u64 stop_offset = bbb_Max(string.size + 1, needle.size) - needle.size;
  u8* stop_p      = string.str + stop_offset;
  if (needle.size > 0) {
    u8*                  string_opl     = string.str + string.size;
    bbb_String8          needle_tail    = bbb_str8_skip(needle, 1);
    bbb_StringMatchFlags adjusted_flags = flags | bbb_StringMatchFlag_RightSideSloppy;
    u8                   needle_first_char_adjusted = needle.str[0];
    if (adjusted_flags & bbb_StringMatchFlag_CaseInsensitive) {
      needle_first_char_adjusted = char_to_upper(needle_first_char_adjusted);
    }
    for (; p < stop_p; p += 1) {
      u8 haystack_char_adjusted = *p;
      if (adjusted_flags & bbb_StringMatchFlag_CaseInsensitive) {
        haystack_char_adjusted = char_to_upper(haystack_char_adjusted);
      }
      if (haystack_char_adjusted == needle_first_char_adjusted) {
        if (bbb_str8_match(bbb_str8_range(p + 1, string_opl), needle_tail, adjusted_flags)) {
          break;
        }
      }
    }
  }
  u64 result = string.size;
  if (p < stop_p) result = (u64)(p - string.str);
  return result;
}

u64
bbb_str8_find_needle_reverse(bbb_String8 string, u64 start_pos, bbb_String8 needle, bbb_StringMatchFlags flags) {
  u64 result = 0;
  u64 needed = start_pos + needle.size;
  u64 count  = (needed <= string.size) ? (string.size - needed) : 0; // clamp instead of
                                                                      // underflowing when the
                                                                      // needle can't possibly fit
  bbb_foreach_index_reverse(i, count) {
    bbb_String8 haystack = bbb_str8_substr(string, rng_1u64(i, i + needle.size));
    if (bbb_str8_match(haystack, needle, flags)) {
      result = (u64)i + needle.size;
      break;
    }
  }
  return result;
}

b32
bbb_str8_starts_with(bbb_String8 string, bbb_String8 start, bbb_StringMatchFlags flags) {
  return bbb_str8_match(start, bbb_str8_prefix(string, start.size), flags);
}

b32
bbb_str8_ends_with(bbb_String8 string, bbb_String8 end, bbb_StringMatchFlags flags) {
  return bbb_str8_match(end, bbb_str8_postfix(string, end.size), flags);
}

bbb_String8
bbb_str8_cat(bbb_Arena* arena, bbb_String8 s1, bbb_String8 s2) {
  bbb_String8 str;
  str.size = s1.size + s2.size;
  str.str  = bbb_push_array(arena, u8, str.size + 1);
  bbb_MemoryCopy(str.str, s1.str, s1.size);
  bbb_MemoryCopy(str.str + s1.size, s2.str, s2.size);
  str.str[str.size] = 0;
  return str;
}

bbb_String8
bbb_str8_copy(bbb_Arena* arena, bbb_String8 s) {
  bbb_String8 str;
  str.size = s.size;
  str.str  = bbb_push_array(arena, u8, str.size + 1);
  bbb_MemoryCopy(str.str, s.str, s.size);
  str.str[str.size] = 0;
  return str;
}

char*
bbb_cstring_str8(bbb_Arena* arena, bbb_String8 s) {
  char* out = bbb_push_array(arena, char, s.size + 1);
  bbb_MemoryCopy(out, s.str, s.size);
  out[s.size] = 0;
  return out;
}

bbb_String8
bbb_str8fv(bbb_Arena* arena, char* fmt, va_list args) {
  va_list args2;
  va_copy(args2, args);
  // vsnprintf returns NEGATIVE on an output error (an encoding error, per C99);
  // the `+ 1` used to wrap that to 0 in a u32, so nothing was allocated and the
  // second call's own negative return became a huge u64 `.size`, whose
  // `result.str[result.size] = 0` wrote far past the arena. An empty string is
  // the right answer instead: this formats a message, and there is no message.
  int needed = vsnprintf(0, 0, fmt, args);
  if (needed < 0) { va_end(args2); return (bbb_String8){0}; }

  u32         needed_bytes = (u32)needed + 1;
  bbb_String8 result       = { 0 };
  result.str               = bbb_push_array(arena, u8, needed_bytes);
  result.size              = (u64)needed; // the sizing call above already measured it; re-reading the
                                          // second call's return would reintroduce the same signed wrap
  vsnprintf((char*)result.str, needed_bytes, fmt, args2);
  result.str[result.size]  = 0;
  va_end(args2);
  return result;
}

bbb_String8
bbb_str8f(bbb_Arena* arena, char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  bbb_String8 result = bbb_str8fv(arena, fmt, args);
  va_end(args);
  return result;
}

////////////////////////////////
//~ `print`/`str` bridges

bool
bbb_string_match(bbb_String8 a, bbb_String8 b, u32 flags) {
  return (bool)bbb_str8_match(a, b, (bbb_StringMatchFlags)flags);
}

// NOTE: bbb_str8_find_needle(_reverse) takes `(string, start_pos, needle,
// flags)` -- `needle` and `start_pos` are reordered here to put `needle`
// right after the string it's searched in, matching this bridge's own (and
// the language builtin's) more readable argument order.
u64
bbb_string_find(bbb_String8 s, bbb_String8 needle, u64 start_pos, u32 flags) {
  return bbb_str8_find_needle(s, start_pos, needle, (bbb_StringMatchFlags)flags);
}

u64
bbb_string_find_reverse(bbb_String8 s, bbb_String8 needle, u64 start_pos, u32 flags) {
  return bbb_str8_find_needle_reverse(s, start_pos, needle, (bbb_StringMatchFlags)flags);
}

bool
bbb_string_starts_with(bbb_String8 s, bbb_String8 prefix, u32 flags) {
  return (bool)bbb_str8_starts_with(s, prefix, (bbb_StringMatchFlags)flags);
}

bool
bbb_string_ends_with(bbb_String8 s, bbb_String8 suffix, u32 flags) {
  return (bool)bbb_str8_ends_with(s, suffix, (bbb_StringMatchFlags)flags);
}

////////////////////////////////
//~ `string-to-i32`/`string-to-i64`/`string-to-u32`/`string-to-u64`/
// `string-to-f32`/`string-to-f64` builtins -- see bbb_string.h's own
// comment on this whole section for the "why hand-rolled, not strtoll"
// reasoning and the exact failure contract.

bbb_ParseI64Result
bbb_string_to_i64(bbb_String8 s) {
  bbb_ParseI64Result r = {0};
  u64 i   = 0;
  b32 neg = 0;
  if (i < s.size && (s.str[i] == '-' || s.str[i] == '+')) { neg = (s.str[i] == '-'); i += 1; }
  if (i >= s.size) return r; // empty, or just a lone sign
  u64 mag         = 0;
  u64 digit_count = 0;
  for (; i < s.size; i += 1) {
    u8 c = s.str[i];
    if (c < '0' || c > '9') return r; // a non-digit before the string ends -- no partial parse
    // Overflow guard: `mag` (a u64) is the UNSIGNED magnitude regardless of
    // sign, and it has to be bounded BEFORE each multiply, not just at the
    // end -- a 20-digit input can otherwise wrap past U64_MAX and land back
    // under I64_MAX, passing the final check and reporting ok=true for a
    // magnitude the string never named. 2^63 is the largest either polarity
    // of i64 can represent, so it is the ceiling to accumulate against.
    u64 digit = (u64)(c - '0');
    if (mag > (9223372036854775808ull - digit) / 10) return r;
    mag          = mag * 10 + digit;
    digit_count += 1;
    if (digit_count > 20) return r; // already more digits than any i64 magnitude could need
  }
  if (digit_count == 0) return r;
  if (!neg && mag > 9223372036854775807ull) return r;         // > I64_MAX; `neg` may use the full 2^63
  r.ok    = 1;
  r.value = neg ? -(i64)mag : (i64)mag;
  return r;
}

bbb_ParseI32Result
bbb_string_to_i32(bbb_String8 s) {
  bbb_ParseI32Result r   = {0};
  bbb_ParseI64Result r64 = bbb_string_to_i64(s);
  if (!r64.ok || r64.value < -2147483648ll || r64.value > 2147483647ll) return r;
  r.ok    = 1;
  r.value = (i32)r64.value;
  return r;
}

bbb_ParseU64Result
bbb_string_to_u64(bbb_String8 s) {
  bbb_ParseU64Result r = {0};
  u64 i = 0;
  if (i < s.size && s.str[i] == '+') { i += 1; } // no `-` at all -- unsigned
  if (i >= s.size) return r;
  u64 mag         = 0;
  u64 digit_count = 0;
  for (; i < s.size; i += 1) {
    u8 c = s.str[i];
    if (c < '0' || c > '9') return r;
    u64 next = mag * 10 + (u64)(c - '0');
    if (next < mag) return r; // wrapped past U64_MAX
    mag          = next;
    digit_count += 1;
  }
  if (digit_count == 0) return r;
  r.ok    = 1;
  r.value = mag;
  return r;
}

bbb_ParseU32Result
bbb_string_to_u32(bbb_String8 s) {
  bbb_ParseU32Result r   = {0};
  bbb_ParseU64Result r64 = bbb_string_to_u64(s);
  if (!r64.ok || r64.value > 4294967295ull) return r;
  r.ok    = 1;
  r.value = (u32)r64.value;
  return r;
}

// Float parsing goes through strtod/strtof rather than a hand-rolled
// decimal<->binary conversion (see bbb_string.h's own comment) -- `s` is
// first copied into a small, guaranteed-null-terminated stack buffer so
// strtod/strtof never reads past what `s` actually points at (a
// bbb_String8 is a fat-pointer VIEW, not guaranteed null-terminated at
// `s.str[s.size]`). Anything too long to plausibly be a real numeric
// literal (`>= 64` bytes) is rejected outright rather than truncated.
bbb_ParseF64Result
bbb_string_to_f64(bbb_String8 s) {
  bbb_ParseF64Result r = {0};
  if (s.size == 0 || s.size >= 64) return r;
  char buf[64];
  bbb_MemoryCopy(buf, s.str, s.size);
  buf[s.size] = 0;
  char* endptr = 0;
  f64   v      = strtod(buf, &endptr);
  if (endptr != buf + s.size) return r; // trailing garbage, or nothing consumed at all
  r.ok    = 1;
  r.value = v;
  return r;
}

bbb_ParseF32Result
bbb_string_to_f32(bbb_String8 s) {
  bbb_ParseF32Result r = {0};
  if (s.size == 0 || s.size >= 64) return r;
  char buf[64];
  bbb_MemoryCopy(buf, s.str, s.size);
  buf[s.size] = 0;
  char* endptr = 0;
  f32   v      = strtof(buf, &endptr);
  if (endptr != buf + s.size) return r;
  r.ok    = 1;
  r.value = v;
  return r;
}
