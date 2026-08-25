CC = gcc
LLVM_PREFIX = /usr/lib/llvm-19
TARGET = 3b

# `3b translate` (the C header -> 3b binding generator) needs libclang for
# ITS OWN build, a real separate dependency -- set WITHOUT_TRANSLATE=1 to
# build the rest of the compiler without it (e.g. when no matching
# libclang is available for the target platform; see the `windows` target
# below, which sets this itself). `3b translate` just reports "not
# available" in a build like that (see main.c) -- everything else works.
WITHOUT_TRANSLATE ?= 0

# A plain `make` builds a DEBUG toolchain: unoptimized, with -DXDEBUG so
# xassert and the arena poisoning are compiled in. RELEASE=1 (what the
# `release` target below sets, and what CI builds a shipped binary with)
# swaps in exactly the flags `3b build --release` gives a 3b PROGRAM -- -O2
# with those debug-only checks compiled out, -g kept either way (see
# build_invoke_toolchain in build.c). Set on a command line it propagates to
# every sub-make, so it composes with the recursive targets: `make RELEASE=1
# windows` cross-compiles a release 3b.exe.
#
# `-O2` and NOT `-O2 -DNDEBUG`, which is the reflex: base.c guards its
# entry points with plain `assert` (a null Context, a zero arena size, an
# `os_entity_alloc` before `os_state_init`, `async_run_phase` called before
# the last phase finished) and those are meant to survive into a shipped
# binary. They are caller errors, once per process/thread/phase, and each
# one aborts at the mistake instead of corrupting an arena and crashing
# somewhere unrelated. -DNDEBUG deletes all 20 silently. The debug-only
# checks -- the ones that ARE hot, and are internal invariants rather than
# preconditions -- are spelled `xassert` and keyed off XDEBUG instead
# (base.h), which is the knob this line is for.
RELEASE ?= 0
ifeq ($(RELEASE),1)
OPT_CFLAGS = -O2
else
OPT_CFLAGS = -O0 -DXDEBUG
endif

CFLAGS = -Wall -std=c11 -pedantic -Wno-missing-braces -Wmissing-field-initializers -D_XOPEN_SOURCE=600 -D_GNU_SOURCE $(OPT_CFLAGS) -g -I. -MMD -MP
LFLAGS = -lm -lpthread -ldl

ifeq ($(WITHOUT_TRANSLATE),1)
CFLAGS += -DBBB_NO_TRANSLATE
else
CFLAGS += -I$(LLVM_PREFIX)/include
LFLAGS += -L$(LLVM_PREFIX)/lib -lclang
endif

BASE_SOURCES = base/base.c
BASE_OBJS    = $(BASE_SOURCES:.c=.o)

CORE_SOURCES      = main.c compiler.c file.c atom.c lexer.c parser.c lower.c checker.c codegen.c 3b.c format.c diag.c build.c layout.c \
                    bcgen.c bcvm.c bcio.c bcnative.c bcosprims.c bcmap.c script.c script_native.c
TRANSLATE_SOURCES = translate/translate.c translate/config.c translate/cwalk.c translate/emit.c translate/rename.c bcconfigprims.c

ifeq ($(WITHOUT_TRANSLATE),1)
SOURCE_FILES = $(CORE_SOURCES)
else
SOURCE_FILES = $(CORE_SOURCES) $(TRANSLATE_SOURCES)
endif

OBJFILES = $(SOURCE_FILES:.c=.o) $(BASE_OBJS)
DEPFILES = $(OBJFILES:.o=.d)

# `liblib3b.a` -- the compiler's check-only pipeline (lib3b.h/lib3b.c),
# exposed as a linkable library instead of the `3b` CLI -- see lib3b.h.
# Everything CORE_SOURCES has except main.c (CLI argv dispatch), plus
# lib3b.c itself; deliberately
# excludes TRANSLATE_SOURCES so linking this library never requires
# libclang. Part of `all` (build.c's build_invoke_toolchain silently
# assumes it sits next to the `3b` binary whenever a project imports the
# embedded `vm` package -- see self_liblib3b_path in build.c) -- still
# buildable on its own via `make lib`.
LIB_SOURCES  = $(filter-out main.c,$(CORE_SOURCES)) lib3b.c
LIB_OBJFILES = $(LIB_SOURCES:.c=.o) $(BASE_OBJS)
LIB_DEPFILES = $(LIB_OBJFILES:.o=.d)
LIB_TARGET   = liblib3b.a

