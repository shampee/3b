// checker_error_test.c -- the checker's NEGATIVE tests: sources that must be
// rejected, each pinned to the message and the source position it must be
// rejected at.
//
// Until this file, checker.c had no assert-based test of any kind. The
// established convention was test/demo.c's `run_source` print harness plus
// eyeballing a diff of its output -- which catches a diagnostic that
// disappears, but only if someone reads the diff, and says nothing about
// whether a diagnostic still points at the right token. A rule that stops
// firing is the failure mode that matters here: it is silent, and the
// program it should have rejected goes on to codegen.
//
// Cases run through the same three stages compile_package does -- parse,
// lower, check -- with diag_capture_begin/end collecting every diagnostic as
// data. Lowering errors are in scope deliberately: `(enum E [A
// 99999999999999999999999])` is caught in lower.c, not checker.c, and a
// caller writing that line does not care which file rejected it.
//
// Position is asserted, not just the message. `expect_line`/`expect_col` of 0
// means "don't check", used only where the position is genuinely incidental.
//
// The ACCEPT table at the bottom is not decoration. Several rules here have a
// narrow correct boundary -- an integer literal check that rejects `-5u64` but
// must accept `18446744073709551615u64`, a cast check that blocks struct casts
// but must allow a same-type no-op and any cast to `void` -- and a negative
// test alone is satisfied by a rule that rejects everything.
#include "3b.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

enum { CHK_ARENA = MB(32) };

////////////////////////////////
//~ Running one source through parse -> lower -> check with capture on.

typedef struct CheckOutcome {
  b32 errored;      // any stage reported at least one diagnostic
  u64 count;
  // Diagnostics are arena-allocated and die with the Context, so the fields the
  // assertions need are copied out into fixed buffers first.
  char messages[24][512];
  u32  lines[24];
  u32  cols[24];
  u64  kept; // how many of `count` fit in the arrays above
} CheckOutcome;

static CheckOutcome
run_source(const char* src_cstr, const char* name) {
  CheckOutcome out = {0};

  Context ctx;
  ctx_init(&ctx, CHK_ARENA);
  source_registry_reset();

  String8 src     = str8_cstring((char*)src_cstr);
  u32     file_id = source_file_register(str8_cstring((char*)name), src);

  diag_capture_begin(/*also_print=*/false);

  Ast ast;
  ast_init(&ast, ctx_perm());
  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);

  if (!p.had_error) {
    // Strip the leading `(package ...)` form the way compile_package's
    // validate_and_strip_package_form does. No fixture here imports anything.
    u16        form_count;
    NodeIndex* forms      = ast_seq_children(&ast, root, &form_count);
    Token      synth_open = {0};
    synth_open.line       = 1;
    synth_open.col        = 1;
    NodeIndex body =
        ast_push_seq(&ast, AstNodeKind_List, synth_open, forms + 1, (u16)(form_count - 1));

    TypedAst tast;
    typed_ast_init(&tast, ctx_perm());
    Lowerer low = {0};
    low.ast     = &ast;
    low.tast    = &tast;
    TypedIndex own_root = lower_program(&low, body);

    // Check even when lowering reported something: a lowering error does not
    // necessarily stop the checker having its own, and a fixture asserting a
    // checker message should not be defeated by an unrelated earlier one.
    if (!low.had_error) {
      Checker ck = check_program(&tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
      (void)ck;
    }
  }

  u64         n     = 0;
  Diagnostic* diags = diag_capture_end(&n);
  out.count         = n;
  out.errored       = (n > 0);
  for (u64 i = 0; i < n && i < 24; i += 1) {
    u64 len = diags[i].message.size;
    if (len > sizeof(out.messages[0]) - 1) len = sizeof(out.messages[0]) - 1;
    memcpy(out.messages[i], diags[i].message.str, len);
    out.messages[i][len] = 0;
    out.lines[i]         = diags[i].line;
    out.cols[i]          = diags[i].col;
    out.kept += 1;
  }

  ctx_free();
  return out;
}

////////////////////////////////
//~ The reject table.

typedef struct RejectCase {
  const char* name;
  const char* source;
  const char* expect_msg;  // substring that must appear in some diagnostic
  u32         expect_line; // 0 = don't assert
  u32         expect_col;  // 0 = don't assert
} RejectCase;

// Set CHECKER_TEST_SHOW=1 to dump every case's actual diagnostics, positions
// included. Adding a case means writing down a line:col, and reading it off a
// run is more reliable than deriving it from the fixture by hand.
static b32 g_show = false;

