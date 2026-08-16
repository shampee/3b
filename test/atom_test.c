// atom_test.c -- the numeric-atom classifiers in atom.c: which atoms are
// numbers, which of those are floats, and which are hex.
//
// These three answer a question with no syntax behind it. The lexer and parser
// never look at an atom's shape, so `1e10` reaching the checker as a float and
// `e10` reaching it as an identifier is decided here and nowhere else. Three
// callers depend on the answer: format.c reads it to decide whether a manifest
// value is a number, lower.c to reject a `var` whose name would parse as one,
// and translate/cwalk.c to decide which `#define`s survive into a binding.
//
// Until this file the only exercise they had was a table in test/demo.c that
// PRINTED its verdicts and asserted nothing -- so `0x1e` classifying as a float,
// or `1e` as the integer 1, would have changed one line in 9,000 lines of
// output and failed no test.
//
// The interesting rows are the boundaries atom.c's own comments call out: a
// hex body that ends in something spelled like a float suffix (`0x1f32`), an
// integer suffix on a body that can only be a float (`3.14i32`), and an
// exponent with no digits after it (`1e`, `1e+`).
#include "3b.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

typedef struct AtomCase {
  const char* atom;
  b32         numeric;
  b32         is_float;
  b32         is_hex;
  const char* why; // only where the answer isn't obvious from the spelling
} AtomCase;

static const AtomCase g_cases[] = {
  // ~~ Plain integers and floats, the common path.
  { "10",       true,  false, false, NULL },
  { "-3",       true,  false, false, NULL },
  { "+7",       true,  false, false, NULL },
  { "0",        true,  false, false, NULL },
  { "0.5",      true,  true,  false, NULL },
  { "3.14159",  true,  true,  false, NULL },
  { "-0.25",    true,  true,  false, NULL },

  // ~~ Not numbers at all. `+` matters: it is an operator name, and a
  // classifier that reads a lone sign as a number would swallow it.
  { "count",    false, false, false, NULL },
  { "+",        false, false, false, "a bare sign is an operator name, not a number" },
  { "-",        false, false, false, NULL },
  { "",         false, false, false, "empty atom -- the size==0 early out" },
  { "1two",     false, false, false, "a stray character anywhere means not numeric" },

  // ~~ Scientific notation is a float, exactly as a '.' is.
  { "1e10",     true,  true,  false, NULL },
  { "1.5e-3",   true,  true,  false, NULL },
  { "2E+8",     true,  true,  false, "capital E, and a signed exponent" },
  { "e10",      false, false, false, "no leading digit -- stays an identifier" },
  { "1e",       false, false, false, "no exponent digits: must be rejected outright, NOT read as 1" },
  { "1e+",      false, false, false, "same, with the sign present but no digits" },
  { "1e2e3",    false, false, false, "the exponent tail is fully validated, not scanned loosely" },

  // ~~ Hex. A hex atom is an integer -- there is no hex float syntax -- and
  // the 'e' in `0x1e` is a digit, not an exponent marker.
  { "0x1e",     true,  false, true,  "`e` here is a hex DIGIT; reading it as an exponent would make this a float" },
  { "0XFF",     true,  false, true,  "uppercase prefix" },
  { "-0x10",    true,  false, true,  "sign before the prefix" },
  { "0x",       false, false, false, "prefix with no digits is not a literal" },
  { "0xZZ",     false, false, false, "non-hex digits after the prefix" },

  // ~~ Type suffixes pin the type; the classifier still has to see through
  // them to the body.
  { "20i8",     true,  false, false, NULL },
  { "255u8",    true,  false, false, NULL },
  { "3.14f32",  true,  true,  false, NULL },
  { "1e10f64",  true,  true,  false, "float suffix on an exponent body" },
  { "3.14i32",  false, false, false, "an integer suffix on a float body is rejected, not truncated" },
  { "0x1f32",   true,  false, true,  "reads as ONE hex integer, not `0x1` with an `f32` suffix" },
};

static void
check(const AtomCase* c) {
  String8 a = str8_cstring((char*)c->atom);

  struct { const char* name; b32 got; b32 want; } probes[] = {
    { "atom_looks_numeric",    atom_looks_numeric(a),    c->numeric  },
    { "atom_is_float_literal", atom_is_float_literal(a), c->is_float },
    { "atom_is_hex_literal",   atom_is_hex_literal(a),   c->is_hex   },
  };

  for (u64 i = 0; i < ArrayCount(probes); i += 1) {
    g_checks += 1;
    if (probes[i].got == probes[i].want) continue;
    fprintf(stderr, "FAIL %s(\"%s\"): want %s, got %s\n", probes[i].name, c->atom,
            probes[i].want ? "true" : "false", probes[i].got ? "true" : "false");
    if (c->why) fprintf(stderr, "     (%s)\n", c->why);
    g_failures += 1;
  }
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(8)); // cstr_from_str8_temp pushes onto scratch

  for (u64 i = 0; i < ArrayCount(g_cases); i += 1) check(&g_cases[i]);

  if (g_failures == 0) printf("atom_test: all checks passed (%d checks)\n", g_checks);
  else                 printf("atom_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
