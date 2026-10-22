#!/bin/sh
# eval_sweep.sh CEREAL FILE.c...: the evaluator (src/c/eval.c) against gcc, per function.
#
# `cereal check --eval-sweep=N,SEED` lists the functions of FILE.c it can drive (integer,
# bool, float and double parameters, pointers to objects, records by value; a scalar,
# pointer or void result), calls each on N seeded inputs in the evaluator and prints the
# bytes of every call and its result: the return value and the final contents of every
# pointed-to object.  The same bytes run through a thunk per function (sweep_rt.h) in a gcc
# -O0 -fwrapv build of the file and must give the same result bytes.  For a scalar-only
# function an evaluator trap or step limit is a failure (the corpus is free of undefined
# behaviour); a construct the evaluator does not support is listed, not hidden.  Also runs
# the structural verifier (--verify-cfg), whose findings fail.  Fails on any mismatch or
# when fewer than half the functions were compared.  CEREAL_FLAGS adds flags to both (cereal and gcc)
# (e.g. -std=gnu23).
# EVAL_SWEEP_SOLVE='z3 -in -smt2' also solves for inputs (--eval-solve, docs/CONCOLIC_DESIGN.md):
# the same oracle, and solve.c must then reach every guarded edge; skipped with a note when the
# solver is not installed.
C=${1:?cereal binary}
shift
N=${EVAL_SWEEP_N:-24}
if [ -n "$EVAL_SWEEP_SOLVE" ] && ! command -v "${EVAL_SWEEP_SOLVE%% *}" >/dev/null 2>&1; then
    echo "eval_sweep: solver '${EVAL_SWEEP_SOLVE%% *}' not found, running without it"
    EVAL_SWEEP_SOLVE=
fi
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d) || exit 1
trap 'rm -rf "$W"' EXIT
bad=0
for f in "$@"; do
    rm -f "$W"/*
    : >"$W/exp.txt.unsup"
    abs=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
    # shellcheck disable=SC2086
    if "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS --verify-cfg "$f" 2>&1 </dev/null | grep -q 'verify-cfg:'; then
        echo "$f: verifier findings"
        bad=$((bad + 1))
    fi
    if [ -n "$EVAL_SWEEP_SOLVE" ]; then
        # shellcheck disable=SC2086
        "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS --eval-sweep="$N,1234" --eval-solve="$EVAL_SWEEP_SOLVE" "$f" 2>"$W/solve.err" </dev/null \
            | grep -E '^(fn|thunk|call) ' >"$W/sweep.txt"
        grep -E '^cereal: ' "$W/solve.err" | sed "s|^|$f: |"
    else
        # shellcheck disable=SC2086
        "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS --eval-sweep="$N,1234" "$f" 2>&1 </dev/null \
            | grep -E '^(fn|thunk|call) ' >"$W/sweep.txt"
    fi
    # the driver (the thunks), the calls, and the evaluator's results, in order
    awk -v drv="$W/drv.c" -v calls="$W/calls.txt" -v expf="$W/exp.txt" -v src="$abs" -v rt="$D/sweep_rt.h" '
        $1 == "fn" { nfn++; sig[$2] = 1; next }
        $1 == "thunk" {
            if (!started++) {
                print "#define main sweep_orig_main_" > drv
                print "#include \"" src "\"" > drv
                print "#undef main" > drv
                print "#include \"" rt "\"" > drv
            }
            print substr($0, length($2) + length($3) + 9) > drv
            names[++nth] = $2
            next
        }
        $1 == "call" {
            name = $2
            if ($4 == "=>") {
                print name " " $3 > calls
                print $5 > expf
                ncalls[name]++
                line[++n] = name " " substr($3, 1, 64)
            } else if ($4 == "unsupported") {
                unsup[name] = $4 " " $5
            } else {
                print "TRAP " name " " substr($3, 1, 64) " " $4 > "/dev/stderr"
                trapped[name] = 1
            }
            next
        }
        END {
            if (started) {
                print "static const SwTh sw_tab[] = {" > drv
                for (t = 1; t <= nth; t++) printf "{\"%s\", t_%s},\n", names[t], names[t] > drv
                print "};\nint main(int argc, char **argv)\n{\n    return sw_main(sw_tab, sizeof sw_tab / sizeof *sw_tab, argv[1]);\n}" > drv
            }
            for (l = 1; l <= n; l++) print line[l] > (expf ".calls")
            nf = 0; ok = 0
            for (s in sig) { nf++; if (ncalls[s] && !(s in trapped)) ok++ }
            printf "%d %d\n", nf, ok > (expf ".count")
            for (s in unsup) printf "%s (%s)\n", s, unsup[s] > (expf ".unsup")
        }' "$W/sweep.txt" 2>"$W/trap.txt"
    [ -s "$W/trap.txt" ] && { sed "s|^|$f: |" "$W/trap.txt"; bad=$((bad + 1)); }
    read -r nf ok <"$W/exp.txt.count" 2>/dev/null || { nf=0; ok=0; }
    unsup=$(tr '\n' ',' <"$W/exp.txt.unsup")
    mism=0
    if [ -s "$W/calls.txt" ]; then
        # shellcheck disable=SC2086
        if ! gcc -O0 -fwrapv -w $CEREAL_FLAGS -I "$(dirname "$abs")" -o "$W/drv" "$W/drv.c" -lm 2>"$W/gcc.err"; then
            echo "$f: gcc failed"; head -5 "$W/gcc.err"
            bad=$((bad + 1)); continue
        fi
        "$W/drv" "$W/calls.txt" </dev/null >"$W/got.txt" 2>&1 || { echo "$f: the gcc build ran badly"; bad=$((bad + 1)); continue; }
        paste -d'\t' "$W/exp.txt.calls" "$W/exp.txt" "$W/got.txt" >"$W/all.txt"
        mism=$(awk -F'\t' -v f="$f" '$2 != $3 {
            c++
            if (c <= 5) {
                k = 1
                while (k <= length($2) && substr($2, k, 1) == substr($3, k, 1)) k++
                w = k > 24 ? k - 24 : 1
                printf "%s: %s: evaluator ...%s, gcc ...%s (hex digit %d of %d)\n", f, $1, substr($2, w, 48), substr($3, w, 48), k, length($2) > "/dev/stderr"
            }
        } END { print c + 0 }' "$W/all.txt" 2>"$W/mm.txt")
        cat "$W/mm.txt"
        if [ -n "$EVAL_SWEEP_SOLVE" ] && [ "$(basename "$f")" = solve.c ]; then
            # every guarded function must have a call that took its guarded edge (result 1)
            miss=$(awk -F'\t' '{ split($1, a, " "); if (substr($2, 1, 8) == "01000000") hit[a[1]] = 1 }
                END { split("magic square sumsw neighbour mixed", w, " ");
                      for (i = 1; i in w; i++) if (!(w[i] in hit)) printf "%s ", w[i] }' "$W/all.txt")
            if [ -n "$miss" ]; then
                echo "$f: the solver did not reach the guarded edge of: $miss"
                bad=$((bad + 1))
            fi
        fi
    fi
    echo "$f: $ok of $nf functions compared${unsup:+, unsupported: ${unsup%,}}"
    bad=$((bad + mism))
    if [ $((ok * 2)) -lt "$nf" ]; then
        echo "$f: fewer than half the functions were compared"
        bad=$((bad + 1))
    fi
done
[ "$bad" -eq 0 ]
