// bcio_corrupt_test.c -- bc_program_load against a DAMAGED `.3bc`.
//
// bcio_verify_test.c covers the cache file that is well-formed but STALE (a
// host import whose signature has since changed). This covers the one that is
// not well-formed at all: bytes patched to values bcgen.c would never emit.
// That is not a hypothetical -- a `.3bc` is an ordinary file sitting next to
// its source, so a truncated write, a bad disk, or a stray edit all produce
// one, and the loader mmaps it and the VM executes it.
//
// The bug this pins: the loader's cursor kept every READ inside the mapping,
// but nothing checked that the instructions it read were RUNNABLE. A patched
// jump target walked `pc` off the end of the code array (SEGV), and a patched
// opcode indexed bcvm.c's computed-goto dispatch table out of bounds and
// jumped through whatever pointer followed it. Both are now rejected at load
// time by bcio.c's bcio_verify_loaded_code, plus a range check on the
// dispatch itself.
//
// The check is deliberately "no crash, and `ok` is honest" rather than a
// pinned reject/accept per offset: which byte means what is bcio.c's own
// business and changes with the format. A patch that lands somewhere
// inconsequential SHOULD still load fine, so accepting is not a failure --
// crashing is. Run this under `make sanitize` to make the OOB reads it
// guards against actually fatal rather than silent.
#include "3b.h"
#include "bcgen.h"
#include "bcvm.h"
#include "bcio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

static void
expect_true(const char* what, b32 got) {
  if (!got) {
    fprintf(stderr, "FAIL %s: got false, want true\n", what);
    g_failures += 1;
  }
}

// Small, but with a loop, a call and a string constant, so the code array has
// real jump targets and const-pool slots to corrupt.
static const char* g_fixture_source =
  "(fn add [a i32 b i32] i32 (+ a b))\n"
  "(fn main [] i32\n"
  "  (let [acc i32 0]\n"
  "    (for [i 0 8]\n"
  "      (set acc (add acc i)))\n"
  "    (println \"acc={}\" acc)\n"
  "    acc))\n";

static Checker
check_fixture(TypedAst* tast, TypedIndex* out_root) {
  Ast ast;
  ast_init(&ast, ctx_perm());

  String8 src     = str8_cstring((char*)(g_fixture_source));
  u32     file_id = source_file_register(str8_lit("bcio_corrupt_test_fixture.3b"), src);

  Parser p;
  parser_init(&p, src, &ast, file_id);
  NodeIndex root = parse_program(&p);
  if (p.had_error) { fprintf(stderr, "FATAL: fixture failed to parse\n"); exit(1); }

  typed_ast_init(tast, ctx_perm());
  Lowerer low = {0};
  low.ast  = &ast;
  low.tast = tast;
  TypedIndex own_root = lower_program(&low, root);
  if (low.had_error) { fprintf(stderr, "FATAL: fixture failed to lower\n"); exit(1); }

  *out_root = own_root;
  return check_program(tast, own_root, /*is_root_package=*/true, /*scope_query=*/NULL);
}

static u8*
read_file_bytes(const char* path, u64* out_size) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "FATAL: cannot reopen %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "FATAL: short read\n"); exit(1); }
  fclose(f);
  *out_size = (u64)n;
  return buf;
}

int
main(void) {
  Context ctx;
  ctx_init(&ctx, MB(16));

  TypedAst   tast;
  TypedIndex root;
  Checker    ck = check_fixture(&tast, &root);

  const char* good_path = "/tmp/3b_bcio_corrupt_test_good.3bc";
  const char* bad_path  = "/tmp/3b_bcio_corrupt_test_bad.3bc";
  remove(good_path);

  ArenaTemp   compile_temp = arena_temp_begin(ctx_scratch());
  LayoutCache layout_cache;
  layout_cache_init(&layout_cache, ctx_perm());

  BcProgram prog = bc_compile_program(&ck, &tast, root, compile_temp.arena, ctx_perm(),
                                       &layout_cache, NULL, NULL);
  u64 content_hash = 0x0123456789abcdefULL;
  if (!bc_program_save(&prog, NULL, str8_cstring((char*)good_path), content_hash)) {
    fprintf(stderr, "FATAL: bc_program_save failed\n");
    exit(1);
  }

  // The baseline has to actually load, or every "didn't crash" below would be
  // vacuous -- a file rejected at the magic number never reaches the code.
  {
    ArenaTemp    t      = arena_temp_begin(ctx_scratch());
    BcLoadResult loaded = bc_program_load(str8_cstring((char*)good_path), NULL, NULL, t.arena, ctx_perm());
    expect_true("the UNpatched fixture loads (baseline for the cases below)", loaded.ok);
    if (loaded.ok) bc_program_unload(&loaded);
    arena_temp_end(&t);
  }

  u64 size  = 0;
  u8* bytes = read_file_bytes(good_path, &size);

  // Every 4-byte-aligned field from the end of the fixed header onward, set to
  // each of a few hostile values: huge counts, huge indices, all-ones. Between
  // them these hit opcodes, jump targets, register numbers, chunk indices,
  // array lengths and string lengths without this test needing to know which
  // offset is which.
  static const u64 hostile[] = {
    0x2000000000000000ull, // overflows a `count * elem_size` bounds check
    0xffffffffffffffffull,
    0x7fffffffffffffffull,
    0x0000000100000001ull,
    0x00000000ffffffffull,
  };

  u64 cases = 0, accepted = 0, rejected = 0;
  for (u64 off = 16; off + 8 <= size; off += 4) {
    for (u64 vi = 0; vi < ArrayCount(hostile); vi += 1) {
      u8* patched = (u8*)malloc((size_t)size);
      MemoryCopy(patched, bytes, size);
      MemoryCopy(patched + off, &hostile[vi], sizeof(u64));

      FILE* o = fopen(bad_path, "wb");
      fwrite(patched, 1, (size_t)size, o);
      fclose(o);
      free(patched);

      // The load itself is the assertion: it must return, not read out of
      // bounds. `ok` either way is legitimate -- see this file's header.
      ArenaTemp    t      = arena_temp_begin(ctx_scratch());
      BcLoadResult loaded = bc_program_load(str8_cstring((char*)bad_path), NULL, NULL, t.arena, ctx_perm());
      if (loaded.ok) { accepted += 1; bc_program_unload(&loaded); }
      else           { rejected += 1; }
      arena_temp_end(&t);
      cases += 1;
    }
  }

  free(bytes);
  arena_temp_end(&compile_temp);
  remove(good_path);
  remove(bad_path);

  expect_true("every corrupted load returned instead of crashing", cases > 0);
  printf("bcio_corrupt_test: %llu corrupted file(s) loaded without a crash (%llu rejected, %llu accepted)\n",
         (unsigned long long)cases, (unsigned long long)rejected, (unsigned long long)accepted);

  if (g_failures == 0) printf("bcio_corrupt_test: all checks passed\n");
  else                 printf("bcio_corrupt_test: %d check(s) FAILED\n", g_failures);

  ctx_free();
  return g_failures == 0 ? 0 : 1;
}