# `3b-lsp` -- the Language Server (see lsp/lsp_main.c), linking $(LIB_TARGET)
# instead of talking to `3b` as a subprocess.
# `-lm -lpthread -ldl` directly (not $(LFLAGS)) since
# $(LIB_TARGET) excludes TRANSLATE_SOURCES and so never needs -lclang.
# `3b-lsp-test` is the committed regression test (lsp/lsp_test.c) that
# spawns 3b-lsp and drives the wire protocol against it.
LSP_SOURCES       = lsp/json.c lsp/lsp_main.c
LSP_OBJFILES      = $(LSP_SOURCES:.c=.o)
LSP_TEST_OBJFILES = lsp/json.o lsp/lsp_test.o
LSP_DEPFILES      = $(LSP_OBJFILES:.o=.d) lsp/lsp_test.d
LIB_LFLAGS        = -lm -lpthread -ldl

# The real, editable runtime module files codegen.c's cg_write_runtime_header/
# cg_write_runtime_source assemble into every build's own 3b_runtime.h/.c --
# baked into the `3b` binary itself at ITS OWN build time (tools/embed_runtime
# -> runtime_embed.h) so `3b` stays a single self-contained binary with no
# runtime/ dependency once built. Order here must match the concatenation
# order in codegen.c.
RUNTIME_HEADERS = runtime/bbb_prelude.h runtime/bbb_arena.h runtime/bbb_thread.h \
                  runtime/bbb_context.h runtime/bbb_handle.h runtime/bbb_string.h runtime/bbb_file.h \
                  runtime/bbb_os.h runtime/bbb_hashtable.h
RUNTIME_SOURCES = runtime/bbb_arena.c runtime/bbb_context.c runtime/bbb_thread.c \
                  runtime/bbb_string.c runtime/bbb_file.c runtime/bbb_os.c
RUNTIME_FILES   = $(RUNTIME_HEADERS) $(RUNTIME_SOURCES)

all: $(TARGET) $(LIB_TARGET)

$(TARGET): $(OBJFILES)
	$(CC) $(CFLAGS) $(OBJFILES) $(LFLAGS) -o $@

.PHONY: lib
lib: $(LIB_TARGET)

$(LIB_TARGET): $(LIB_OBJFILES)
	ar rcs $@ $(LIB_OBJFILES)

.PHONY: lsp lsp-test
lsp: 3b-lsp

3b-lsp: $(LSP_OBJFILES) $(LIB_TARGET)
	$(CC) $(CFLAGS) $(LSP_OBJFILES) $(LIB_TARGET) $(LIB_LFLAGS) -o $@

3b-lsp-test: $(LSP_TEST_OBJFILES)
	$(CC) $(CFLAGS) $(LSP_TEST_OBJFILES) -o $@

lsp-test: 3b-lsp 3b-lsp-test
	./3b-lsp-test ./3b-lsp

# layout.c's own regression test (see test/layout_test.c) -- hand-derived
# expected sizes/offsets PLUS a cross-check against a real `cc`-compiled
# probe. Links against $(LIB_TARGET) (parser/lower/checker/layout, no
# codegen/translate needed) same as 3b-lsp above.
.PHONY: layout-test
layout-test: test/layout_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/layout_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/layout_test
	./test/layout_test

# bcgen.c/bcvm.c's own regression test (see test/bcgen_test.c) -- compiles
# a handful of fixture functions to bytecode and runs them through the
# interpreter, checking results against hand-computed expected values.
.PHONY: bcgen-test
bcgen-test: test/bcgen_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_test
	./test/bcgen_test

# Struct-specific follow-up to bcgen-test (see test/bcgen_struct_test.c) --
# construction, nested by-value fields, and reading through get/get-in.
.PHONY: bcgen-struct-test
bcgen-struct-test: test/bcgen_struct_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_struct_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_struct_test
	./test/bcgen_struct_test

# floats/strings/handles as struct fields (see test/bcgen_types_test.c).
.PHONY: bcgen-types-test
bcgen-types-test: test/bcgen_types_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_types_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_types_test
	./test/bcgen_types_test

# calls between compiled functions, including recursion (see test/bcgen_call_test.c).
.PHONY: bcgen-call-test
bcgen-call-test: test/bcgen_call_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_call_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_call_test
	./test/bcgen_call_test

# f32, array fields, and arbitrary-value nested-struct fields (see test/bcgen_extra_test.c).
.PHONY: bcgen-extra-test
bcgen-extra-test: test/bcgen_extra_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_extra_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_extra_test
	./test/bcgen_extra_test

# Host imports (BcOp_CallHost) -- real native C functions called from
# compiled bytecode via `extern fn` (see test/bcgen_host_test.c).
.PHONY: bcgen-host-test
bcgen-host-test: test/bcgen_host_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_host_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_host_test
	./test/bcgen_host_test

# `set`/mutation -- identifier/deref/field/index targets, `var` locals
# (see test/bcgen_set_test.c).
.PHONY: bcgen-set-test
bcgen-set-test: test/bcgen_set_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_set_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_set_test
	./test/bcgen_set_test

# Loop constructs -- while/range for (see test/bcgen_loop_test.c).
.PHONY: bcgen-loop-test
bcgen-loop-test: test/bcgen_loop_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_loop_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_loop_test
	./test/bcgen_loop_test

