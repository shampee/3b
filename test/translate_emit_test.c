// translate_emit_test.c -- the two checks that decide whether `3b translate`
// will emit a binding at all: verify_record_layouts (does the generated 3b
// struct match the C record it mirrors?) and emit_function's by-value-struct
// skip. Both exist because the failures they catch are otherwise silent or
// misattributed -- see emit.c's "Layout verification" note and
// type_is_struct_by_value.
//
// Driven off hand-built CUnits rather than real headers. That is not a
// shortcut: what needs pinning down is the ARITHMETIC over an extracted
// record -- offsets, trailing padding, unions, packing, nesting -- and a
// header-driven test would express each case as C source, hand it to
// libclang, and check the same numbers with a build dependency on libclang
// and a much vaguer failure message. Here a case IS its numbers, and
// `c_size`/`c_offset` can be set to what a real compiler measures (the
// comments say which) or deliberately to something else, which is the whole
// point of the check.
//
// Same rig as the other test/*_test.c files. `make check` builds this against
// translate/emit.o + translate/rename.o; neither needs libclang.
#include "3b.h"
#include "translate/translate.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

////////////////////////////////
//~ Fixture builders

static CType
ctype_named(const char* name) {
  CType t = {0};
  t.kind  = CTypeKind_Named;
  t.name  = str8_cstring((char*)name);
  return t;
}

static CType
ctype_ptr_to(CType pointee) {
  CType* boxed = push_array(ctx_perm(), CType, 1);
  *boxed       = pointee;
  CType t      = {0};
  t.kind       = CTypeKind_Pointer;
  t.pointee    = boxed;
  return t;
}

// `Ret (*)(P0, P1, ...)`. Params are copied, so callers can pass a stack array.
static CType
ctype_fn_ptr(CType ret, CType* params, u32 param_count) {
  CType t       = {0};
  t.kind        = CTypeKind_FunctionPointer;
  t.fn_return   = push_array(ctx_perm(), CType, 1);
  *t.fn_return  = ret;
  if (param_count > 0) {
    t.fn_params      = push_array(ctx_perm(), CType, param_count);
    t.fn_param_count = param_count;
    for (u32 i = 0; i < param_count; i += 1) t.fn_params[i] = params[i];
  }
  return t;
}

static CType
ctype_array_of(CType elem, u64 count) {
  CType* boxed  = push_array(ctx_perm(), CType, 1);
  *boxed        = elem;
  CType t       = {0};
  t.kind        = CTypeKind_Array;
  t.pointee     = boxed;
  t.array_count = count;
  return t;
}

// Appends one ordinary field at `offset`, as libclang would have measured it.
static void
add_field(CRecord* r, const char* name, CType type, u64 offset) {
  CField f  = {0};
  f.name    = str8_cstring((char*)name);
  f.type    = type;
  f.c_offset = offset;
  dyn_push(ctx_perm(), r->fields, f);
}

// A bitfield member. `c_offset` is meaningless for one (its storage is a bit
// range), which is exactly why the check rejects any record containing one.
static void
add_bitfield(CRecord* r, const char* name, CType type) {
  CField f     = {0};
  f.name       = str8_cstring((char*)name);
  f.type       = type;
  f.is_bitfield = true;
  dyn_push(ctx_perm(), r->fields, f);
  r->bitfield_count += 1;
}

static CRecord*
add_record(CUnit* unit, const char* name, u64 c_size, u64 c_align) {
  CRecord r    = {0};
  r.name       = str8_cstring((char*)name);
  r.is_complete = true;
  r.c_size     = c_size;
  r.c_align    = c_align;
  dyn_push(ctx_perm(), unit->records, r);
  return &unit->records[dyn_count(unit->records) - 1];
}

static void
expect_layout(const char* what, CUnit* unit, Config* cfg, b32 want_ok) {
  // Diagnostics land on stderr by design -- they are the feature. Silence only
  // the ones an expected failure produces, so the test's own output stays
  // readable; a surprise failure still prints, since it goes through the same
  // fprintf but the assertion below is what reports it.
  b32 got = verify_record_layouts(cfg, unit);
  if (got != want_ok) {
    fprintf(stderr, "FAIL %s: verify_record_layouts returned %s, want %s\n",
            what, got ? "true" : "false", want_ok ? "true" : "false");
    g_failures += 1;
  }
}

static void
expect_names(const char* what, CUnit* unit, Config* cfg, b32 want_ok) {
  b32 got = verify_type_names(cfg, unit);
  if (got != want_ok) {
    fprintf(stderr, "FAIL %s: verify_type_names returned %s, want %s\n",
            what, got ? "true" : "false", want_ok ? "true" : "false");
    g_failures += 1;
  }
}

////////////////////////////////
//~ Emission, for the by-value checks

static void
add_function(CUnit* unit, const char* name, CType ret, CType* param_types, u64 param_count) {
  CFunction fn  = {0};
  fn.name       = str8_cstring((char*)name);
  fn.return_type = ret;
  foreach_index(i, param_count) {
    CParam p = {0};
    p.name   = str8_cstring((char*)"x");
    p.type   = param_types[i];
    dyn_push(ctx_perm(), fn.params, p);
  }
  dyn_push(ctx_perm(), unit->functions, fn);
}