static void
check_reject(const RejectCase* c) {
  g_checks += 1;
  CheckOutcome o = run_source(c->source, "neg.3b");

  if (g_show) {
    fprintf(stderr, "-- %s\n", c->name);
    for (u64 i = 0; i < o.kept; i += 1) {
      fprintf(stderr, "     %u:%u %s\n", o.lines[i], o.cols[i], o.messages[i]);
    }
  }

  if (!o.errored) {
    fprintf(stderr, "FAIL %s: expected a diagnostic containing \"%s\", got NONE -- the source was ACCEPTED\n",
            c->name, c->expect_msg);
    g_failures += 1;
    return;
  }

  for (u64 i = 0; i < o.kept; i += 1) {
    if (strstr(o.messages[i], c->expect_msg) == NULL) continue;

    b32 line_ok = (c->expect_line == 0) || (o.lines[i] == c->expect_line);
    b32 col_ok  = (c->expect_col == 0) || (o.cols[i] == c->expect_col);
    if (line_ok && col_ok) return; // matched message AND position

    fprintf(stderr, "FAIL %s: right message, wrong position -- want %u:%u, got %u:%u\n    %s\n",
            c->name, c->expect_line, c->expect_col, o.lines[i], o.cols[i], o.messages[i]);
    g_failures += 1;
    return;
  }

  fprintf(stderr, "FAIL %s: no diagnostic contained \"%s\". Got %llu:\n",
          c->name, c->expect_msg, (unsigned long long)o.count);
  for (u64 i = 0; i < o.kept; i += 1) {
    fprintf(stderr, "    %u:%u %s\n", o.lines[i], o.cols[i], o.messages[i]);
  }
  g_failures += 1;
}

