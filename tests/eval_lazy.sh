#!/bin/sh
# eval_lazy.sh CEREAL: the sweep reaches what the old fixed layout's budget lost
# (docs/LAZY_BLOBS_DESIGN.md).  tests/cfg_lazy/lazy_downcast.c is the shape of Lua's luaV_execute: a
# pointer cast to the larger struct it starts, and a pointer inside it that decides the result.  The
# sweep must print calls with the five results of lz_run past the first pointer (the first four result bytes, hex; -1, the call's NULL path, may or may not show and is left out).
C=${1:?cereal binary}
D=$(cd "$(dirname "$0")" && pwd)
got=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 "$D/cfg_lazy/lazy_downcast.c" 2>/dev/null </dev/null |
    awk '$1 == "call" && $2 == "lz_run" && $4 == "=>" { print substr($5, 1, 8) }' | grep -v ffffffff | sort -u | tr '\n' ' ')
want="01000000 02000000 03000000 fdffffff feffffff "
if [ "$got" != "$want" ]; then
    echo "eval_lazy: lz_run results: got '$got', want '$want'"
    exit 1
fi
echo "eval_lazy: ok"
# With the solver, lazy calls are solved too: lg_run's guard needs k[0] == 331, behind a pointer.
if command -v z3 >/dev/null 2>&1; then
    got=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 --eval-solve='z3 -in -smt2' "$D/cfg_lazy/lazy_guard.c" 2>/dev/null </dev/null |
        awk '$1 == "call" && $2 == "lg_run" && $4 == "=>" { print substr($5, 1, 8) }' | sort -u | tr '\n' ' ')
    case "$got" in
    *09000000*) ;;
    *) echo "eval_lazy: lg_run with the solver: got '$got', want 09000000 among them"; exit 1 ;;
    esac
    echo "eval_lazy: solver ok"
fi
# Fixtures where the sweep must stop instead of differing from gcc: lazy_ptrbyte.c reads a stored pointer back through a
# char; lazy_ptrcopy.c copies part of one with memcpy; lazy_constwrite.c writes a static const object; lazy_overwrite.c sets a whole pointer slot as an integer (allowed).
for f in lazy_ptrbyte lazy_ptrcopy lazy_constwrite lazy_overwrite; do
    if ! sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/$f.c" >/dev/null 2>&1; then
        echo "eval_lazy: $f.c differs from gcc"
        exit 1
    fi
done
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 "$D/cfg_lazy/lazy_overwrite.c" 2>/dev/null </dev/null | grep -ac '^call lo_run .* =>')
if [ "$n" -lt 40 ]; then
    echo "eval_lazy: lazy_overwrite.c: the sweep ran too few calls of lo_run (the whole-slot store stopped them)"
    exit 1
fi
echo "eval_lazy: stop fixtures ok"
# Two pointer parameters subtracted: the retry makes the later one share the earlier, so every call finishes (11 of 24 without it).
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_paralias.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_paralias.c differs from gcc"; exit 1; }
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 "$D/cfg_lazy/lazy_paralias.c" 2>/dev/null </dev/null | grep -ac '^call pa_run .* =>')
if [ "$n" -lt 24 ]; then
    echo "eval_lazy: lazy_paralias.c: only $n of 24 calls of pa_run finished (the parameter alias retry)"
    exit 1
fi
echo "eval_lazy: parameter alias ok"
# A cell drawn to share its own object (a pointer read as an int): the retry redraws the cell as a new blob (958 of 1000 finish without it).
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_selfptr.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_selfptr.c differs from gcc"; exit 1; }
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=1000,1234 "$D/cfg_lazy/lazy_selfptr.c" 2>/dev/null </dev/null | grep -ac '^call sp_run .* =>')
if [ "$n" -lt 965 ]; then
    echo "eval_lazy: lazy_selfptr.c: only $n of 1000 calls of sp_run finished (the pointer-read retry)"
    exit 1
fi
echo "eval_lazy: pointer-read retry ok"
# Bytes read by memcpy are zero unless the solver sets them: with it, mc_eq's unequal edge and mc_const's equal edge are reached
# (without it the first always returns 1 and the second 0), and gcc agrees on the bytes it set.
if command -v z3 >/dev/null 2>&1; then
    sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_memcpy.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_memcpy.c differs from gcc"; exit 1; }
    out=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 --eval-solve='z3 -in -smt2' "$D/cfg_lazy/lazy_memcpy.c" 2>/dev/null </dev/null)
    for w in "mc_eq 00000000" "mc_const 01000000" "mc_init 01000000"; do
        set -- $w
        echo "$out" | awk -v f="$1" -v r="$2" '$1 == "call" && $2 == f && $4 == "=>" && substr($5, 1, 8) == r { found = 1 } END { exit !found }' ||
            { echo "eval_lazy: $1 with the solver never gave $2 (untyped reads of blob bytes are not symbolic)"; exit 1; }
    done
    echo "eval_lazy: untyped read ok"
fi