# String/struct content comparison (see bc_compile_value_cmp in
# bcgen.c/test/bcgen_cmp_test.c).
.PHONY: bcgen-cmp-test
bcgen-cmp-test: test/bcgen_cmp_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_cmp_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_cmp_test
	./test/bcgen_cmp_test

# `and`/`or` (real short-circuit)/`not`/unary `-`/`+`/`string-len`/
# `cstring`/`sizeof`/`alignof`/`member-offset`/`zero`/`nil` (see
# test/bcgen_ops_test.c).
.PHONY: bcgen-ops-test
bcgen-ops-test: test/bcgen_ops_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_ops_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_ops_test
	./test/bcgen_ops_test

# Module-level `var`/`val` globals -- BcOp_LoadGlobal/StoreGlobal,
# BcProgram.globals, the synthesized `#init_globals` chunk (run
# automatically by both bc_compile_program and bc_program_load) (see
# test/bcgen_globals_test.c).
.PHONY: bcgen-globals-test
bcgen-globals-test: test/bcgen_globals_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_globals_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_globals_test
	./test/bcgen_globals_test

# `cast` (real int<->float conversion, int width adjustment, bool
# truthiness, void discard, pointer<->any passthrough) + the 5 bitwise
# binops/`bit-not` (see test/bcgen_cast_test.c).
.PHONY: bcgen-cast-test
bcgen-cast-test: test/bcgen_cast_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_cast_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_cast_test
	./test/bcgen_cast_test

# `reinterpret` (real bit-pattern copy, not a numeric conversion -- f32<->
# i32/f64<->i64 round trips, negative-value sign/zero-extension
# normalization, pointer<->any) -- see test/bcgen_reinterpret_test.c.
.PHONY: bcgen-reinterpret-test
bcgen-reinterpret-test: test/bcgen_reinterpret_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_reinterpret_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_reinterpret_test
	./test/bcgen_reinterpret_test

# `EnumAccess` (`Name/Variant`) -- a compile-time constant replicating
# codegen.c's cg_enum_decl auto-assignment algorithm (see
# test/bcgen_enum_test.c).
.PHONY: bcgen-enum-test
bcgen-enum-test: test/bcgen_enum_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_enum_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_enum_test
	./test/bcgen_enum_test

# Arena support -- `create`/`destroy`/`reset`/`release`/`mark`/`pop`,
# `push`/`push0`/push-with-value, `scratch`, `alloc`/`free` (see
# test/bcgen_arena_test.c).
.PHONY: bcgen-arena-test
bcgen-arena-test: test/bcgen_arena_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_arena_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_arena_test
	./test/bcgen_arena_test

# The lane system under the VM's one-lane serial fallback -- `parallel`,
# `parallel-for`, `lane-index`/`-count`/`-sync`/`-arena` (see
# test/bcgen_lane_test.c).
.PHONY: bcgen-lane-test
bcgen-lane-test: test/bcgen_lane_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_lane_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_lane_test
	./test/bcgen_lane_test

# `sqrt-checked`/`asin-checked`/`acos-checked`/`pow-checked` -- real libm
# calls (always in f64) + isfinite, building the synthesized `(bool T)`
# result struct (see test/bcgen_checked_math_test.c).
.PHONY: bcgen-checked-math-test
bcgen-checked-math-test: test/bcgen_checked_math_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_checked_math_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_checked_math_test
	./test/bcgen_checked_math_test

# Handle pools -- `(handle Name)`/`handle-pool-init`/`handle-alloc`/
# `handle-deref`/`handle-free`/`handle-valid?` (see
# test/bcgen_handle_test.c).
.PHONY: bcgen-handle-test
bcgen-handle-test: test/bcgen_handle_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_handle_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_handle_test
	./test/bcgen_handle_test

# Collection `for` over Array/Vector (see test/bcgen_foreach_test.c).
.PHONY: bcgen-foreach-test
bcgen-foreach-test: test/bcgen_foreach_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_foreach_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_foreach_test
	./test/bcgen_foreach_test

# `string-to-i32`/`string-to-i64`/`string-to-u32`/`string-to-u64`/
# `string-to-f32`/`string-to-f64` (see test/bcgen_parse_number_test.c).
.PHONY: bcgen-parse-number-test
bcgen-parse-number-test: test/bcgen_parse_number_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_parse_number_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_parse_number_test
	./test/bcgen_parse_number_test

# `vector-index-of` (see test/bcgen_index_of_test.c).
.PHONY: bcgen-index-of-test
bcgen-index-of-test: test/bcgen_index_of_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_index_of_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_index_of_test
	./test/bcgen_index_of_test

# `dyn-push`/`vector-push`/`commit` (see test/bcgen_dyn_push_test.c).
.PHONY: bcgen-dyn-push-test
bcgen-dyn-push-test: test/bcgen_dyn_push_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_dyn_push_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_dyn_push_test
	./test/bcgen_dyn_push_test

