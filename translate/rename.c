// Pattern-driven renaming: the engine behind `(rename-const-pattern "regex"
// "template")` and its func/type siblings. See translate.h's RenamePattern and
// rename_patterns_apply for the surface these implement.
//
// A C API's constants are usually named mechanically -- one shared prefix,
// SCREAMING_SNAKE words -- and the 3b spelling wanted for them is mechanical
// too. A regex plus a substitution template states that relationship once,
// where `(rename-const [...])` needs one line per constant.
//
// What a pattern cannot do is be right about every member: `MPEG1VIDEO` carries
// no separator saying where `Mpeg1` ends and `Video` begins. That is why
// explicit renames still exist and still win; see rename_lookup's callers in
// emit.c.
#include "translate.h"
#include <regex.h>
#include <stdarg.h>

// POSIX regexec's pmatch array: index 0 is the whole match, 1..9 are capture
// groups. Templates address groups by the same numbers, so one cap covers
// both.
#define RENAME_MAX_GROUPS 10

////////////////////////////////
//~ Compiled-regex cache

// Every constant is tested against every pattern, so compiling per test costs
// O(constants * patterns) regcomps -- thousands on a header like libavcodec's.
// Patterns are few, so a linear-scanned cache keyed by pattern text suffices.
// The translator is one-shot, so this lives process-wide rather than in
// EmitState.
typedef struct CompiledRe {
  String8 pattern;
  regex_t re;
  b32     valid; // false = regcomp failed; kept in the cache anyway so the
                  // report the diagnostic once, not once per name tested
} CompiledRe;

static CompiledRe* g_re_cache; // dyn array, ctx_perm()

static CompiledRe*
compiled_regex(String8 pattern) {
  foreach_index(i, dyn_count(g_re_cache)) {
    if (str8_match(g_re_cache[i].pattern, pattern, 0)) return &g_re_cache[i];
  }
  CompiledRe entry = {0};
  entry.pattern    = str8_copy(ctx_perm(), pattern);
  int rc           = regcomp(&entry.re, cstr_from_str8_temp(pattern), REG_EXTENDED);
  entry.valid      = (rc == 0);
  if (!entry.valid) {
    fprintf(stderr, "translate: invalid regex pattern '%.*s' -- treating as no-match\n", str8_varg(pattern));
  }
  dyn_push(ctx_perm(), g_re_cache, entry);
  return &g_re_cache[dyn_count(g_re_cache) - 1];
}

b32
rename_regex_matches(String8 pattern, String8 name) {
  CompiledRe* c = compiled_regex(pattern);
  if (!c->valid) return false;
  return regexec(&c->re, cstr_from_str8_temp(name), 0, NULL, 0) == 0;
}

////////////////////////////////
//~ Word splitting + case styles

typedef enum NameStyle {
  NameStyle_Verbatim, // `{N}` with no `:style` -- the capture, untouched
  NameStyle_Pascal,
  NameStyle_Camel,
  NameStyle_Snake,
  NameStyle_Kebab,
  NameStyle_Upper, // plain ASCII case map, NOT word-based (see translate.h)
  NameStyle_Lower,
} NameStyle;

static b32
style_from_text(String8 s, NameStyle* out) {
  if (s.size == 0)                      { *out = NameStyle_Verbatim; return true; }
  if (str8_match_lit("pascal", s, 0))   { *out = NameStyle_Pascal;   return true; }
  if (str8_match_lit("camel", s, 0))    { *out = NameStyle_Camel;    return true; }
  if (str8_match_lit("snake", s, 0))    { *out = NameStyle_Snake;    return true; }
  if (str8_match_lit("kebab", s, 0))    { *out = NameStyle_Kebab;    return true; }
  if (str8_match_lit("upper", s, 0))    { *out = NameStyle_Upper;    return true; }
  if (str8_match_lit("lower", s, 0))    { *out = NameStyle_Lower;    return true; }
  if (str8_match_lit("verbatim", s, 0)) { *out = NameStyle_Verbatim; return true; }
  return false;
}

typedef enum RCharKind { RCharKind_Lower, RCharKind_Upper, RCharKind_Digit, RCharKind_Other } RCharKind;

static RCharKind
rchar_kind(u8 c) {
  if (c >= 'a' && c <= 'z') return RCharKind_Lower;
  if (c >= 'A' && c <= 'Z') return RCharKind_Upper;
  if (c >= '0' && c <= '9') return RCharKind_Digit;
  return RCharKind_Other;
}

static u8 rchar_lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c - 'A' + 'a') : c; }
static u8 rchar_upper(u8 c) { return (c >= 'a' && c <= 'z') ? (u8)(c - 'a' + 'A') : c; }