// Cases whose ordering follows the checker's own concerns rather than the
// order rules were added in. The four groups called out below are the ones
// added by recent maturity work with no regression test behind them at all --
// the reason this file exists.
static const RejectCase g_reject_cases[] = {

  // ~~ Group 1: `addr` must have an lvalue operand (UnaryAddr). Note this is
  // `addr`, not `&`: bare `&` is the field-reference/map-get sugar and
  // reports its own separate "needs a base expression and a field name".
  {
    "addr-of-literal",
    "(package p)\n(fn f [] i32 (do (val q i32* (addr 5)) 0))\n",
    "requires an addressable value", 2, 30,
  },
  {
    "addr-of-arithmetic",
    "(package p)\n(fn f [a i32 b i32] i32 (do (val q i32* (addr (+ a b))) 0))\n",
    "requires an addressable value", 2, 41,
  },

  // ~~ Group 2: integer literals that do not fit their type. A decimal
  // literal is a magnitude and must be rejected; the hex spelling of the same
  // bit pattern is a bit pattern and is accepted (see the ACCEPT table).
  {
    "u8-literal-too-large",
    "(package p)\n(val x u8 300u8)\n",
    "literal 300 doesn't fit in u8", 2, 11,
  },
  {
    "i8-literal-too-negative",
    "(package p)\n(val x i8 -200i8)\n",
    "literal -200 doesn't fit in i8", 2, 11,
  },
  {
    "negative-literal-in-unsigned",
    "(package p)\n(val x u64 -5u64)\n",
    "literal -5 doesn't fit in u64", 2, 12,
  },
  {
    // Caught in lower.c (atom_parse_i64's ERANGE check), not checker.c --
    // before that landed, strtoll clamped this to i64 max silently.
    "enum-variant-value-out-of-range",
    "(package p)\n(enum E [A 99999999999999999999999])\n",
    "doesn't fit in 64 bits", 2, 12,
  },

  // ~~ Group 3: `cast` legality -- only numeric<->numeric, pointer<->pointer,
  // pointer<->any and enum<->numeric are real conversions.
  {
    "cast-struct-to-int",
    "(package p)\n(struct S [a i32])\n(fn f [s S] i32 (cast i32 s))\n",
    "cannot `cast` S to i32", 3, 17,
  },
  {
    "cast-string-to-int",
    "(package p)\n(fn f [s string] i32 (cast i32 s))\n",
    "cannot `cast` string to i32", 2, 22,
  },

  // ~~ Group 4: duplicate declarations -- previously last-write-wins, silent.
  {
    "duplicate-enum-variant",
    "(package p)\n(enum E [A B A])\n",
    "enum `E` declares variant `A` more than once", 2, 1,
  },
  {
    "duplicate-toplevel-fn",
    "(package p)\n(fn dup [] i32 1)\n(fn dup [] i32 2)\n",
    "`fn dup` is declared more than once in this package", 3, 1,
  },
  {
    "duplicate-toplevel-struct",
    "(package p)\n(struct S [a i32])\n(struct S [b i32])\n",
    "`struct S` is declared more than once in this package", 3, 1,
  },

  // ~~ Group 5: all-paths-return. A no-else `if` whose then-branch diverges
  // used to propagate Unresolved, which the body/return-type mismatch check
  // skips -- so this whole function type-checked clean.
  {
    "no-else-if-falls-off-i32-function",
    "(package p)\n(fn f [c bool] i32 (if c (return 1)))\n",
    "`fn f` declared to return i32 but body evaluates to void", 2, 1,
  },

  // ~~ Group 6: the everyday rules. Not recent work, but the ones a
  // refactor is most likely to break, and none had a test either.
  {
    "undefined-identifier",
    "(package p)\n(fn f [] i32 nope)\n",
    "undefined identifier `nope`", 2, 14,
  },
  {
    "call-undefined-function",
    "(package p)\n(fn f [] i32 (nope 1))\n",
    "call to undefined function `nope`", 2, 14,
  },
  {
    "unknown-type-annotation",
    "(package p)\n(fn f [x Nope] i32 0)\n",
    "unknown type `Nope`", 2, 10,
  },
  {
    "val-initializer-type-mismatch",
    "(package p)\n(fn f [] i32 (do (val x i32 \"str\") 0))\n",
    "`val x` declared as i32 but initializer is string", 2, 18,
  },
  {
    "set-on-val-is-immutable",
    "(package p)\n(fn f [] i32 (do (val x i32 1) (set x 2) 0))\n",
    "immutable", 2, 32,
  },
  {
    "return-type-mismatch",
    "(package p)\n(fn f [] i32 \"nope\")\n",
    "`fn f` declared to return i32 but body evaluates to string", 2, 1,
  },
  {
    "if-branches-mismatched",
    "(package p)\n(fn f [c bool] i32 (if c 1 \"two\"))\n",
    "`if` branches have mismatched types: i32 vs string", 2, 20,
  },
  {
    "field-on-non-struct",
    "(package p)\n(fn f [x i32] i32 (. x field))\n",
    "`.` requires a struct or Map here, got i32", 2, 19,
  },
  {
    "unknown-field",
    "(package p)\n(struct S [a i32])\n(fn f [s S] i32 (. s b))\n",
    "`S` has no field `b`", 3, 17,
  },
  {
    "struct-literal-wrong-field-type",
    "(package p)\n(struct S [a i32])\n(fn f [] S (S {:a \"str\"}))\n",
    "field `a` of `S`: expected i32, got string", 3, 12,
  },
  {
    "deref-non-pointer",
    "(package p)\n(fn f [x i32] i32 (deref x))\n",
    "cannot `deref` non-pointer type i32", 2, 19,
  },
  {
    "wrong-argument-count",
    "(package p)\n(fn g [a i32] i32 a)\n(fn f [] i32 (g 1 2))\n",
    "`g` expects 1 argument(s), got 2", 3, 14,
  },
  {
    "wrong-argument-type",
    "(package p)\n(fn g [a i32] i32 a)\n(fn f [] i32 (g \"s\"))\n",
    "`g` argument 1: expected i32, got string", 3, 14,
  },
  {
    "binary-op-mismatched-operands",
    "(package p)\n(fn f [a i32 b f32] i32 (+ a b))\n",
    "`+` operands have mismatched types: i32 vs f32", 2, 25,
  },
  {
    "nth-on-non-indexable",
    "(package p)\n(fn f [x i32] i32 (nth x 0))\n",
    "`nth` requires an array, pointer, or Vector, got i32", 2, 19,
  },
  {
    "for-over-non-iterable",
    "(package p)\n(fn f [x i32] void (for [e x] (void e)))\n",
    "`for` can't iterate i32", 2, 20,
  },
  {
    "unary-minus-on-string",
    "(package p)\n(fn f [s string] i32 (- s))\n",
    "`-` (unary) requires a numeric operand, got string", 2, 22,
  },
  {
    "print-placeholder-count-mismatch",
    "(package p)\n(fn f [] void (println \"{} {}\" 1))\n",
    "`println` template has 2 `{}` placeholder(s) but got 1 value(s)", 2, 15,
  },
  {
    "lane-index-outside-parallel",
    "(package p)\n(fn f [] u32 (lane-index))\n",
    "`lane-index` used outside of a `parallel` block", 2, 14,
  },
  {
    "break-outside-any-loop",
    "(package p)\n(fn f [] i32 (do (break) 0))\n",
    "`break` used outside of a loop", 2, 18,
  },
  {
    "continue-outside-any-loop",
    "(package p)\n(fn f [] i32 (do (continue) 0))\n",
    "`continue` used outside of a loop", 2, 18,
  },
  {
    // The `parallel` barrier: its body becomes a separate C function, so the
    // enclosing `for` is not a loop this `break` can reach. Without the
    // save-and-zero of Checker.loop_depth this type-checked and then failed
    // in gcc with "break statement not within loop or switch".
    "break-in-parallel-body-inside-a-loop",
    "(package p)\n(fn f [] void (for [i 0 4] (parallel [n 2] (break))))\n",
    "`break` used outside of a loop", 2, 44,
  },
  {
    // A jump in a loop's HEADER, where the two backends could not agree on
    // which loop it belonged to: natively the header sits outside the loop's
    // C body, so the `break` bound to the enclosing `for`, while bcgen opens
    // its loop context before compiling the header and bound it to the
    // `while`. Rejecting the shape settles it -- see check_loop_header.
    "break-in-while-condition",
    "(package p)\n(fn f [] void (for [i 0 4] (while (do (break) true) (void 0))))\n",
    "`break` cannot appear in a loop's header", 2, 39,
  },
  {
    // The `continue` spelling of the same thing, which on the VM jumped back
    // to the very condition it sat in and spun forever.
    "continue-in-while-condition",
    "(package p)\n(fn f [] void (for [i 0 4] (while (do (continue) true) (void 0))))\n",
    "`continue` cannot appear in a loop's header", 2, 39,
  },
  {
    "break-in-for-range-bound",
    "(package p)\n(fn f [] void (for [i 0 4] (for [j 0 (do (break) 2)] (void 0))))\n",
    "`break` cannot appear in a loop's header", 2, 42,
  },
  {
    // Reported as a header error rather than "outside of a loop": there IS an
    // enclosing loop here, it is just the wrong part of one.
    "break-in-outermost-loop-header",
    "(package p)\n(fn f [] void (while (do (break) true) (void 0)))\n",
    "`break` cannot appear in a loop's header", 2, 26,
  },
  {
    "break-takes-no-arguments",
    "(package p)\n(fn f [] void (for [i 0 4] (break 1)))\n",
    "`break` takes no arguments", 2, 28,
  },
  {
    "handle-deref-on-non-handle",
    "(package p)\n(fn f [x i32] i32 (do (handle-deref x) 0))\n",
    "`handle-deref` expects a handle, got i32", 2, 23,
  },
  {
    "swap-non-lvalue",
    "(package p)\n(fn f [a i32] i32 (do (swap a (+ a 1)) 0))\n",
    "`swap` argument 2 must be a mutable variable", 2, 23,
  },
  {
    "main-must-return-i32",
    "(package p)\n(fn main [] f32 1.0)\n",
    "`fn main` must return i32, got f32", 2, 1,
  },

  ////////////////////////////////
  // Ported from test/demo.c, which ran these same sources but only PRINTED
  // the result -- `3b test` returned 0 whatever they did, so a rule that
  // stopped firing flipped its line from FAILED to succeeded in 9,000 lines
  // of output and failed nothing. Every case below is one no other suite
  // pins. The ~30 demo.c cases that duplicated the groups above were dropped
  // rather than ported; the positive snippets it also carried are covered by
  // examples/ through backend-diff.

  // ~~ Arena verbs. Every one of these takes an arena or an arena-shaped operand,
  // and each had only demo.c's printed output behind it.
  {
    "destroy-on-non-arena",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (destroy 5))\n",
    "`destroy` expects an arena, got i32", 3, 3,
  },
  {
    "create-with-non-numeric-reserve",
    "(package p)\n"
    "(fn bad [] arena\n"
    "  (create \"not a size\"))\n",
    "`create`'s reserve size must be numeric, got string", 3, 3,
  },
  {
    "mark-on-non-arena",
    "(package p)\n"
    "(fn bad [] ArenaMark\n"
    "  (mark 5))\n",
    "`mark` expects an arena, got i32", 3, 3,
  },
  {
    "pop-with-non-arenamark",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (let [a arena (create)]\n"
    "    (pop a 5)))\n",
    "`pop` argument 2 must be an ArenaMark, got i32", 4, 5,
  },
  {
    "push-with-non-numeric-count",
    "(package p)\n"
    "(fn bad [arena arena] i32*\n"
    "  (push arena i32 \"not a count\"))\n",
    "`push` count must be numeric, got string", 3, 3,
  },
  {
    "push0-with-a-value-to-copy",
    "(package p)\n"
    "(struct Asset [id i32 size i32])\n"
    "(fn bad [arena arena some-asset Asset] Asset*\n"
    "  (push0 arena some-asset))\n",
    "`push0/push-zero` doesn't make sense with a value to copy", 4, 3,
  },
  {
    "commit-to-non-arena",
    "(package p)\n"
    "(fn bad [] i32*\n"
    "  (scratch [t]\n"
    "    (var nums i32* nil)\n"
    "    (commit 5 nums)))\n",
    "`commit`'s first argument must be an arena, got i32", 5, 5,
  },
  {
    "commit-of-non-pointer",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  (scratch [t]\n"
    "    (commit t 5)))\n",
    "`commit` expects a dyn-push-grown array (a pointer), got i32", 4, 5,
  },

  // ~~ `dyn-push`: the target must be a mutable pointer/Vector local and the value
  // must match its element type.
  {
    "dyn-push-on-non-pointer",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (scratch [t]\n"
    "    (var x i32 0)\n"
    "    (dyn-push t x 1)))\n",
    "`x` must be a pointer or Vector type (e.g. `i32*` or `(Vector i32)`), got i32", 5, 5,
  },
  {
    "dyn-push-on-a-val",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (scratch [t]\n"
    "    (val nums i32* nil)\n"
    "    (dyn-push t nums 1)))\n",
    "`nums` must be mutable", 5, 5,
  },
  {
    "dyn-push-wrong-element-type",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (scratch [t]\n"
    "    (var nums i32* nil)\n"
    "    (dyn-push t nums \"nope\")))\n",
    "`nums` holds i32, value is string", 5, 5,
  },

  // ~~ `scratch` takes exactly one arena name and a body.
  {
    "scratch-with-two-names",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  (scratch [t u]\n"
    "    1))\n",
    "`scratch`'s `[]` must name exactly one arena, e.g. `[t]`", 3, 12,
  },
  {
    "scratch-with-no-body",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  (scratch [t]))\n",
    "`scratch` requires a `[name]` vector and at least one body expression", 3, 3,
  },

  // ~~ Bit operators over numeric/enum/flags operands only, and never across two
  // different enum types.
  {
    "bit-or-on-non-numeric",
    "(package p)\n"
    "(fn bad [] string\n"
    "  (bit-or \"a\" \"b\"))\n",
    "`bit-or` requires numeric or enum/flags operands, got string", 3, 3,
  },
  {
    "bit-or-across-two-enums",
    "(package p)\n"
    "(enum LightType [Directional Point Spot])\n"
    "(flags WindowFlags [Resizable Hidden])\n"
    "(fn bad [] LightType\n"
    "  (bit-or LightType/Spot WindowFlags/Resizable))\n",
    "`bit-or` operands have mismatched types: LightType vs WindowFlags", 5, 3,
  },
  {
    "bit-not-on-non-numeric",
    "(package p)\n"
    "(fn bad [] string\n"
    "  (bit-not \"a\"))\n",
    "`bit-not` requires a numeric or enum/flags operand, got string", 3, 3,
  },
  {
    "bit-shl-on-non-numeric",
    "(package p)\n"
    "(fn bad [] string\n"
    "  (bit-shl \"a\" 1))\n",
    "`bit-shl` requires a numeric or enum/flags left operand, got string", 3, 3,
  },
  {
    "bit-shl-with-non-numeric-shift",
    "(package p)\n"
    "(fn bad [a u32] u32\n"
    "  (bit-shl a \"1\"))\n",
    "`bit-shl` requires a numeric shift amount, got string", 3, 3,
  },

  // ~~ Struct construction and destructuring are strict on arity in both
  // directions -- no silent zero-init, no silent drop.
  {
    "struct-literal-too-many-fields",
    "(package p)\n"
    "(struct Creature [name string health i32])\n"
    "(fn bad [] Creature\n"
    "  (Creature {:name \"Orc\" :health 50 :mana 10}))\n",
    "`Creature` literal has 3 field(s), struct declares 2", 4, 3,
  },
  {
    "positional-construction-too-few",
    "(package p)\n"
    "(struct Vector2 [x f32 y f32])\n"
    "(fn bad [] Vector2\n"
    "  (Vector2 1.0))\n",
    "`Vector2` positional construction supplies 1 value(s), struct declares 2", 4, 3,
  },
  {
    "positional-construction-too-many",
    "(package p)\n"
    "(struct Vector2 [x f32 y f32])\n"
    "(fn bad [] Vector2\n"
    "  (Vector2 1.0 2.0 3.0))\n",
    "`Vector2` positional construction supplies 3 value(s), struct declares 2", 4, 3,
  },
  {
    "positional-destructure-out-of-range",
    "(package p)\n"
    "(struct Vector2 [x f32 y f32])\n"
    "(fn bad [v Vector2] f32\n"
    "  (let [[a b c] v]\n"
    "    (+ a (+ b c))))\n",
    "positional destructure slot 3 is out of range", 4, 9,
  },

  // ~~ Array literals: count, element type, and the need for an expected type.
  {
    "array-literal-wrong-count",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (let [nums [i32 4] [1 2 3]]\n"
    "    (print \"{}\" (nth nums 0))))\n",
    "array literal has 3 element(s), expected 4", 3, 22,
  },
  {
    "array-literal-wrong-element-type",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (let [nums [i32 2] [1 \"two\"]]\n"
    "    (print \"{}\" (nth nums 0))))\n",
    "array literal element 2: expected i32, got string", 3, 22,
  },
  {
    "array-literal-with-no-expected-type",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (print \"{}\" [1 2 3]))\n",
    "array literal needs a known element type from context (e.g. a", 3, 15,
  },

  // ~~ `return`: outside a function, wrong type, and the multi-value forms.
  {
    "return-outside-a-function",
    "(package p)\n"
    "(val bad i32 (return 5))\n",
    "`return` used outside of a function body", 2, 14,
  },
  {
    "return-type-mismatch-in-body",
    "(package p)\n"
    "(fn bad [v i32] bool\n"
    "  (if (< v 0)\n"
    "    (return 5)\n"
    "    (return true)))\n",
    "`return` value has type i32, but the enclosing `fn` returns bool", 4, 5,
  },
  {
    "multi-return-from-single-value-fn",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  (return 1 2))\n",
    "`return` with 2 values requires the enclosing `fn`'s declared return type to", 3, 3,
  },
  {
    "multi-return-wrong-value-count",
    "(package p)\n"
    "(fn bad [] (bool i32)\n"
    "  (return true 1 2))\n",
    "`AnonReturn0` literal has 3 field(s), struct declares 2", 3, 3,
  },

  // ~~ Cast legality, the other half of the pair already in Group 3 above.
  {
    "cast-struct-to-numeric",
    "(package p)\n"
    "(struct Vector2 [x f32 y f32])\n"
    "(fn bad [v Vector2] i32\n"
    "  (cast i32 v))\n",
    "cannot `cast` Vector2 to i32", 4, 3,
  },
  {
    "cast-numeric-to-struct",
    "(package p)\n"
    "(struct Vector2 [x f32 y f32])\n"
    "(fn bad [n i32] Vector2\n"
    "  (cast Vector2 n))\n",
    "cannot `cast` i32 to Vector2", 4, 3,
  },
  {
    "cast-string-to-pointer",
    "(package p)\n"
    "(fn bad [s string] i32*\n"
    "  (cast i32* s))\n",
    "cannot `cast` string to i32*", 3, 3,
  },
  {
    "cast-arena-to-int",
    "(package p)\n"
    "(fn bad [] i64\n"
    "  (scratch [t]\n"
    "    (cast i64 t)))\n",
    "cannot `cast` arena to i64", 4, 5,
  },

  // ~~ The parse/number/checked-math builtins validate their arguments.
  {
    "string-to-i32-on-non-string",
    "(package p)\n"
    "(fn bad [n i32] bool\n"
    "  (let [[valid v] (string-to-i32 n)]\n"
    "    valid))\n",
    "`string-to-i32` requires a string argument, got i32", 3, 19,
  },
  {
    "string-to-i32-with-two-args",
    "(package p)\n"
    "(fn bad [] bool\n"
    "  (let [[valid v] (string-to-i32 \"1\" \"2\")]\n"
    "    valid))\n",
    "`string-to-i32` takes exactly one argument, a string to parse", 3, 19,
  },
  {
    "sqrt-checked-on-non-float",
    "(package p)\n"
    "(fn bad [x i32] bool\n"
    "  (let [[valid v] (sqrt-checked x)]\n"
    "    valid))\n",
    "`sqrt-checked` requires an f32 or f64 argument, got i32", 3, 19,
  },
  {
    "nth-checked-on-plain-pointer",
    "(package p)\n"
    "(fn bad [p i32* i i32] bool\n"
    "  (let [r (nth-checked p i)]\n"
    "    (if r true false)))\n",
    "`nth-checked` requires an array or Vector (needs a known length to check", 3, 11,
  },

  // ~~ Enums and flags.
  {
    "unknown-enum-variant",
    "(package p)\n"
    "(enum LightType [Directional Point Spot])\n"
    "(fn bad [] LightType\n"
    "  LightType/Ambient)\n",
    "`LightType` has no variant `Ambient`", 4, 3,
  },
  {
    "duplicate-flags-variant",
    "(package p)\n"
    "(flags Permissions [Read 1 Write 2 Read 4])\n",
    "flags `Permissions` declares variant `Read` more than once", 2, 1,
  },

  // ~~ Return-type mismatches the declared type can't absorb.
  {
    "pointer-depth-mismatch",
    "(package p)\n"
    "(fn bad [p i32*] i32\n"
    "  (deref (addr p)))\n",
    "`fn bad` declared to return i32 but body evaluates to i32*", 2, 1,
  },
  {
    "nil-returned-for-non-pointer",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  nil)\n",
    "`fn bad` declared to return i32 but body evaluates to nil", 2, 1,
  },

  // ~~ `val`/`var` declaration shape, and the old pre-`[]` syntax that must stay
  // rejected now that the grammar moved on.
  {
    "val-missing-type-and-value",
    "(package p)\n"
    "(+ 1)\n"
    "(val bad-val)\n"
    "(let [] a)\n",
    "`val` requires a name, an explicit type, and (unless the type is an array) a", 3, 1,
  },
  {
    "non-array-val-with-no-value",
    "(package p)\n"
    "(val limit i32)\n",
    "`val` requires a value unless the type is an array, Vector, Map, or Set (all", 2, 1,
  },
  {
    "val-does-not-infer-its-type",
    "(package p)\n"
    "(struct Vector2 [x f32 y f32])\n"
    "(val v (Vector2 1.0 2.0))\n",
    "expected a type: an atom, `[ElementType Count]` for an array, `(Vector", 3, 8,
  },
  {
    "var-as-an-operand",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  (+ 1 (var x i32 5)))\n",
    "`var` can only appear as a statement inside a block (a function body, `do`,", 3, 8,
  },
  {
    "old-fn-shape-no-return-type",
    "(package p)\n"
    "(fn add\n"
    "    ((a i32)\n"
    "     (b i32))\n"
    "    (+ a b))\n",
    "`fn` requires a name, a `[]` parameter vector, an explicit return type, and", 2, 1,
  },
  {
    "old-let-shape-no-bindings-vector",
    "(package p)\n"
    "(let count 10)\n",
    "`let` bindings must be a vector, e.g. `[a i32 0 b i32 1]`", 2, 6,
  },

  // ~~ Odds and ends, each the only test of its rule.
  {
    "equality-on-struct-with-vector-field",
    "(package p)\n"
    "(struct Bag [items (Vector i32)])\n"
    "(fn same [a Bag b Bag] bool\n"
    "  (= a b))\n",
    "`=` requires a comparable type, got Bag", 4, 3,
  },
  {
    "const-char-pointer-passed-as-mutable",
    "(package p)\n"
    "(fn takes-mutable [s char*] i32 0)\n"
    "(fn caller [s (const char*)] i32\n"
    "  (takes-mutable s))\n",
    "`takes-mutable` argument 1: expected char*, got const char*", 4, 3,
  },
  {
    "field-access-on-non-struct",
    "(package p)\n"
    "(fn bad [x i32] i32\n"
    "  (get x health))\n",
    "cannot access field `health` on non-struct type i32", 3, 3,
  },
  {
    "set-on-undefined-identifier",
    "(package p)\n"
    "(fn bad [] i32\n"
    "  (set nonexistent 5))\n",
    "cannot `set` undefined identifier `nonexistent`", 3, 3,
  },
  {
    "for-range-mismatched-bounds",
    "(package p)\n"
    "(fn bad [] void\n"
    "  (for [i 0 10.0]\n"
    "    (print \"{}\" i)))\n",
    "`for` range bounds have mismatched types: i32 vs f32", 3, 3,
  },
  {
    "bare-map-literal-is-not-lowerable",
    "(package p)\n"
    "{:alive #t :health 100 :mana 20}\n",
    "unexpected node kind in expression position", 2, 1,
  },

  // ~~ Parser error paths. The parser reports through the same diag channel, so a
  // fixture that never reaches the checker still belongs here.
  {
    "unterminated-list",
    "(package p)\n"
    "(fn broken [a i32] i32\n",
    "unterminated list (expected `)`)", 2, 1,
  },
  {
    "mismatched-closing-delimiter",
    "(package p)\n"
    "(fn broken [a i32) i32 a)\n",
    "mismatched closing delimiter `)`", 2, 18,
  },
};

////////////////////////////////
//~ The accept table -- sources that must NOT be rejected. These exist to pin
// the exact boundary of the rules above, each one a case a slightly-too-eager
// version of the same check would break.

typedef struct AcceptCase {
  const char* name;
  const char* source;
  const char* why;
} AcceptCase;

static const AcceptCase g_accept_cases[] = {
  {
    "break-in-a-loop-nested-inside-a-loop-header",
    "(package p)\n(fn f [] void (for [i 0 (do (while true (break)) 3)] (void 0)))\n(fn main [] i32 0)\n",
    "the header rule bans jumps that belong to the loop being declared, not "
    "ones inside a loop that merely sits in a header -- this `break` is in a "
    "real loop BODY. check_loop_body clearing in_loop_header is what allows it",
  },
  {
    "scratch-in-a-loop-header",
    "(package p)\n(fn f [] void (while (< 0 (scratch [t] 1)) (void 0)))\n(fn main [] i32 0)\n",
    "only JUMPS are banned in a header; a `scratch` there opens and closes "
    "within the header and needs no unwinding",
  },
  {
    "hex-u64-sentinel",
    "(package p)\n(val x u64 0xFFFFFFFFFFFFFFFFu64)\n(fn main [] i32 0)\n",
    "a hex literal is a bit pattern, not a magnitude -- this is a real "
    "GL_TIMEOUT_IGNORED-style sentinel and must be accepted",
  },
  {
    "decimal-u64-top-of-range",
    "(package p)\n(val x u64 18446744073709551615u64)\n(fn main [] i32 0)\n",
    "the decimal spelling of the same value reaches the checker as -1 in a "
    "signed field; rejecting it was a real bug",
  },
  {
    "u8-literal-at-boundary",
    "(package p)\n(val x u8 255u8)\n(fn main [] i32 0)\n",
    "255 is the largest u8 -- an off-by-one in the range check shows up here",
  },
  {
    "i8-literal-at-boundary",
    "(package p)\n(val x i8 -128i8)\n(fn main [] i32 0)\n",
    "-128 is the most negative i8",
  },
  {
    "cast-to-void-is-always-allowed",
    "(package p)\n(struct S [a i32])\n(fn f [s S] void (void s))\n(fn main [] i32 0)\n",
    "`(void expr)` compiles to exactly a cast-to-void and is used throughout "
    "examples/ to force an arm void",
  },
  {
    "same-type-cast-is-a-no-op",
    "(package p)\n(struct S [a i32])\n(fn f [s S] S (cast S s))\n(fn main [] i32 0)\n",
    "a same-type cast is always allowed even for types no real conversion accepts",
  },
  {
    "same-variant-name-in-two-enums",
    "(package p)\n(enum A [Red Green])\n(enum B [Red Blue])\n(fn main [] i32 0)\n",
    "duplicate-variant detection is per-declaration; reuse across two enums is fine",
  },
  {
    "if-with-both-branches-returning",
    "(package p)\n(fn f [c bool] i32 (if c (return 1) (return 2)))\n(fn main [] i32 0)\n",
    "both branches diverge, so nothing falls off the end",
  },
  {
    "break-and-continue-inside-every-loop-form",
    "(package p)\n"
    "(fn f [v [i32] m {i32 i32}] void\n"
    "  (while true (break))\n"
    "  (for [i 0 4] (continue))\n"
    "  (for [x v] (break))\n"
    "  (for [[k y] m] (continue))\n"
    "  (parallel [n 4] (parallel-for [j n] (break))))\n"
    "(fn main [] i32 0)\n",
    "loop_depth must be raised by EVERY loop form, not just `while` -- a form "
    "that forgets to go through check_loop_body rejects a legal `break`",
  },
  {
    "loop-depth-unwinds-after-a-loop-ends",
    "(package p)\n(fn f [] void (do (for [i 0 4] (break)) (for [j 0 4] (continue))))\n(fn main [] i32 0)\n",
    "check_loop_body's decrement must run: a leaked depth would make the "
    "SECOND loop's jumps legal for the wrong reason, and one after the last "
    "loop legal outright",
  },
  {
    "break-inside-a-loop-nested-in-a-parallel-body",
    "(package p)\n(fn f [] void (parallel [n 4] (for [i 0 4] (break))))\n(fn main [] i32 0)\n",
    "zeroing loop_depth at a `parallel` must not block a loop written INSIDE "
    "the body -- that loop lives in the trampoline with the jump",
  },
  {
    "shadowing-is-not-a-duplicate",
    "(package p)\n(fn helper [] i32 1)\n(fn f [helper i32] i32 helper)\n(fn main [] i32 0)\n",
    "a parameter sharing a name with a top-level fn is legal shadowing",
  },
};

static void
check_accept(const AcceptCase* c) {
  g_checks += 1;
  CheckOutcome o = run_source(c->source, "pos.3b");
  if (!o.errored) return;

  fprintf(stderr, "FAIL %s: expected NO diagnostic, got %llu.\n    (%s)\n",
          c->name, (unsigned long long)o.count, c->why);
  for (u64 i = 0; i < o.kept; i += 1) {
    fprintf(stderr, "    %u:%u %s\n", o.lines[i], o.cols[i], o.messages[i]);
  }
  g_failures += 1;
}

////////////////////////////////

int
main(void) {
  const char* show = getenv("CHECKER_TEST_SHOW");
  g_show           = (show && show[0] == '1');

  for (u64 i = 0; i < sizeof(g_reject_cases) / sizeof(g_reject_cases[0]); i += 1) {
    check_reject(&g_reject_cases[i]);
  }
  for (u64 i = 0; i < sizeof(g_accept_cases) / sizeof(g_accept_cases[0]); i += 1) {
    check_accept(&g_accept_cases[i]);
  }

  if (g_failures == 0) printf("checker_error_test: all checks passed (%d checks)\n", g_checks);
  else                 printf("checker_error_test: %d check(s) FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