# `print`/`println` (see test/bcgen_print_test.c).
.PHONY: bcgen-print-test
bcgen-print-test: test/bcgen_print_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_print_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_print_test
	./test/bcgen_print_test

# Integer arithmetic WRAPPING at its own type's width instead of at the
# register's 64 bits, plus `bit-shr`/divide/compare signedness (see
# test/bcgen_wrap_test.c). The unit-level counterpart to `make backend-diff`
# below, which catches the same class end-to-end by diffing the two backends.
.PHONY: bcgen-wrap-test
bcgen-wrap-test: test/bcgen_wrap_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_wrap_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_wrap_test
	./test/bcgen_wrap_test

# i8/u8/i16/u16/char field and array-element load/store (see
# test/bcgen_narrow_int_test.c).
.PHONY: bcgen-narrow-int-test
bcgen-narrow-int-test: test/bcgen_narrow_int_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_narrow_int_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_narrow_int_test
	./test/bcgen_narrow_int_test

# Nested-scope shadowing (`do`/`let`/`for`/`scratch` popping fc->locals
# back on close -- see test/bcgen_scope_test.c).
.PHONY: bcgen-scope-test
bcgen-scope-test: test/bcgen_scope_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_scope_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_scope_test
	./test/bcgen_scope_test

# `string-match` (see test/bcgen_string_match_test.c).
.PHONY: bcgen-string-match-test
bcgen-string-match-test: test/bcgen_string_match_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_string_match_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_string_match_test
	./test/bcgen_string_match_test

# `mem-set`/`mem-copy`/`mem-zero`/`mem-compare` (see test/bcgen_mem_test.c).
.PHONY: bcgen-mem-test
bcgen-mem-test: test/bcgen_mem_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_mem_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_mem_test
	./test/bcgen_mem_test

# `vector-clear`/`vector-swap-remove`/`vector-remove-at`/`vector-contains?`
# (see test/bcgen_vector_remove_test.c).
.PHONY: bcgen-vector-remove-test
bcgen-vector-remove-test: test/bcgen_vector_remove_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_vector_remove_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_vector_remove_test
	./test/bcgen_vector_remove_test

# Map<K,V>/Set<T> support -- map-set/map-get/map-contains?/map-remove/
# set-add/set-contains?/set-remove + collection `for` (see
# test/bcgen_map_test.c).
.PHONY: bcgen-map-test
bcgen-map-test: test/bcgen_map_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_map_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_map_test
	./test/bcgen_map_test

# `(addr x)`/`&x` on a scalar local via address-taken analysis (see
# test/bcgen_addr_taken_test.c).
.PHONY: bcgen-addr-taken-test
bcgen-addr-taken-test: test/bcgen_addr_taken_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_addr_taken_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_addr_taken_test
	./test/bcgen_addr_taken_test

# `addr`/`&`, `swap`, bare `dyn-count`, `nth-checked`, `len` (see
# test/bcgen_extra2_test.c).
.PHONY: bcgen-extra2-test
bcgen-extra2-test: test/bcgen_extra2_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_extra2_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_extra2_test
	./test/bcgen_extra2_test

# The `.3bs` driver (see script.c/test/script_test.c).
.PHONY: script-test
script-test: test/script_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/script_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/script_test
	./test/script_test

# Generic OS-facing host imports for `.3bs` scripts (see
# bcosprims.c/test/bcosprims_test.c).
.PHONY: bcosprims-test
bcosprims-test: test/bcosprims_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcosprims_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcosprims_test
	./test/bcosprims_test

# Config-mutation host imports for `.3bs` translator-config scripts (see
# bcconfigprims.c/test/bcconfigprims_test.c). bcconfigprims.o is linked
# directly (not through $(LIB_TARGET), which deliberately excludes
# TRANSLATE_SOURCES) -- it has no actual libclang dependency itself (only
# translate.h's plain Config structs), so this links clean without -lclang.
.PHONY: bcconfigprims-test
bcconfigprims-test: test/bcconfigprims_test.c bcconfigprims.o $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcconfigprims_test.c bcconfigprims.o $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcconfigprims_test
	./test/bcconfigprims_test

# Pattern-rename engine for translator configs (see translate/rename.c and
# test/rename_test.c). Links translate/rename.o directly for the same
# reason bcconfigprims-test above does: it's part of TRANSLATE_SOURCES,
# which $(LIB_TARGET) deliberately excludes, but it only needs translate.h's
# plain structs plus libc's <regex.h> -- no libclang.
.PHONY: rename-test
rename-test: test/rename_test.c translate/rename.o $(LIB_TARGET)
	$(CC) $(CFLAGS) test/rename_test.c translate/rename.o $(LIB_TARGET) $(LIB_LFLAGS) -o test/rename_test
	./test/rename_test

