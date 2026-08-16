#!/bin/sh
# Runs every runnable example package through BOTH backends and diffs what
# they produce -- `3b build <dir>` + the linked binary (the C toolchain, i.e.
# codegen.c) against `3b run <dir>/main.3b` (the bytecode VM, i.e. bcgen.c +
# bcvm.c). "The same source runs on either backend" is a stated design goal,
# and until this existed nothing in the tree actually checked it: every other
# suite exercises ONE backend, so a VM-only wrong answer stayed invisible
# until someone happened to run the same example both ways by hand. The first
# sweep this replaced found three real divergences (print argument evaluation
# order, string-to-i32's failure value, narrow-integer wraparound) in a single
# pass -- all three now fixed, all three regression-guarded from here.
#
# Compared, per example:
#   - the program's stdout, byte for byte
#   - the program's exit status
# stderr is NOT compared: the native path's gcc can emit warnings there that
# have no VM counterpart at all. It is printed on failure, since it's usually
# where the reason for one lives.
#
# usage: test/backend_diff.sh [3b-binary] [examples-dir]
set -u

BBB=${1:-./3b}
EXAMPLES=${2:-examples}

case $BBB in /*) ;; *) BBB=$(pwd)/$BBB ;; esac
case $EXAMPLES in /*) ;; *) EXAMPLES=$(pwd)/$EXAMPLES ;; esac

[ -x "$BBB" ] || { echo "backend-diff: no 3b binary at '$BBB' -- run \`make\` first" >&2; exit 1; }

# Every example that is NOT compared, each with the reason it can't be. This
# list is deliberately explicit and deliberately short: an example that simply
# doesn't appear here is COMPARED, so a newly added one can't slip through
# unchecked, and a skip can't quietly outlive the gap that justified it. Each
# entry names a real, separately-tracked limitation -- when it closes, the
# entry goes away and the example starts being compared with no other change.
#
# `game`/`game3d` aren't listed: they need SDL3/OpenGL dev packages and are
# excluded from every other automated path in this repo too (see the CI
# workflow), so they're filtered structurally below rather than skipped here.
skip_reason() {
  case $1 in
    hotreload-demo)
      echo "imports \`vm\`, a native-only package the VM driver can't import. Not run natively here either: it's an interactive demo that sleeps 3s for a human to edit fireball.3bs, and its post-reload check is PRINTED, not asserted -- unattended it exits 0 while reporting the reload it was demonstrating didn't happen. The reload machinery itself (including a \`.3bc\` reloaded by a separate process) is covered by test/script_test.c" ;;
    *)
      echo "" ;;
  esac
}

# The linked binary `3b build` produces: build.cfg.3b's `(binary "...")` if it
# has one, else the package directory's own basename (build.c's
# dir_default_name).
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
skipped=""
passed=0

for dir in "$EXAMPLES"/*/; do
  dir=${dir%/}
  name=$(basename "$dir")

  # Not skips -- these two have no automated path anywhere in this repo (they
  # need SDL3/OpenGL), so they're not part of the comparable set to begin with.
  case $name in game|game3d) continue ;; esac
  [ -f "$dir/main.3b" ] || continue

  reason=$(skip_reason "$name")
  if [ -n "$reason" ]; then
    printf 'skip %-20s %s\n' "$name" "$reason"
    skipped="$skipped $name"
    continue
  fi

  # Both backends run with the example directory as their WORKING directory,
  # the way CI's own example step does: an example that opens a relative path
  # (streams/main.3b writes and reads one back) must resolve it identically on
  # both sides, and whatever it leaves behind belongs next to the example
  # rather than in the repo root.

  # -- native: build, then execute the binary directly. Deliberately NOT
  # `3b run <dir>`, which would interleave the toolchain's own chatter (the
  # `package '<name>': compiled ...` line, the echoed gcc commands) into the
  # very stdout being diffed, and would need filtering to separate them again.
  # Building and executing as two steps means no filter ever runs over the
  # program's own output -- there is nothing to accidentally filter OUT.
  if ! (cd "$dir" && "$BBB" build .) > "$work/build.log" 2>&1; then
    echo "FAIL $name: native build failed"
    sed 's/^/    /' "$work/build.log"
    failed="$failed $name"
    continue
  fi
  bin=$(binary_name "$dir")
  (cd "$dir" && "./$bin") > "$work/native.out" 2> "$work/native.err"
  native_status=$?

  # -- VM: `3b run <file.3b>` compiles that file AND its same-package siblings
  # in the same directory (script.c's script_compile_unit, which is what lets a
  # multi-file example like `creatures` be compared at all) to bytecode, then
  # interprets it. main.c's run_script_cmd prints main's return value as a
  # trailing line of its own once the run finishes, so that last line is the
  # VM side's exit status and the lines above it are the program's stdout.
  # The process's OWN exit status is 0/1 for "did the VM get that far", which
  # is why a non-zero one is reported here as an outright VM failure rather
  # than compared against the native program's status.
  (cd "$dir" && "$BBB" run main.3b) > "$work/vm.raw" 2> "$work/vm.err"
  if [ $? -ne 0 ]; then
    echo "FAIL $name: the VM could not run it (native ran fine, status $native_status)"
    sed 's/^/    /' "$work/vm.err"
    tail -n 5 "$work/vm.raw" | sed 's/^/    /'
    failed="$failed $name"
    continue
  fi
  vm_status=$(tail -n 1 "$work/vm.raw")
  sed '$d' "$work/vm.raw" > "$work/vm.out"
  case $vm_status in
    ''|*[!0-9-]*)
      echo "FAIL $name: expected main's return value as the VM run's last line, got '$vm_status'"
      failed="$failed $name"
      continue ;;
  esac

  ok=1
  if ! diff -u "$work/native.out" "$work/vm.out" > "$work/diff.txt"; then
    echo "FAIL $name: the two backends printed different things"
    echo "     (--- native/C toolchain   +++ bytecode VM)"
    sed -e '1,2d' -e 's/^/    /' "$work/diff.txt"
    ok=0
  fi
  if [ "$native_status" != "$vm_status" ]; then
    echo "FAIL $name: exit status differs -- native $native_status, VM $vm_status"
    ok=0
  elif [ "$native_status" != 0 ]; then
    # Agreeing on a FAILURE is still a failure. Every example here asserts its
    # own feature area and returns non-zero when one of its own checks doesn't
    # hold (`(if ok 0 1)`), so a non-zero status means the example is telling
    # us something is broken -- on both backends at once, which a diff alone
    # would happily call a match.
    echo "FAIL $name: both backends agree, but the example itself failed (exit $native_status --"
    echo "     these examples assert their own feature area and return non-zero on a mismatch)"
    ok=0
  fi
  if [ $ok -eq 0 ]; then
    if [ -s "$work/native.err" ]; then
      echo "     native stderr:"
      sed 's/^/    /' "$work/native.err"
    fi
    if [ -s "$work/vm.err" ]; then
      echo "     VM stderr:"
      sed 's/^/    /' "$work/vm.err"
    fi
    failed="$failed $name"
    continue
  fi

  printf 'ok   %-20s (exit %s, %s line(s) of output)\n' "$name" "$native_status" "$(wc -l < "$work/native.out" | tr -d ' ')"
  passed=$((passed + 1))
done

echo
if [ -n "$failed" ]; then
  echo "backend-diff FAILED:$failed"
  echo "($passed example(s) agreed. Where the two disagree, ONE of them is wrong: the diff"
  echo " labels which output came from which, but not which one is correct.)"
  exit 1
fi
echo "backend-diff: $passed example(s) agree on both backends$([ -n "$skipped" ] && echo ", skipped:$skipped")"
