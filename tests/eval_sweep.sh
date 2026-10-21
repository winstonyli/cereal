#!/bin/sh
# eval_sweep.sh CEREAL FILE.c...: the evaluator (src/c/eval.c) against gcc, per function.
#
# `cereal check --eval-sweep=N,SEED` lists every scalar function of FILE.c (integer,
# bool, float and double parameters and result), calls it on N seeded argument vectors in
# the evaluator and prints the results.  The same calls, in the same order, run in a gcc
# -O0 -fwrapv build of the file and must print the same values.  An evaluator trap or step
# limit is a failure (the corpus is free of undefined behaviour); a construct the evaluator
# does not support is listed, not hidden.  Also runs the structural verifier (--verify-cfg),
# whose findings fail.  Fails on any mismatch or when fewer than half the functions
# were compared.  CEREAL_FLAGS adds cereal flags (e.g. -std=gnu23).
C=${1:?cereal binary}
shift
N=${EVAL_SWEEP_N:-24}
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
    # shellcheck disable=SC2086
    "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS --eval-sweep="$N,1234" "$f" 2>&1 </dev/null \
        | grep -E '^(fn|call) ' >"$W/sweep.txt"
    # the driver (calls with a result) and the evaluator's results, in order
    awk -v drv="$W/drv.c" -v expf="$W/exp.txt" -v src="$abs" '
        $1 == "fn" { nfn++; sig[$2] = $3; next }
        $1 == "call" {
            name = $2; args = $3
            r = substr($0, index($0, $3) + length($3) + 1)
            if (r ~ /^[if]:/) {
                tag = substr(r, 1, 1)
                if (!started++) {
                    print "#define main sweep_orig_main_" > drv
                    print "#include \"" src "\"" > drv
                    print "#undef main" > drv
                    print "#include <stdio.h>" > drv
                    print "int main(void)\n{" > drv
                }
                if (tag == "i") printf "    printf(\"%%lld\\n\", (long long)%s%s);\n", name, args > drv
                else printf "    printf(\"%%a\\n\", (double)%s%s);\n", name, args > drv
                print substr(r, 3) > expf
                ncalls[name]++
                line[++n] = name " " args
            } else if (r ~ /^unsupported/) {
                unsup[name] = r
            } else {
                print "TRAP " name args " " r > "/dev/stderr"
                trapped[name] = 1
            }
            next
        }
        END {
            if (started) print "    return 0;\n}" > drv
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
    if [ -s "$W/drv.c" ]; then
        # shellcheck disable=SC2086
        if ! gcc -O0 -fwrapv -w -I "$(dirname "$abs")" -o "$W/drv" "$W/drv.c" -lm 2>"$W/gcc.err"; then
            echo "$f: gcc failed"; head -5 "$W/gcc.err"
            bad=$((bad + 1)); continue
        fi
        "$W/drv" </dev/null >"$W/got.txt" 2>&1 || { echo "$f: the gcc build ran badly"; bad=$((bad + 1)); continue; }
        paste -d'\t' "$W/exp.txt.calls" "$W/exp.txt" "$W/got.txt" >"$W/all.txt"
        mism=$(awk -F'\t' -v f="$f" '$2 != $3 { c++; if (c <= 5) print f ": " $1 ": evaluator " $2 ", gcc " $3 > "/dev/stderr" } END { print c + 0 }' "$W/all.txt" 2>"$W/mm.txt")
        cat "$W/mm.txt"
    fi
    echo "$f: $ok of $nf functions compared${unsup:+, unsupported: ${unsup%,}}"
    bad=$((bad + mism))
    if [ $((ok * 2)) -lt "$nf" ]; then
        echo "$f: fewer than half the functions were compared"
        bad=$((bad + 1))
    fi
done
[ "$bad" -eq 0 ]