# The two checks that can stop `3b translate` writing a binding at all --
# struct-mirror layout verification and the by-value-struct skip (see
# translate/emit.c and test/translate_emit_test.c). Links translate/emit.o for
# the same reason rename-test above links rename.o; the CUnits it checks are
# built by hand, so no libclang is involved here either.
.PHONY: translate-emit-test
translate-emit-test: test/translate_emit_test.c translate/emit.o translate/rename.o $(LIB_TARGET)
	$(CC) $(CFLAGS) test/translate_emit_test.c translate/emit.o translate/rename.o $(LIB_TARGET) $(LIB_LFLAGS) -o test/translate_emit_test
	./test/translate_emit_test

# Serialization/caching (see bcio.c/test/bcgen_io_test.c).
.PHONY: bcgen-io-test
bcgen-io-test: test/bcgen_io_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_io_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_io_test
	./test/bcgen_io_test

# Load-time host-import signature re-verification (see
# bc_verify_cached_host_imports in bcio.c/test/bcio_verify_test.c).
.PHONY: bcio-verify-test
bcio-verify-test: test/bcio_verify_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcio_verify_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcio_verify_test
	./test/bcio_verify_test

# Real zero-marshaling direct host calls (see bcnative.c/test/bcgen_native_test.c).
.PHONY: bcgen-native-test
bcgen-native-test: test/bcgen_native_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_native_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_native_test
	./test/bcgen_native_test

# Typed host-signature verification (see bc_verify_host_imports in
# bcgen.c/test/bcgen_verify_test.c).
.PHONY: bcgen-verify-test
bcgen-verify-test: test/bcgen_verify_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/bcgen_verify_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/bcgen_verify_test
	./test/bcgen_verify_test

# format.c's own regression test (see test/format_test.c) -- idempotence and
# formatter-output-re-parses over every `.3b` file in the tree, semantic
# preservation (same generated C before and after formatting) on self-contained
# fixtures, plus a few small exact-rendering goldens. Runs from the repo root,
# which is where it looks for the corpus.
.PHONY: format-test
format-test: test/format_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/format_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/format_test
	./test/format_test

# The corpus-scale half of the same property, at PACKAGE granularity, which
# test/format_test.c structurally cannot reach: build every example package,
# format every file in it, rebuild, and require byte-identical generated C.
# Drives the real `3b` binary over copies in a temp directory -- the working
# tree is never touched. This is the by-hand ritual that used to be performed
# whenever format.c was edited, made into a target.
.PHONY: format-corpus
format-corpus: $(TARGET)
	./tools/format_corpus.sh ./$(TARGET) .

# checker.c's negative tests (see test/checker_error_test.c) -- sources that
# must be REJECTED, each pinned to the diagnostic text and the line:col it must
# be rejected at, plus an accept table pinning the boundary of each rule.
# CHECKER_TEST_SHOW=1 dumps every case's actual diagnostics, which is how you
# find the line:col for a new case.
.PHONY: checker-error-test
checker-error-test: test/checker_error_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/checker_error_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/checker_error_test
	./test/checker_error_test

# codegen.c golden output (see test/codegen_golden_test.c) -- small `.3b`
# fixtures in test/golden/ with the header and source C they must produce
# checked in beside them. After an INTENDED codegen change, regenerate with
# `UPDATE_GOLDENS=1 ./test/codegen_golden_test` and read `git diff test/golden/`.
.PHONY: codegen-golden-test
codegen-golden-test: test/codegen_golden_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) test/codegen_golden_test.c $(LIB_TARGET) $(LIB_LFLAGS) -o test/codegen_golden_test
	./test/codegen_golden_test

# Runs every runnable example through BOTH backends and diffs their stdout
# and exit status (see test/backend_diff.sh). Every other suite in this file
# exercises ONE backend, so nothing checked the "same source runs on either
# backend" design goal until this existed -- and the first sweep it replaced
# found three real divergences. Needs the built `3b` and the examples, no
# compiled test binary of its own.
.PHONY: backend-diff
backend-diff: $(TARGET)
	./test/backend_diff.sh ./$(TARGET) examples

# Builds and runs every example with the GENERATED program under ASAN+UBSAN,
# and additionally under TSAN where it uses `parallel` (see
# test/sanitize_examples.sh). The complement of `make sanitize` below, which
# instruments the COMPILER: this is the only thing that sanitizes the C
# codegen.c emits and the runtime prelude that ships with it (arenas, dynamic
# arrays, the lane pool). Deliberately not part of `check` -- it rebuilds every
# example twice with instrumentation, so it is its own step rather than a
# tripling of `check`'s runtime.
.PHONY: sanitize-examples
sanitize-examples: $(TARGET)
	./test/sanitize_examples.sh ./$(TARGET) examples