// Splits `s` into words on non-alphanumeric separators and on case boundaries,
// reproducing emit.c's camel_to_kebab rule so a `{1:kebab}` template and the
// translator's default function naming agree on where a word starts:
//   - a run of uppercase stays one word until the last letter before a
//     lowercase one (`XMLParser` -> `XML` `Parser`)
//   - digits attach to the word before them (`S16LE` -> `S16le`, not
//     `S` `16` `LE`), which is what makes codec names read right
// Returns a dyn array of views into `s`, empty when `s` has no alphanumerics.
static String8*
split_words(Arena* arena, String8 s) {
  String8*  words      = NULL;
  u64       word_start = (u64)-1;
  RCharKind prev       = RCharKind_Other;
  foreach_index(i, s.size) {
    RCharKind kind = rchar_kind(s.str[i]);
    if (kind == RCharKind_Other) {
      if (word_start != (u64)-1) {
        dyn_push(arena, words, str8(s.str + word_start, i - word_start));
        word_start = (u64)-1;
      }
      prev = kind;
      continue;
    }
    b32 boundary = false;
    if (word_start != (u64)-1 && kind == RCharKind_Upper) {
      RCharKind next = (i + 1 < s.size) ? rchar_kind(s.str[i + 1]) : RCharKind_Other;
      boundary = (prev == RCharKind_Lower) || (prev == RCharKind_Upper && next == RCharKind_Lower);
    }
    if (boundary) {
      dyn_push(arena, words, str8(s.str + word_start, i - word_start));
      word_start = i;
    } else if (word_start == (u64)-1) {
      word_start = i;
    }
    prev = kind;
  }
  if (word_start != (u64)-1) dyn_push(arena, words, str8(s.str + word_start, s.size - word_start));
  return words;
}

static String8
apply_style(Arena* arena, NameStyle style, String8 s) {
  if (style == NameStyle_Verbatim) return s;

  if (style == NameStyle_Upper || style == NameStyle_Lower) {
    // Not word-based: `upper`/`lower` map the case of whatever the group
    // captured, separators included. Splitting into words here would make
    // `{1:lower}` silently drop separators.
    u8* buf = push_array(arena, u8, s.size > 0 ? s.size : 1);
    foreach_index(i, s.size) buf[i] = (style == NameStyle_Upper) ? rchar_upper(s.str[i]) : rchar_lower(s.str[i]);
    return str8(buf, s.size);
  }

  // The word list comes from `arena`, not a scratch temp: `arena` may itself be
  // the scratch arena at some call site, where releasing a temp mark would free
  // the result buffer out from under the caller. A handful of views left in a
  // one-shot process costs nothing.
  String8* words  = split_words(arena, s);
  u64      nwords = dyn_count(words);
  if (nwords == 0) return str8_lit("");

  // Separators are dropped and at most one reinserted per pair of words, so the
  // output cannot exceed the input's length plus the number of joins.
  u8* buf = push_array(arena, u8, s.size + nwords);
  u64 out = 0;
  foreach_index(wi, nwords) {
    String8 w = words[wi];
    if (wi > 0 && (style == NameStyle_Snake || style == NameStyle_Kebab)) {
      buf[out] = (style == NameStyle_Snake) ? '_' : '-';
      out += 1;
    }
    foreach_index(ci, w.size) {
      // Capitalize a word's first character only, and only if it is a letter:
      // a word starting with a digit (`8SVX`, `4XM`) keeps the digit and
      // lowercases the rest, giving `8svx` rather than a stray capital.
      b32 capitalize = (ci == 0) && (style == NameStyle_Pascal || (style == NameStyle_Camel && wi > 0));
      buf[out] = capitalize ? rchar_upper(w.str[ci]) : rchar_lower(w.str[ci]);
      out += 1;
    }
  }
  return str8(buf, out);
}

////////////////////////////////
//~ Template expansion

// A malformed template is a property of the config, but is only discovered
// while expanding against a particular name, so it would otherwise report once
// per matching name and bury every other diagnostic. Templates already reported
// are remembered here. Keyed on template text, not pattern: two rules may share
// a regex and differ only in their replacement.
static String8* g_reported_templates; // dyn array, ctx_perm()

static b32
template_error_already_reported(String8 replacement) {
  foreach_index(i, dyn_count(g_reported_templates)) {
    if (str8_match(g_reported_templates[i], replacement, 0)) return true;
  }
  dyn_push(ctx_perm(), g_reported_templates, str8_copy(ctx_perm(), replacement));
  return false;
}

