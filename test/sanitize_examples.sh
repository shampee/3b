#!/bin/sh
# Builds and runs every example with the GENERATED program under sanitizers --
# ASAN+UBSAN for all of them, TSAN additionally for any that use `parallel`.
#
# This covers the half of the tree `make sanitize` cannot reach. That target
# instruments the COMPILER (lexer/parser/checker/codegen), catching bugs in
# 3b's own execution; nothing there says anything about the C that codegen.c
# EMITS, or about the runtime prelude it emits alongside (arenas, dynamic
# arrays, the lane fork-join pool). Those only run when a generated program
# runs, which is what happens here.
#
# The flags reach gcc through `BBB_EXTRA_CFLAGS`, which build.c splices into
# every compile line AND the link line (see its own comment there).
#
# usage: test/sanitize_examples.sh [3b-binary] [examples-dir]
set -u

BBB=${1:-./3b}
EXAMPLES=${2:-examples}

case $BBB in /*) ;; *) BBB=$(pwd)/$BBB ;; esac
case $EXAMPLES in /*) ;; *) EXAMPLES=$(pwd)/$EXAMPLES ;; esac

[ -x "$BBB" ] || { echo "sanitize-examples: no 3b binary at '$BBB' -- run \`make\` first" >&2; exit 1; }

ASAN_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
TSAN_FLAGS="-fsanitize=thread"

# Generated programs allocate their arenas at startup and hand them back to the
# OS at exit rather than freeing them, so LeakSanitizer reports the arena
# headers (~80 bytes, from bbb_arena_create_vm) on every single example. That is
# the arena pattern working as designed, not a leak, and leaving detection on
# would fail all of them. Everything else ASAN checks stays on.
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1
export TSAN_OPTIONS=halt_on_error=1

# `game`/`game3d` need SDL3/OpenGL and are outside every automated path here.
# `hotreload-demo` is an interactive demo that sleeps for a human edit -- see
# test/backend_diff.sh's skip list, which carries the full reason.
skip_reason() {
  case $1 in
    hotreload-demo)
      echo "interactive -- sleeps 3s waiting for a human to edit fireball.3bs (see test/backend_diff.sh)" ;;
    *)
      echo "" ;;
  esac
}

binary_name() {
  dir=$1
  if [ -f "$dir/build.cfg.3b" ]; then
    name=$(sed -n 's/^[[:space:]]*(binary[[:space:]]*"\([^"]*\)").*/\1/p' "$dir/build.cfg.3b" | head -n 1)
    [ -n "$name" ] && { echo "$name"; return; }
  fi
  basename "$dir"
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

failed=""
passed=0

# One build+run under one set of flags. A sanitizer reports on STDERR while the
# process can still exit 0 (UBSAN without -fno-sanitize-recover keeps going), so
# a non-empty stderr is a failure here just as much as a non-zero status is.
run_under() {
  dir=$1; name=$2; label=$3; flags=$4

  rm -rf "$dir/output"
  if ! (cd "$dir" && BBB_EXTRA_CFLAGS="$flags" "$BBB" build .) > "$work/build.log" 2>&1; then
    echo "FAIL $name [$label]: build failed"
    sed 's/^/    /' "$work/build.log"
    return 1
  fi

  bin=$(binary_name "$dir")
  (cd "$dir" && "./$bin") > "$work/run.out" 2> "$work/run.err"
  status=$?

  if [ $status -ne 0 ] || [ -s "$work/run.err" ]; then
    echo "FAIL $name [$label]: exit $status$([ -s "$work/run.err" ] && echo ', diagnostics on stderr')"
    sed 's/^/    /' "$work/run.err"
    return 1
  fi
  return 0
}

for dir in "$EXAMPLES"/*/; do
  dir=${dir%/}
  name=$(basename "$dir")

  case $name in game|game3d) continue ;; esac
  [ -f "$dir/main.3b" ] || continue

  reason=$(skip_reason "$name")
  if [ -n "$reason" ]; then
    printf 'skip %-20s %s\n' "$name" "$reason"
    continue
  fi

  ok=1
  run_under "$dir" "$name" "ASAN+UBSAN" "$ASAN_FLAGS" || ok=0

  # TSAN only where there are actually threads to race: `parallel`/`parallel-for`
  # is the only construct that starts any. Detected by grep rather than a list,
  # so a new example that uses lanes is covered the day it lands.
  if grep -qE '\(parallel(-for)?[[:space:]]' "$dir"/*.3b 2>/dev/null; then
    run_under "$dir" "$name" "TSAN" "$TSAN_FLAGS" || ok=0
    label="ASAN+UBSAN, TSAN"
  else
    label="ASAN+UBSAN"
  fi

  if [ $ok -eq 0 ]; then
    failed="$failed $name"
    continue
  fi
  printf 'ok   %-20s (%s)\n' "$name" "$label"
  passed=$((passed + 1))
done

echo
if [ -n "$failed" ]; then
  echo "sanitize-examples FAILED:$failed"
  exit 1
fi
echo "sanitize-examples: $passed example(s) clean under the sanitizers"