########################################
# `make check` -- every test suite in the tree, in one command.
#
# The per-suite targets above stay as they are: each is the way to iterate on
# one suite, and each carries the note on what that suite covers. What was
# missing was a single command that says whether the tree is green.
#
# The binary list is a wildcard, not a hand-written list, so adding a
# test/*_test.c picks it up here (and in `clean`) with no Makefile edit. That
# is the actual fix: the hand-maintained list `clean` used had drifted to
# naming 19 of 39 test binaries, leaving the other 20 to survive a clean and
# go stale.
TEST_SOURCES = $(wildcard test/*_test.c)
TEST_BINS    = $(TEST_SOURCES:.c=)

test/%_test: test/%_test.c $(LIB_TARGET)
	$(CC) $(CFLAGS) $< $(TEST_EXTRA_OBJS) $(LIB_TARGET) $(LIB_LFLAGS) -o $@

# The suites needing objects beyond $(LIB_TARGET), which deliberately excludes
# TRANSLATE_SOURCES so that nothing linking it needs libclang. None of these
# objects actually has a libclang dependency -- cwalk.c is the only translator
# source that calls into it -- so all three still link with no -lclang and
# build fine under WITHOUT_TRANSLATE=1.
test/bcconfigprims_test:  TEST_EXTRA_OBJS = bcconfigprims.o
test/bcconfigprims_test:  bcconfigprims.o
test/rename_test:         TEST_EXTRA_OBJS = translate/rename.o
test/rename_test:         translate/rename.o
test/translate_emit_test: TEST_EXTRA_OBJS = translate/emit.o translate/rename.o
test/translate_emit_test: translate/emit.o translate/rename.o

# learn-3b-in-y-minutes.org is what README calls "the closest thing to a
# spec", and claims is "kept honest by being checked against real, compiling
# code under examples/" -- nothing enforced that until this target, and the
# file had drifted (a section still claiming `for` couldn't iterate a
# collection, a link left dangling by the stb/ -> stbimg/ rename, a
# build.cfg.3b snippet naming moved c-sources). See the script's own header
# for what the two checks do and do NOT cover.
.PHONY: check-doc
check-doc: $(TARGET)
	./tools/check_org_snippets.sh ./$(TARGET) learn-3b-in-y-minutes.org

# Runs every suite even after one fails, then reports which ones did and exits
# non-zero -- one run should show the whole picture, not just the first
# breakage. The LSP's wire-protocol test, the formatter's package-level corpus
# sweep, the os-module backend-parity check and the doc check are real suites
# too, so they run here alongside the standalone binaries.
#
# format-corpus is a script rather than a $(TEST_BINS) binary because it drives
# the real `3b` CLI over real package directories; it copies each into a temp
# directory first, so it never touches the working tree.
.PHONY: check test
check: $(TARGET) 3b-lsp 3b-lsp-test $(TEST_BINS)
	@failed=""; \
	for t in $(TEST_BINS); do \
	  echo "== $$t"; \
	  ./$$t || failed="$$failed $$t"; \
	done; \
	echo "== 3b-lsp"; \
	./3b-lsp-test ./3b-lsp || failed="$$failed 3b-lsp-test"; \
	echo "== format-corpus"; \
	./tools/format_corpus.sh ./$(TARGET) . || failed="$$failed format-corpus"; \
	echo "== backend-diff"; \
	./test/backend_diff.sh ./$(TARGET) examples || failed="$$failed backend-diff"; \
	echo "== os-parity"; \
	$(MAKE) --no-print-directory os-parity || failed="$$failed os-parity"; \
	echo "== check-doc"; \
	./tools/check_org_snippets.sh ./$(TARGET) learn-3b-in-y-minutes.org || failed="$$failed check-doc"; \
	if [ -n "$$failed" ]; then echo; echo "FAILED:$$failed"; exit 1; fi; \
	echo; echo "all suites passed ($(words $(TEST_BINS)) binaries + 3b-lsp + format-corpus + backend-diff + os-parity + check-doc)"

# `(import os)` resolves to two INDEPENDENT implementations -- the embedded C
# runtime for a compiled package (native_pkgs/os/os.3b), a table of VM host
# imports for `3b run` (bcosprims.c) -- which is exactly the shape that drifts
# silently, and did: the two once offered different verb sets and a
# differently-signed `getenv`. examples/os-portable exercises the whole module,
# and this target is the thing that actually notices, by requiring its stdout
# and its exit status to be IDENTICAL through both.
#
# Run from a scratch directory rather than the repo root because the example
# writes (and lists, and stats) real files in its working directory; running
# both backends from the same one is also what makes its directory-listing
# check mean anything. The build log `3b run <dir>` prints goes to stdout, so
# the native side is a `3b build` with its own output discarded, followed by
# executing the linked binary directly.
#
# The one non-obvious step: `3b run <script>` prints whatever `main` returned
# as a trailing line of ITS OWN (run_script_cmd in main.c), which is the VM
# driver talking, not the program. That line is the exact counterpart of the
# native binary's exit status, so it is moved into one -- giving a comparison
# that covers the return value instead of one that has to ignore it.
.PHONY: os-parity
os-parity: $(TARGET)
	@rm -rf .os-parity && mkdir -p .os-parity
	@here=`pwd`; cd .os-parity && \
	  "$$here/$(TARGET)" run "$$here/examples/os-portable/main.3b" > vm.raw 2>vm.err; \
	  status=$$?; \
	  if [ $$status -eq 0 ] && tail -n 1 vm.raw | grep -qx -- '-\{0,1\}[0-9][0-9]*'; then \
	    sed '$$d' vm.raw > vm.out; \
	    echo "exit status: $$(tail -n 1 vm.raw)" >> vm.out; \
	  else \
	    cp vm.raw vm.out; \
	    echo "exit status: $$status" >> vm.out; \
	  fi
	@./$(TARGET) build examples/os-portable > .os-parity/build.log 2>&1 || \
	  { echo "os-parity: the example failed to build natively"; cat .os-parity/build.log; exit 1; }
	@here=`pwd`; cd .os-parity && \
	  "$$here/examples/os-portable/os-portable" > native.out 2>native.err; \
	  echo "exit status: $$?" >> native.out
	@if diff -u .os-parity/vm.out .os-parity/native.out && diff -u .os-parity/vm.err .os-parity/native.err; then \
	  echo "os-parity: bytecode VM and native output are identical"; \
	else \
	  echo "os-parity: FAILED -- the os module behaves differently on the two backends"; \
	  echo "  left:  3b run examples/os-portable/main.3b   (bytecode VM)"; \
	  echo "  right: examples/os-portable/os-portable      (compiled and linked)"; \
	  exit 1; \
	fi

# `make test` would otherwise resolve to the test/ DIRECTORY and be considered
# up to date; .PHONY above is what makes this alias work at all.
test: check

# Always the HOST's own compiler, NOT $(CC) -- this runs during the build
# itself (to generate runtime_embed.h below), so cross-compiling 3b via
# CC=x86_64-w64-mingw32-gcc (see the `windows` target) must not touch it.
HOSTCC ?= gcc
tools/embed_runtime: tools/embed_runtime.c
	$(HOSTCC) -std=c11 -O2 -o $@ $<

runtime_embed.h: tools/embed_runtime $(RUNTIME_FILES)
	./tools/embed_runtime $(RUNTIME_FILES) > $@

codegen.o: runtime_embed.h

# `.3bs` modules known to the script.c driver (see that file's own
# top-of-file note) -- baked into the `3b` binary the same way runtime
# source is above, so `(import build)` inside a `.3bs` script works with
# no separate file to find/ship at runtime.
SCRIPT_MODULE_FILES = translate/build.3bs translate/config.3bs native_pkgs/rng/rng.3b native_pkgs/sort/sort.3b

script_embed.h: tools/embed_runtime $(SCRIPT_MODULE_FILES)
	./tools/embed_runtime $(SCRIPT_MODULE_FILES) > $@

script.o: script_embed.h

# Native packages baked into the `3b` binary the same way -- `(import os)`/
# `(import vm)` in a NATIVELY-compiled `.3b` program work with no `os/`/
# `vm/` directory needed in the project at all (compiler.c's compile_package
# falls back to these when a real directory doesn't resolve -- see its own
# comment). `vm`'s own script_native_abi.h -- the `-include`-able mirror of
# script_native.h a project needs to compile against native_script_* --
# rides along here too, materialized into a project's own output/ by
# build.c whenever `vm` is one of its packages (see build_invoke_toolchain).
NATIVE_PKG_EMBED_FILES = native_pkgs/os/os.3b native_pkgs/vm/vm.3b native_pkgs/vm/script_native_abi.h native_pkgs/rng/rng.3b native_pkgs/sort/sort.3b

native_pkgs_embed.h: tools/embed_runtime $(NATIVE_PKG_EMBED_FILES)
	./tools/embed_runtime $(NATIVE_PKG_EMBED_FILES) > $@

compiler.o: native_pkgs_embed.h
build.o: native_pkgs_embed.h

debug:
	valgrind --tool=memcheck --max-stackframe=8392944  --show-leak-kinds=definite,indirect --leak-check=full --show-leak-kinds=all --track-origins=yes -s ./$(TARGET)

# Rebuilds the COMPILER ITSELF (lexer/parser/checker/codegen/base.h, ...)
# with ASAN+UBSAN, into a separate `3b-sanitize` binary -- catches memory
# bugs in 3b's own execution (running `./3b-sanitize test`, say), as
# distinct from sanitizing a GENERATED program's runtime (lanes/arenas),
# which `make sanitize-examples` above covers instead. Always does a full
# clean rebuild first since plain object
# files aren't tagged by which flags built them -- `make` again
# afterwards needs its own clean rebuild back to an uninstrumented `3b`.
SANFLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all
.PHONY: sanitize
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(SANFLAGS)" LFLAGS="$(LFLAGS) $(SANFLAGS)" TARGET=3b-sanitize

# Drives the INSTRUMENTED compiler over every example, the way `3b` is
# actually used. `./3b-sanitize test` alone is not that: it runs the
# in-process demo suite, which never compiles a package off disk, so for a
# long time it passed green while `./3b-sanitize run examples/anything`
# aborted instantly on a `MemoryCopy(dst, NULL, 0)` in str8_list_join_ex --
# the first UB, on a code path every single run takes. `-fno-sanitize-recover`
# means the FIRST hit aborts, so one such bug hides everything behind it and
# the instrumented compiler is worth nothing until it is fixed.
#
# Both backends, since they share almost no code below the checker: `run` is
# bcgen.c + the VM, `build` is codegen.c emitting C. Not part of `check` for
# the same reason `sanitize-examples` isn't -- it needs the separate
# instrumented build `make sanitize` produces.
.PHONY: sanitize-compiler
sanitize-compiler: 3b-sanitize
	@failed=""; \
	for d in examples/*/; do \
	  n=`basename $$d`; \
	  case $$n in game|game3d|hotreload-demo) continue;; esac; \
	  for mode in run build; do \
	    if ASAN_OPTIONS=detect_leaks=0 ./3b-sanitize $$mode $$d > /tmp/3b-sanitize-$$n-$$mode.log 2>&1; then \
	      echo "ok   $$n ($$mode)"; \
	    else \
	      echo "FAIL $$n ($$mode)"; tail -n 20 /tmp/3b-sanitize-$$n-$$mode.log; failed="$$failed $$n/$$mode"; \
	    fi; \
	  done; \
	done; \
	if [ -n "$$failed" ]; then echo; echo "FAILED:$$failed"; exit 1; fi; \
	echo; echo "sanitize-compiler: the instrumented compiler is clean on every example, both backends"