static b32
template_error(String8 replacement, const char* fmt, ...) {
  if (!template_error_already_reported(replacement)) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "translate: rename template '%.*s' -- ", str8_varg(replacement));
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
  }
  return false; // always "this rule didn't apply", for callers to return directly
}

// Expands one `replacement` template against one successful regexec. `offsets`
// indexes into `name`: regexec ran on a NUL-terminated copy of exactly those
// bytes, so the offsets carry over. Returns false, having reported why, on a
// malformed template or a reference to a group the pattern lacks; callers treat
// that as the rule not applying, so one bad rule cannot rename everything to
// garbage.
static b32
expand_template(Arena* arena, String8 replacement, String8 name, regmatch_t* offsets, String8* out) {
  String8List parts   = {0};
  StringJoin  no_join = {0};
  u64         lit     = 0; // start of the pending run of literal template text
  u64         i       = 0;

  while (i < replacement.size) {
    // `}}` collapses to one `}` for symmetry with `{{`. A lone `}` is already
    // literal, but anyone who wrote `{{` will reach for `}}` to match.
    if (replacement.str[i] == '}' && i + 1 < replacement.size && replacement.str[i + 1] == '}') {
      if (i > lit) str8_list_push(arena, &parts, str8(replacement.str + lit, i - lit));
      str8_list_push(arena, &parts, str8_lit("}"));
      i  += 2;
      lit = i;
      continue;
    }
    if (replacement.str[i] != '{') { i += 1; continue; }
    if (i > lit) str8_list_push(arena, &parts, str8(replacement.str + lit, i - lit));

    if (i + 1 < replacement.size && replacement.str[i + 1] == '{') { // `{{` -- literal brace
      str8_list_push(arena, &parts, str8_lit("{"));
      i  += 2;
      lit = i;
      continue;
    }

    u64 close = i + 1;
    while (close < replacement.size && replacement.str[close] != '}') close += 1;
    if (close >= replacement.size) {
      return template_error(replacement, "unterminated `{` (use `{{` for a literal brace)");
    }

    String8 body       = str8(replacement.str + i + 1, close - i - 1);
    String8 index_text = body;
    String8 style_text = {0};
    foreach_index(k, body.size) {
      if (body.str[k] == ':') {
        index_text = str8(body.str, k);
        style_text = str8(body.str + k + 1, body.size - k - 1);
        break;
      }
    }

    if (index_text.size == 0) return template_error(replacement, "a `{}` with no capture-group number");
    u32 group = 0;
    foreach_index(k, index_text.size) {
      if (index_text.str[k] < '0' || index_text.str[k] > '9') {
        return template_error(replacement, "`{%.*s}` is not a capture-group number", str8_varg(index_text));
      }
      group = group * 10 + (u32)(index_text.str[k] - '0');
    }

    NameStyle style;
    if (!style_from_text(style_text, &style)) {
      return template_error(replacement, "unknown style `%.*s` (pascal, camel, snake, kebab, upper, lower, verbatim)",
                            str8_varg(style_text));
    }

    if (group >= RENAME_MAX_GROUPS || offsets[group].rm_so < 0) {
      return template_error(replacement, "refers to `{%u}`, but the pattern has no such capture group", group);
    }
    String8 captured = str8(name.str + offsets[group].rm_so, (u64)(offsets[group].rm_eo - offsets[group].rm_so));
    str8_list_push(arena, &parts, apply_style(arena, style, captured));

    i  = close + 1;
    lit = i;
  }
  if (replacement.size > lit) str8_list_push(arena, &parts, str8(replacement.str + lit, replacement.size - lit));

  *out = str8_list_join(arena, &parts, &no_join);
  return true;
}

String8
rename_patterns_apply(Arena* arena, RenamePattern* patterns, String8 name) {
  foreach_index(i, dyn_count(patterns)) {
    CompiledRe* c = compiled_regex(patterns[i].pattern);
    if (!c->valid) continue;
    regmatch_t offsets[RENAME_MAX_GROUPS];
    if (regexec(&c->re, cstr_from_str8_temp(name), RENAME_MAX_GROUPS, offsets, 0) != 0) continue;

    String8 renamed;
    if (!expand_template(arena, patterns[i].replacement, name, offsets, &renamed)) continue;
    if (renamed.size == 0) {
      fprintf(stderr, "translate: rename pattern '%.*s' produced an empty name for '%.*s' -- ignoring the rule here\n",
              str8_varg(patterns[i].pattern), str8_varg(name));
      continue;
    }
    return renamed;
  }
  return str8_lit("");
}
