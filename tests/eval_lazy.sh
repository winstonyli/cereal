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
# A walk over six list elements, each subtracted from one field of the head: one hint for the pointee size makes them all share
# (hints per cell stop at the fifth element: 17 of 200 calls finish without a hint for the size).
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_list.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_list.c differs from gcc"; exit 1; }
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=200,1234 "$D/cfg_lazy/lazy_list.c" 2>/dev/null </dev/null | grep -ac '^call ll_run .* =>')
if [ "$n" -lt 180 ]; then
    echo "eval_lazy: lazy_list.c: only $n of 200 calls of ll_run finished (the size hint)"
    exit 1
fi
echo "eval_lazy: size hint ok (list)"
# Ten fields of one pointee size subtracted from an eleventh: one hint for the size makes them all share (a hint per cell or per offset
# stops at the ninth).  Calls whose one NULL lands on a pointer cell stop for good, so the count stays under 200.
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_size.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_size.c differs from gcc"; exit 1; }
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=200,1234 "$D/cfg_lazy/lazy_size.c" 2>/dev/null </dev/null | grep -ac '^call sz_run .* =>')
if [ "$n" -lt 155 ]; then
    echo "eval_lazy: lazy_size.c: only $n of 200 calls of sz_run finished (the size hint)"
    exit 1
fi
echo "eval_lazy: size hint ok"
# A cell drawn to share its own object (a pointer read as an int): the retry redraws the cell as a new blob (958 of 1000 finish without it).
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_selfptr.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_selfptr.c differs from gcc"; exit 1; }
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=1000,1234 "$D/cfg_lazy/lazy_selfptr.c" 2>/dev/null </dev/null | grep -ac '^call sp_run .* =>')
if [ "$n" -lt 965 ]; then
    echo "eval_lazy: lazy_selfptr.c: only $n of 1000 calls of sp_run finished (the pointer-read retry)"
    exit 1
fi
echo "eval_lazy: pointer-read retry ok"
# A computed goto over drawn bytes (the dispatch of Lua's luaV_execute): the sweep follows it, so every call finishes, and gcc agrees.
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_igoto.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_igoto.c differs from gcc"; exit 1; }
n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 "$D/cfg_lazy/lazy_igoto.c" 2>/dev/null </dev/null | grep -ac '^call ig_run .* =>')
if [ "$n" -lt 24 ]; then
    echo "eval_lazy: lazy_igoto.c: only $n of 24 calls of ig_run finished (the computed goto)"
    exit 1
fi
echo "eval_lazy: computed goto ok"
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
    # Pointer offsets are the solver's too (D11): ps_win needs two pointers 4 to 7 bytes apart, ps_cell two cells 3 bytes apart;
    # the plain draws are whole 8-byte steps, so only a solver round gives 1.  gcc must agree on the offsets it set.
    EVAL_SWEEP_SOLVE='z3 -in -smt2' sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_ptrsolve.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_ptrsolve.c differs from gcc"; exit 1; }
    out=$("$C" check --target=x86_64-linux-gnu --eval-sweep=200,1234 --eval-solve='z3 -in -smt2' "$D/cfg_lazy/lazy_ptrsolve.c" 2>/dev/null </dev/null)
    for f in ps_win ps_cell; do
        echo "$out" | awk -v f="$f" '$1 == "call" && $2 == f && $4 == "=>" && substr($5, 1, 8) == "01000000" { found = 1 } END { exit !found }' ||
            { echo "eval_lazy: $f with the solver never gave 1 (pointer offsets are not the solver's)"; exit 1; }
    done
    echo "eval_lazy: pointer offset ok"
    # Whether a pointer cell is NULL is the solver's too (D11a): behind a guard that only a solved call passes, ns_null needs
    # NULL (1) and ns_deep needs NULL (1) and then a second cell that is NULL (4); gcc must agree on those calls.
    EVAL_SWEEP_SOLVE='z3 -in -smt2' sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_nullsolve.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_nullsolve.c differs from gcc"; exit 1; }
    out=$("$C" check --target=x86_64-linux-gnu --eval-sweep=200,1234 --eval-solve='z3 -in -smt2' "$D/cfg_lazy/lazy_nullsolve.c" 2>/dev/null </dev/null)
    for w in "ns_null 01000000" "ns_deep 01000000" "ns_deep 04000000"; do
        set -- $w
        echo "$out" | awk -v f="$1" -v r="$2" '$1 == "call" && $2 == f && $4 == "=>" && substr($5, 1, 8) == r { found = 1 } END { exit !found }' ||
            { echo "eval_lazy: $1 with the solver never gave $2 (whether a pointer is NULL is not the solver's)"; exit 1; }
    done
    echo "eval_lazy: pointer null ok"
fi
# A function that frees what the sweep drew (a lazy blob is never freed, on either side): every call finishes.
for fn in lf_run lf_twice lf_own; do
    n=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 "$D/cfg_lazy/lazy_free.c" 2>/dev/null </dev/null | grep -ac "^call $fn .* =>")
    if [ "$n" -lt 24 ]; then
        echo "eval_lazy: lazy_free.c: only $n of 24 calls of $fn finished (free of a lazy blob)"
        exit 1
    fi
done
echo "eval_lazy: lazy free ok"
sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_free.c" >/dev/null 2>&1 || { echo "eval_lazy: lazy_free.c differs from gcc"; exit 1; }
echo "eval_lazy: lazy free agrees with gcc"