# Cross-compiles the compiler for Windows via mingw-w64. WITHOUT_TRANSLATE=1
# because no Windows libclang is built here -- `3b translate` just isn't
# available in this binary (see main.c). No -lpthread/-ldl -- Windows uses
# its own native thread/sync primitives (see base.c/base.h's _WIN32
# branches) and has no dlopen family here to link against. Same "own clean
# first" requirement as `sanitize` -- switching between this and a plain
# `make`/`make sanitize` needs `make clean` in between, since object files
# aren't tagged by which target/platform built them. Verified (cross-
# compiled + run under Wine): the compiler itself, not yet `3b build`/`3b
# run`'s actual toolchain invocation on a real Windows machine (pkg-config,
# cc-flags, and cmd.exe-vs-/bin/sh script syntax all still assume a Unix
# toolchain on the OTHER end of that -- a separate, larger concern).
.PHONY: windows
windows:
	$(MAKE) clean
	$(MAKE) CC=x86_64-w64-mingw32-gcc WITHOUT_TRANSLATE=1 LFLAGS="-lm" TARGET=3b.exe

# Everything a release ships -- `3b`, liblib3b.a (build.c expects it beside
# the binary) and `3b-lsp` -- built with RELEASE=1, i.e. optimized and with
# the debug-only checks compiled out. This is the CI entry point: one target,
# no flag list to keep in sync at the call site.
#
# Same "own clean first" requirement as `sanitize` and `windows` above, and
# for the same reason: object files aren't tagged by which flags built them,
# so going back to a debug `make` afterwards needs its own `make clean`.
#
# Only the TOOLCHAIN. A 3b program is a release build when compiled with
# `3b build --release`, which is a property of that build, not of the `3b`
# binary that runs it -- a release compiler still defaults to debug output,
# exactly as an optimized gcc still defaults to -O0.
.PHONY: release
release:
	$(MAKE) clean
	$(MAKE) RELEASE=1 all lsp

# $(TEST_BINS) and friends are wildcard-derived (see `make check`), so this
# stays correct as tests are added -- the hand-written list it replaced had
# fallen 20 binaries behind.
clean:
	rm -rf $(OBJFILES) $(DEPFILES) $(TARGET) $(LIB_TARGET) $(LSP_OBJFILES) $(LSP_DEPFILES) 3b-lsp 3b-lsp-test 3b-sanitize 3b.exe *~ tools/embed_runtime runtime_embed.h script_embed.h native_pkgs_embed.h \
	  $(TEST_BINS) $(TEST_SOURCES:.c=.o) $(TEST_SOURCES:.c=.d) bcconfigprims.o bcconfigprims.d translate/rename.o translate/rename.d translate/emit.o translate/emit.d \
	  .os-parity examples/os-portable/os-portable

-include $(DEPFILES) $(LIB_DEPFILES) $(LSP_DEPFILES)