static String8
slurp_and_close(FILE* f) {
  fflush(f);
  long size = ftell(f);
  rewind(f);
  u8* buf = push_array(ctx_perm(), u8, (u64)size + 1);
  u64 got = fread(buf, 1, (u64)size, f);
  fclose(f);
  buf[got] = 0;
  return str8(buf, got);
}

// Runs emit_package into temp files and returns the whole generated text, so a
// case can assert on what was and wasn't emitted. `out_byval`, when given, also
// receives the generated C bridge header -- the two halves of a bridged
// function land in different files, and both matter.
static String8
emit_to_string_with_byval(Config* cfg, CUnit* unit, String8* out_byval) {
  FILE* f  = tmpfile();
  FILE* bv = tmpfile();
  if (!f || !bv) { fprintf(stderr, "FATAL: tmpfile() failed\n"); exit(1); }
  emit_package(cfg, unit, f, bv, NULL);
  if (out_byval) *out_byval = slurp_and_close(bv);
  else            fclose(bv);
  return slurp_and_close(f);
}

static String8
emit_to_string(Config* cfg, CUnit* unit) {
  return emit_to_string_with_byval(cfg, unit, NULL);
}

static void
expect_emitted(const char* what, String8 text, const char* needle, b32 want_present) {
  b32 present = str8_find_needle(text, 0, str8_cstring((char*)needle), 0) < text.size;
  if (present != want_present) {
    fprintf(stderr, "FAIL %s: \"%s\" was %s in the generated package, want %s\n",
            what, needle, present ? "present" : "absent", want_present ? "present" : "absent");
    g_failures += 1;
  }
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(64));

  Config cfg = {0};
  cfg.package_name = str8_lit("t");

  ////////////////////////////////
  //~ Layout verification: the cases that must PASS
  //
  // Each c_size/c_align/c_offset below is what gcc actually measures for the
  // C in the comment, on the System V LP64 ABI these bindings target.

  { // struct { int a; int b; } -- the ordinary case, nothing interesting
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Pair", 8, 4);
    add_field(r, "a", ctype_named("int"), 0);
    add_field(r, "b", ctype_named("int"), 4);
    expect_layout("plain struct", &unit, &cfg, true);
  }

  { // struct { char c; int n; } -- interior padding after `c`
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Padded", 8, 4);
    add_field(r, "c", ctype_named("signed char"), 0);
    add_field(r, "n", ctype_named("int"), 4);
    expect_layout("interior padding", &unit, &cfg, true);
  }

  { // struct { int n; char c; } -- TRAILING padding, which only the size
    // comparison catches: every field offset already agrees.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Tail", 8, 4);
    add_field(r, "n", ctype_named("int"), 0);
    add_field(r, "c", ctype_named("signed char"), 4);
    expect_layout("trailing padding", &unit, &cfg, true);
  }

  { // union { int i; double d; } -- every member at 0, size from the widest
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "U", 8, 8);
    r->is_union = true;
    add_field(r, "i", ctype_named("int"), 0);
    add_field(r, "d", ctype_named("double"), 0);
    expect_layout("union", &unit, &cfg, true);
  }

  { // A pointer field is 8/8 whatever it points at, including at a record
    // this unit never captured -- the reason mirror_layout_of_ctype maps every
    // pointer shape to `any` before looking anything up.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Holder", 16, 8);
    add_field(r, "p", ctype_ptr_to(ctype_named("struct Elsewhere")), 0);
    add_field(r, "n", ctype_named("long"), 8);
    expect_layout("pointer field", &unit, &cfg, true);
  }

  { // struct Outer { struct Inner { int a; double b; } in; int n; } -- nesting,
    // where the inner record's own alignment propagates outward
    CUnit unit = {0};
    CRecord* inner = add_record(&unit, "Inner", 16, 8);
    add_field(inner, "a", ctype_named("int"), 0);
    add_field(inner, "b", ctype_named("double"), 8);
    CRecord* outer = add_record(&unit, "Outer", 24, 8);
    add_field(outer, "in", ctype_named("Inner"), 0);
    add_field(outer, "n", ctype_named("int"), 16);
    expect_layout("nested record", &unit, &cfg, true);
  }

  { // struct { int a[3]; char c; } -- array stride, then trailing padding
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Arr", 16, 4);
    add_field(r, "a", ctype_array_of(ctype_named("int"), 3), 0);
    add_field(r, "c", ctype_named("signed char"), 12);
    expect_layout("array field", &unit, &cfg, true);
  }

  { // __attribute__((packed)) struct { char c; int n; } -- no padding at all
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Packed", 5, 1);
    r->is_packed = true;
    add_field(r, "c", ctype_named("signed char"), 0);
    add_field(r, "n", ctype_named("int"), 1);
    expect_layout("packed struct", &unit, &cfg, true);
  }

  { // An enum member is int-sized on both sides.
    CUnit unit = {0};
    CEnum e = {0};
    e.name  = str8_lit("Mode");
    dyn_push(ctx_perm(), unit.enums, e);
    CRecord* r = add_record(&unit, "HasEnum", 8, 4);
    add_field(r, "m", ctype_named("Mode"), 0);
    add_field(r, "n", ctype_named("int"), 4);
    expect_layout("enum field", &unit, &cfg, true);
  }

  { // A typedef is followed to what it names.
    CUnit unit = {0};
    CTypedef td = {0};
    td.name     = str8_lit("i32_t");
    td.underlying = ctype_named("int");
    dyn_push(ctx_perm(), unit.typedefs, td);
    CRecord* r = add_record(&unit, "ViaTypedef", 8, 4);
    add_field(r, "a", ctype_named("i32_t"), 0);
    add_field(r, "b", ctype_named("i32_t"), 4);
    expect_layout("typedef field", &unit, &cfg, true);
  }

  ////////////////////////////////
  //~ Layout verification: the cases that must FAIL
  //
  // The messages these print to stderr are expected output, not noise -- each
  // is what a user would see instead of a corrupted read.

  { // struct { long long pos; int flags:2; int size:30; int min_distance; }
    // -- ffmpeg's AVIndexEntry: 16 real bytes of bitfield storage become 8,
    // so the mirror runs long.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "IndexEntry", 16, 8);
    add_field(r, "pos", ctype_named("long long"), 0);
    add_bitfield(r, "flags", ctype_named("int"));
    add_bitfield(r, "size", ctype_named("int"));
    add_field(r, "min_distance", ctype_named("int"), 12);
    expect_layout("bitfields", &unit, &cfg, false);
  }

  { // A bitfield that happens to leave the SIZE right. Only the per-field
    // offset comparison rejects this one, which is why size alone isn't
    // enough: `struct { int a:16; int b:16; }` is 4 bytes, and so is a mirror
    // of one 4-byte field -- but the mirror has dropped a member.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "TwoHalves", 4, 4);
    add_bitfield(r, "a", ctype_named("int"));
    expect_layout("size-matching bitfield", &unit, &cfg, false);
  }

  { // struct { int n; union { int i; float f; }; } -- a C11 anonymous member,
    // which has no name to put in a 3b field vector and is dropped outright.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "WithAnon", 8, 4);
    add_field(r, "n", ctype_named("int"), 0);
    r->anon_member_count = 1;
    expect_layout("anonymous member", &unit, &cfg, false);
  }

  { // The `bool` ABI bug, reconstructed: C's _Bool is 1 byte, and while 3b
    // mapped it to a 4-byte b32 every field after it sat at the wrong offset.
    // This is the case that justifies the check being a whitelist -- no rule
    // here knows anything about bools, the numbers simply disagree.
    //
    // TWO bools, not one: `struct { _Bool a; int n; }` is 8 bytes either way,
    // the widened field simply eating padding that was already there. It takes
    // a second one for the divergence to reach an offset, which is why this
    // shape gets a case of its own.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Flags", 8, 4); // struct { _Bool on, off; int n; }
    add_field(r, "on", ctype_named("_Bool"), 0);
    add_field(r, "off", ctype_named("_Bool"), 1);
    add_field(r, "n", ctype_named("int"), 4);
    // Pretend `_Bool` maps to something 4 bytes wide, as it once did.
    cfg.type_map = NULL;
    TypeMapEntry tm = { str8_lit("_Bool"), str8_lit("i32") };
    dyn_push(ctx_perm(), cfg.type_map, tm);
    expect_layout("width mismatch", &unit, &cfg, false);
    cfg.type_map = NULL;
  }

  ////////////////////////////////
  //~ Layout verification: the cases with nothing to check

  { // force-opaque never becomes a struct at all, so its layout is irrelevant
    // -- and this is the escape hatch every failure message points at, so it
    // has to actually work on a record that would otherwise fail.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "IndexEntry", 16, 8);
    add_field(r, "pos", ctype_named("long long"), 0);
    add_bitfield(r, "flags", ctype_named("int"));
    Config oc = {0};
    oc.package_name = str8_lit("t");
    dyn_push(ctx_perm(), oc.force_opaque, str8_lit("IndexEntry"));
    expect_layout("force-opaque escape hatch", &unit, &oc, true);
  }

  { // Forward-declared only: no fields to compare, and emit.c writes an alias
    // rather than a struct.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Opaque", 0, 0);
    r->is_complete = false;
    expect_layout("incomplete record", &unit, &cfg, true);
  }

  { // libclang declined to measure the original, so there is no C side to
    // compare against -- deliberately NOT treated as a failure.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Unmeasured", 0, 0);
    r->layout_unknown = true;
    add_field(r, "a", ctype_named("int"), 0);
    expect_layout("unmeasurable record", &unit, &cfg, true);
  }

  { // A field type nothing captured has no computable 3b layout, so the check
    // stands down -- emit_record warns per field and the package won't build.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Unresolvable", 4, 4);
    add_field(r, "a", ctype_named("struct NeverCaptured"), 0);
    expect_layout("unresolvable field type", &unit, &cfg, true);
  }

  { // A struct reachable from itself by value is not legal C, so no real
    // header produces one -- but the walk still has to TERMINATE on it rather
    // than run the stack out, which is what MIRROR_MAX_DEPTH is for. Reaching
    // the next line at all is most of the assertion; the depth cap yields "no
    // computable layout", so the check stands down and passes.
    CUnit unit = {0};
    CRecord* a = add_record(&unit, "A", 8, 8);
    add_field(a, "b", ctype_named("B"), 0);
    CRecord* b = add_record(&unit, "B", 8, 8);
    add_field(b, "a", ctype_named("A"), 0);
    expect_layout("self-referential records terminate", &unit, &cfg, true);
  }

  ////////////////////////////////
  //~ By-value struct params and returns
  //
  // These are bridged, not skipped: the wrapper hands the struct through a
  // `void*` to a generated `static inline` shim that memcpys it into the real
  // C type and makes the by-value call itself. Both halves are checked -- the
  // 3b package and the C bridge header are separate files, and a function is
  // only actually callable if both got written.

  {
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 8, 4);
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);
    CEnum e = {0};
    e.name  = str8_lit("Mode");
    dyn_push(ctx_perm(), unit.enums, e);

    CType by_value  = ctype_named("Rat");
    CType by_ptr    = ctype_ptr_to(ctype_named("Rat"));
    CType an_int    = ctype_named("int");
    CType an_enum   = ctype_named("Mode");

    add_function(&unit, "c_take_val", ctype_named("double"), &by_value, 1);
    add_function(&unit, "c_ret_val", by_value, &an_int, 1);
    add_function(&unit, "c_take_ptr", ctype_named("double"), &by_ptr, 1);
    add_function(&unit, "c_ret_ptr", by_ptr, &an_int, 1);
    add_function(&unit, "c_take_enum", an_enum, &an_enum, 1);
    add_function(&unit, "c_plain_fn", ctype_named("int"), &an_int, 1);

    String8 byval = {0};
    String8 text  = emit_to_string_with_byval(&cfg, &unit, &byval);

    // The wrapper keeps the signature the C function really has -- `Rat` in,
    // `Rat` out -- and only the private extern behind it deals in pointers.
    expect_emitted("by-value param wrapper", text, "(fn c-take-val [x Rat] f64", true);
    expect_emitted("by-value param call", text, "(t_byval_c_take_val (addr x))", true);
    expect_emitted("by-value return wrapper", text, "(fn c-ret-val [x i32] Rat", true);
    expect_emitted("by-value return local", text, "(let [out Rat (Rat {:num 0 :den 0})]", true);
    // The direct extern is gone for a bridged function: calling it is exactly
    // what does not compile.
    expect_emitted("no direct extern", text, "(extern (fn c_take_val", false);

    // The C half: the value crosses as a void*, is memcpy'd into the real type,
    // and the by-value call happens against the real header's declaration.
    expect_emitted("shim for param", byval, "static inline double t_byval_c_take_val(const void *a0_)", true);
    expect_emitted("shim unpacks", byval, "struct Rat v0_; memcpy(&v0_, a0_, sizeof v0_);", true);
    expect_emitted("shim calls real fn", byval, "return c_take_val(v0_);", true);
    expect_emitted("shim for return", byval, "static inline void t_byval_c_ret_val(void *ret_, int a0_)", true);
    expect_emitted("shim copies result out", byval, "memcpy(ret_, &r_, sizeof r_);", true);
    expect_emitted("header pulls in memcpy", byval, "#include <string.h>", true);

    // Unchanged: a pointer to the mirror is what every working binding already
    // passes, and an enum is a plain int on both sides. Neither needs a shim.
    expect_emitted("pointer param", text, "c_take_ptr", true);
    expect_emitted("pointer return", text, "c_ret_ptr", true);
    expect_emitted("enum param and return", text, "c_take_enum", true);
    expect_emitted("unrelated function", text, "c_plain_fn", true);
    expect_emitted("no shim for pointer param", byval, "c_take_ptr", false);
    expect_emitted("no shim for plain fn", byval, "c_plain_fn", false);
  }

  { // The bridge memcpys between the mirror and the original, so it is only
    // sound while those are the same bytes. A record whose measured C size
    // disagrees with its mirror is refused -- the same mismatch
    // verify_record_layouts makes fatal, asked again per record rather than
    // assumed from the package-wide pass.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 12, 4); // 12, but two ints mirror as 8
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);
    CType by_value = ctype_named("Rat");
    add_function(&unit, "c_take_val", ctype_named("double"), &by_value, 1);

    String8 byval = {0};
    String8 text  = emit_to_string_with_byval(&cfg, &unit, &byval);
    expect_emitted("mismatched mirror not bridged (3b)", text, "c_take_val", false);
    expect_emitted("mismatched mirror not bridged (C)", byval, "c_take_val", false);
  }

  { // Same, for a record libclang declined to measure: nothing was compared, so
    // nothing is known, so nothing is copied.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 8, 4);
    r->layout_unknown = true;
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);
    CType by_value = ctype_named("Rat");
    add_function(&unit, "c_take_val", ctype_named("double"), &by_value, 1);
    expect_emitted("unmeasured mirror not bridged", emit_to_string(&cfg, &unit), "c_take_val", false);
  }

  { // A struct RETURN needs a value of that struct for the wrapper's receiving
    // local, and 3b has no "zeroed T" form to conjure one with. An enum field
    // has no zero variant to name, so this one falls back to being skipped
    // rather than emitting 3b that will not check.
    CUnit unit = {0};
    CEnum e = {0};
    e.name  = str8_lit("Mode");
    dyn_push(ctx_perm(), unit.enums, e);
    CRecord* r = add_record(&unit, "Tagged", 8, 4);
    add_field(r, "mode", ctype_named("Mode"), 0);
    add_field(r, "n", ctype_named("int"), 4);
    CType by_value = ctype_named("Tagged");
    CType an_int   = ctype_named("int");
    add_function(&unit, "c_ret_tagged", by_value, &an_int, 1);
    add_function(&unit, "c_take_tagged", an_int, &by_value, 1); // a PARAM needs no such value
    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("unspellable zero, return", text, "c_ret_tagged", false);
    expect_emitted("unspellable zero, param", text, "c_take_tagged", true);
  }

  { // A parameter whose C declarator wraps around its NAME cannot be written
    // into the shim's signature by pasting a type text to the left of one.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 8, 4);
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);
    CType params[2] = { ctype_named("Rat"), ctype_array_of(ctype_named("int"), 4) };
    add_function(&unit, "c_take_val_and_array", ctype_named("int"), params, 2);
    expect_emitted("unspellable C param", emit_to_string(&cfg, &unit), "c_take_val_and_array", false);
  }

  { // force-opaque again: the type is emitted as `any`, so there is no mirror
    // struct at all -- nothing to mismatch, and nothing to bridge.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 8, 4);
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);
    CType by_value = ctype_named("Rat");
    add_function(&unit, "c_take_val", ctype_named("double"), &by_value, 1);

    Config oc = {0};
    oc.package_name = str8_lit("t");
    dyn_push(ctx_perm(), oc.force_opaque, str8_lit("Rat"));
    String8 byval = {0};
    expect_emitted("force-opaque by-value", emit_to_string_with_byval(&oc, &unit, &byval), "c_take_val", true);
    expect_emitted("force-opaque needs no shim", byval, "c_take_val", false);
  }

  { // A typedef to a struct is followed, so aliasing one neither sneaks a
    // by-value struct past the check nor hides it from the bridge. The shim
    // spells the parameter with the TYPEDEF's name, which is the one C
    // accepts bare.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 8, 4);
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);
    CTypedef td   = {0};
    td.name       = str8_lit("RatAlias");
    td.underlying = ctype_named("Rat");
    dyn_push(ctx_perm(), unit.typedefs, td);
    CType by_alias = ctype_named("RatAlias");
    add_function(&unit, "c_take_alias", ctype_named("double"), &by_alias, 1);
    String8 byval = {0};
    expect_emitted("typedef to struct, by value", emit_to_string_with_byval(&cfg, &unit, &byval), "c_take_alias", true);
    expect_emitted("shim uses the typedef name", byval, "RatAlias v0_;", true);
  }

  { // A typedef chain that lands on something that is NOT a struct
    // (`typedef int GLsizei;`) is not by value in any sense, and must be left
    // as a direct call. Reporting the typedef's name while walking the chain
    // makes callers read it as "a by-value struct was found" and bridge every
    // function with a typedef'd parameter, glScissor among them -- with
    // nothing in the shape of the emitted code looking wrong.
    CUnit unit = {0};
    CTypedef td   = {0};
    td.name       = str8_lit("GLsizei");
    td.underlying = ctype_named("int");
    dyn_push(ctx_perm(), unit.typedefs, td);
    CType sized = ctype_named("GLsizei");
    add_function(&unit, "c_scissor", ctype_named("void"), &sized, 1);
    String8 byval = {0};
    String8 text  = emit_to_string_with_byval(&cfg, &unit, &byval);
    expect_emitted("typedef to a non-struct stays direct", text, "(extern (fn c_scissor", true);
    expect_emitted("typedef to a non-struct needs no shim", byval, "c_scissor", false);
  }

  { // A union RETURNED by value has no zero literal either: naming every member
    // at once is not what a union is. As a PARAM it needs no such value, so
    // that half still bridges.
    CUnit unit = {0};
    CRecord* u = add_record(&unit, "Word", 4, 4);
    u->is_union = true;
    add_field(u, "i", ctype_named("int"), 0);
    add_field(u, "f", ctype_named("float"), 0);
    CType by_value = ctype_named("Word");
    CType an_int   = ctype_named("int");
    add_function(&unit, "c_ret_union", by_value, &an_int, 1);
    add_function(&unit, "c_take_union", an_int, &by_value, 1);
    String8 byval = {0};
    String8 text  = emit_to_string_with_byval(&cfg, &unit, &byval);
    expect_emitted("union return not bridged", text, "c_ret_union", false);
    expect_emitted("union param bridged", text, "c_take_union", true);
    expect_emitted("union param shim", byval, "union Word v0_;", true);
  }

  { // An opaque-handle typedef (`typedef struct Ctx_t* Ctx`) is a pointer, and
    // must not be mistaken for a struct passed by value.
    CUnit unit = {0};
    CTypedef td      = {0};
    td.name          = str8_lit("Ctx");
    td.underlying    = ctype_ptr_to(ctype_named("struct Ctx_t"));
    td.is_opaque_handle = true;
    dyn_push(ctx_perm(), unit.typedefs, td);
    CType handle = ctype_named("Ctx");
    add_function(&unit, "c_take_handle", ctype_named("int"), &handle, 1);
    expect_emitted("opaque handle typedef", emit_to_string(&cfg, &unit), "c_take_handle", true);
  }

  { // A force-opaque type whose definition no header ever provided still gets
    // its `(alias Name any)`. This is the `struct SwsContext;` case: libclang
    // hands over a forward declaration, capture_record drops it, and before
    // emit_unreached_opaques existed nothing was left to walk over -- so uses
    // translated fine but the NAME did not exist, and a caller trying to spell
    // the type heard about it from gcc rather than from this tool.
    CUnit unit = {0};
    CType opaque_ptr = ctype_ptr_to(ctype_named("Ctx"));
    add_function(&unit, "c_take_ctx", ctype_named("int"), &opaque_ptr, 1);

    Config oc = {0};
    oc.package_name = str8_lit("t");
    dyn_push(ctx_perm(), oc.force_opaque, str8_lit("Ctx"));
    String8 text = emit_to_string(&oc, &unit);
    expect_emitted("undefined force-opaque gets an alias", text, "(alias Ctx any)", true);
    expect_emitted("undefined force-opaque keeps its function", text, "c_take_ctx", true);
  }

  { // ...and exactly one alias: a force-opaque type the headers DID define is
    // emitted by emit_record, so the backstop must not write a second one.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "Rat", 8, 4);
    add_field(r, "num", ctype_named("int"), 0);
    add_field(r, "den", ctype_named("int"), 4);

    Config oc = {0};
    oc.package_name = str8_lit("t");
    dyn_push(ctx_perm(), oc.force_opaque, str8_lit("Rat"));
    String8 text  = emit_to_string(&oc, &unit);
    u64     count = 0;
    for (u64 i = 0; i + 15 <= text.size; i += 1) {
      if (MemoryMatch(text.str + i, "(alias Rat any)", 15)) count += 1;
    }
    if (count != 1) {
      printf("FAIL defined force-opaque alias emitted once: got %llu\n", (unsigned long long)count);
      g_failures += 1;
    }
  }

  ////////////////////////////////
  //~ verify_type_names: the mangled `<pkg>_<name>` must not be a C name the
  // headers already define. Same shape as the layout cases above -- the
  // interesting thing is a pure name computation, so hand-built records say it
  // more directly than any header would.

  { // The stbtt case that motivated the check. Package `stbtt`, a library whose
    // own types are already `stbtt_`-prefixed, and a config reaching for the
    // obvious `(strip-struct-prefix "stbtt_")`: `stbtt_bakedchar` -> `bakedchar`
    // -> `stbtt_bakedchar`, the real name, in a TU that has already seen it.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "stbtt_bakedchar", 4, 2);
    add_field(r, "x0", ctype_named("unsigned short"), 0);
    add_field(r, "y0", ctype_named("unsigned short"), 2);

    Config cfg = {0};
    cfg.package_name        = str8_lit("stbtt");
    cfg.strip_struct_prefix = str8_lit("stbtt_");
    expect_names("strip-struct-prefix rebuilding the C name collides", &unit, &cfg, false);

    // The fix in stbtt.cfg.3b: a rename to a name that is neither the C one nor
    // a doubling of the package name.
    Config renamed        = {0};
    renamed.package_name  = str8_lit("stbtt");
    RenameRule rr         = {0};
    rr.from               = str8_lit("stbtt_bakedchar");
    rr.to                 = str8_lit("BakedChar");
    dyn_push(ctx_perm(), renamed.type_renames, rr);
    expect_names("rename-type away from the C name is accepted", &unit, &renamed, true);

    // And doing nothing is merely ugly (`stbtt_stbtt_bakedchar`), not a
    // collision -- it must stay a note, not a refusal to emit.
    Config bare       = {0};
    bare.package_name = str8_lit("stbtt");
    expect_names("double-prefixed name is a note, not a failure", &unit, &bare, true);
  }

  { // A forward-declared-only record is never emitted, so its mangled name
    // cannot collide with anything -- reporting it would fail a config that is
    // already correct.
    CUnit unit = {0};
    CRecord r  = {0};
    r.name     = str8_lit("t_Thing"); // would mangle straight back to itself
    dyn_push(ctx_perm(), unit.records, r);

    Config cfg            = {0};
    cfg.package_name      = str8_lit("t");
    cfg.strip_struct_prefix = str8_lit("t_");
    expect_names("forward-declared-only record is not checked", &unit, &cfg, true);
  }

  { // The ordinary case every existing binding is in: the package prefix and the
    // library prefix differ, so nothing can collide. gltf/cgltf, literally.
    CUnit unit = {0};
    CRecord* r = add_record(&unit, "cgltf_node", 8, 4);
    add_field(r, "a", ctype_named("int"), 0);
    add_field(r, "b", ctype_named("int"), 4);

    Config cfg              = {0};
    cfg.package_name        = str8_lit("gltf");
    cfg.strip_struct_prefix = str8_lit("cgltf_");
    expect_names("a package prefix unlike the library's is fine", &unit, &cfg, true);
  }

  ////////////////////////////////
  //~ pin-type: the C name has to survive to the use site
  //
  // A pinned type is the one case where the emitted 3b must NOT reduce a C name
  // to a primitive: `u64*` and `size_t*` are the same width but different C
  // types, so the substitution that `type-map` performs is exactly what makes
  // the generated call warn. See emit_pinned_type_aliases.

  { // The alias carries the C spelling, and uses name it rather than u64.
    CUnit unit  = {0};
    CType  ptr  = ctype_ptr_to(ctype_named("size_t"));
    add_function(&unit, "c_f", ctype_named("void"), &ptr, 1);

    Config cfg       = {0};
    cfg.package_name = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");
    PinTypeEntry e   = { str8_lit("size_t"), str8_lit("u64"), {0} };
    dyn_push(ctx_perm(), cfg.pin_type, e);

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("pin-type emits an alias pinned to the C spelling", text, "(alias size_t u64 \"size_t\")", true);
    expect_emitted("pin-type is used at the use site", text, "size_t*", true);
    expect_emitted("pin-type does not fall through to the primitive", text, "[x u64*]", false);
  }

  { // Without the pin, the same header is substituted down to the primitive --
    // right for every other type, wrong for this one.
    CUnit unit = {0};
    CType  ptr = ctype_ptr_to(ctype_named("size_t"));
    add_function(&unit, "c_f", ctype_named("void"), &ptr, 1);

    Config cfg            = {0};
    cfg.package_name      = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("unpinned size_t becomes u64", text, "[x u64*]", true);
    expect_emitted("unpinned size_t emits no alias", text, "(alias size_t", false);
  }

  { // A pinned name also wins over a `type-map` entry naming the same type;
    // otherwise the substitution would silently defeat the pin.
    CUnit unit = {0};
    CType  ptr = ctype_ptr_to(ctype_named("cgltf_size"));
    add_function(&unit, "c_f", ctype_named("void"), &ptr, 1);

    Config cfg            = {0};
    cfg.package_name      = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");
    PinTypeEntry pin  = { str8_lit("cgltf_size"), str8_lit("u64"), {0} };
    TypeMapEntry map  = { str8_lit("cgltf_size"), str8_lit("u64") };
    dyn_push(ctx_perm(), cfg.pin_type, pin);
    dyn_push(ctx_perm(), cfg.type_map, map);

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("pin-type beats type-map at the use site", text, "cgltf_size*", true);
    expect_emitted("pin-type beats type-map, no primitive fallthrough", text, "[x u64*]", false);
  }

  { // A pinned typedef is not ALSO emitted by emit_typedef -- one declaration,
    // with the C spelling, not two competing ones.
    CUnit    unit = {0};
    CTypedef td   = {0};
    td.name       = str8_lit("size_t");
    td.underlying = ctype_named("unsigned long");
    dyn_push(ctx_perm(), unit.typedefs, td);

    Config cfg       = {0};
    cfg.package_name = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");
    PinTypeEntry e   = { str8_lit("size_t"), str8_lit("u64"), {0} };
    dyn_push(ctx_perm(), cfg.pin_type, e);

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("captured typedef keeps the pinned form", text, "(alias size_t u64 \"size_t\")", true);
    expect_emitted("captured typedef is not emitted twice", text, "(alias size_t u64)", false);
  }

  { // A pin may stand on a DIFFERENT C spelling than the name it pins, for the
    // case where the name itself is invisible to the generated header (SDL's
    // Uint64, which the .h never sees, standing on uint64_t, which it does).
    CUnit unit  = {0};
    CType ptr   = ctype_ptr_to(ctype_named("Uint64"));
    add_function(&unit, "c_f", ctype_named("void"), &ptr, 1);

    Config cfg            = {0};
    cfg.package_name      = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");
    PinTypeEntry e = { str8_lit("Uint64"), str8_lit("u64"), str8_lit("uint64_t") };
    dyn_push(ctx_perm(), cfg.pin_type, e);

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("a pin can stand on another C spelling", text, "(alias Uint64 u64 \"uint64_t\")", true);
    expect_emitted("the pinned NAME is what use sites see", text, "[x Uint64*]", true);
  }

  ////////////////////////////////
  //~ Mirror pins: every struct/enum names the C type it mirrors
  //
  // Unlike a pin-type alias, this does not change the emitted 3b type at all --
  // the mirror is still an independent struct with the package's own name. It
  // is read only by codegen, to cast at an FFI call rather than hand gcc a
  // `t_data*` where the library declared `cgltf_data*`. See cg_ffi_c_type.

  { // A record reached only through its `struct` tag is spelled with it.
    CUnit    unit = {0};
    CRecord* r    = add_record(&unit, "cgltf_data", 8, 8);
    add_field(r, "scenes_count", ctype_named("unsigned long"), 0);

    Config cfg              = {0};
    cfg.package_name        = str8_lit("t");
    cfg.strip_struct_prefix = str8_lit("cgltf_");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("a tag-only record pins `struct C`", text, "(struct data \"struct cgltf_data\"", true);
  }

  { // `typedef struct { ... } Name;` leaves NO tag to write: `struct Name` in
    // the generated C would name a fresh incomplete type, not the record. The
    // walk records that on the CRecord, since it is the only place it shows.
    CUnit    unit = {0};
    CRecord* r    = add_record(&unit, "cgltf_data", 8, 8);
    add_field(r, "scenes_count", ctype_named("unsigned long"), 0);
    r->has_typedef_name = true;

    Config cfg              = {0};
    cfg.package_name        = str8_lit("t");
    cfg.strip_struct_prefix = str8_lit("cgltf_");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("a typedef'd record pins the bare name", text, "(struct data \"cgltf_data\"", true);
    expect_emitted("...and not a tag that names nothing", text, "\"struct cgltf_data\"", false);
  }

  { // Enums take the same operand, and need their own keyword back.
    CUnit unit = {0};
    CEnum e    = {0};
    e.name     = str8_lit("AVPixelFormat");
    CEnumerator m = {0};
    m.name        = str8_lit("AV_PIX_FMT_RGB24");
    m.value_text  = str8_lit("2");
    dyn_push(ctx_perm(), e.enumerators, m);
    dyn_push(ctx_perm(), unit.enums, e);

    Config cfg       = {0};
    cfg.package_name = str8_lit("t");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("an enum pins `enum C`", text, "(enum AVPixelFormat \"enum AVPixelFormat\"", true);
  }

  ////////////////////////////////
  //~ skip-deprecated

  { // Binding a deprecated function costs a -Wdeprecated-declarations warning
    // in the generated .c whether or not anything ever calls the wrapper --
    // the WRAPPER's own body is the call site. `(skip-deprecated)` drops them.
    CUnit unit = {0};
    add_function(&unit, "c_old", ctype_named("void"), NULL, 0);
    add_function(&unit, "c_new", ctype_named("void"), NULL, 0);
    unit.functions[0].is_deprecated = true;

    Config cfg            = {0};
    cfg.package_name      = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");
    cfg.skip_deprecated   = true;

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("skip-deprecated drops the deprecated function", text, "c_old", false);
    expect_emitted("skip-deprecated keeps the rest", text, "c_new", true);
  }

  { // Opt-in: dropping functions is an API change, so the default binds them.
    CUnit unit = {0};
    add_function(&unit, "c_old", ctype_named("void"), NULL, 0);
    unit.functions[0].is_deprecated = true;

    Config cfg            = {0};
    cfg.package_name      = str8_lit("t");
    cfg.strip_func_prefix = str8_lit("c_");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("deprecated functions are bound by default", text, "c_old", true);
  }

  ////////////////////////////////
  //~ Function-pointer types

  { // A function pointer returning a function pointer has no 3b spelling and
    // collapses to `any` -- including when it returns one BY TYPEDEF NAME,
    // which is how GL's PFNGLGETVKPROCADDRNVPROC spells it. Emitting the name
    // instead produces 3b that fails to lower ("a function type cannot return
    // another function type directly").
    CUnit    unit = {0};
    CTypedef td   = {0};
    td.name       = str8_lit("GLVULKANPROCNV");
    td.underlying = ctype_fn_ptr(ctype_named("void"), 0, 0);
    dyn_push(ctx_perm(), unit.typedefs, td);

    CTypedef outer = {0};
    outer.name     = str8_lit("PFNGETPROC");
    outer.underlying = ctype_fn_ptr(ctype_named("GLVULKANPROCNV"), 0, 0);
    dyn_push(ctx_perm(), unit.typedefs, outer);

    Config cfg       = {0};
    cfg.package_name = str8_lit("t");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("a fn returning a typedef'd fn collapses to any", text, "(alias PFNGETPROC (fn [] any))", true);
    expect_emitted("...and does not name the fn typedef", text, "] GLVULKANPROCNV)", false);
  }

  ////////////////////////////////
  //~ Field naming

  { // Fields are kebab-cased the same way function names are, from either C
    // spelling. What makes this worth pinning down rather than trusting to
    // camel_to_kebab's own tests is that a mirror's fields are the ONE set of
    // emitted names with a byte-level obligation behind them -- so the same
    // case also asserts the layout still verifies, i.e. that renaming moved
    // nothing.
    CUnit    unit = {0};
    CRecord* r    = add_record(&unit, "AVCodecContext", 24, 8);
    add_field(r, "bit_rate", ctype_named("int64_t"), 0);
    add_field(r, "gop_size", ctype_named("int"), 8);
    add_field(r, "windowID", ctype_named("int"), 12);
    add_field(r, "width", ctype_named("int"), 16);

    Config cfg       = {0};
    cfg.package_name = str8_lit("t");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("a snake_case field kebab-cases", text, "bit-rate i64", true);
    expect_emitted("...and the C spelling is gone", text, "bit_rate", false);
    expect_emitted("a camelCase field kebab-cases too", text, "window-id i32", true);
    expect_emitted("a field with no separators is left alone", text, "width i32", true);
    expect_layout("kebab-cased fields still verify", &unit, &cfg, true);
  }

  { // Two C fields wanting one kebab name: the first claimant keeps it and the
    // second stays at its C spelling, so the mirror still has one field per C
    // field. Silently emitting the name twice would give the mirror a
    // duplicate and shift every field after it.
    CUnit    unit = {0};
    CRecord* r    = add_record(&unit, "Clash", 8, 4);
    add_field(r, "foo_bar", ctype_named("int"), 0);
    add_field(r, "fooBar", ctype_named("int"), 4);

    Config cfg       = {0};
    cfg.package_name = str8_lit("t");

    String8 text = emit_to_string(&cfg, &unit);
    expect_emitted("the first claimant gets the kebab name", text, "foo-bar i32", true);
    expect_emitted("the second keeps its C spelling", text, "fooBar i32", true);
  }

  if (g_failures == 0) printf("translate_emit_test: all checks passed\n");
  else                 printf("translate_emit_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
