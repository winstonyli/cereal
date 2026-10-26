#!/bin/sh
# eval_sweep.sh CEREAL FILE.c...: the evaluator (src/c/eval.c) against gcc, per function.
#
# `cereal check --eval-sweep=N,SEED` lists the functions of FILE.c it can drive (integer,
# bool, float and double parameters, pointers to objects, records by value; a scalar,
# pointer or void result), calls each on N seeded inputs in the evaluator and prints the
# graph of objects each call made (docs/LAZY_BLOBS_DESIGN.md) and its result: the return value and
# the final contents of every object.  The same bytes run through a thunk per function (sweep_rt.h) in a gcc
# -O0 -fwrapv build of the file and must give the same result bytes.  For a scalar-only
# function an evaluator trap or step limit is a failure (the corpus is free of undefined
# behaviour); a construct the evaluator does not support is listed, not hidden.  Also runs
# the structural verifier (--verify-cfg), whose findings fail.  Fails on any mismatch or
# when fewer than half the functions were compared.  CEREAL_FLAGS adds flags to both (cereal and gcc)
# (e.g. -std=gnu23).
# EVAL_SWEEP_LINK=1 sweeps all the FILEs as one program (--eval-link, docs/MULTITU_DESIGN.md): a
# call into another file runs its body; gcc compiles each file with its thunks and links the objects
# with one driver.  A file's `main` is renamed, not run.
# EVAL_SWEEP_STATE=1 adds --eval-state: functions taking a built state (a constructor's result) are
# swept too; the gcc side calls the constructor afresh per call and compares call results only.
# EVAL_SWEEP_LIB=1 is for library code (bench/tools/lib_oracle.sh): each gcc call runs in a forked child, so
# a crash or a hang there is "!" for that call; evaluator stops and gcc crashes are counted, not failures;
# one summary line per run, and a failure only for a build problem.
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
W=$(mktemp -d ${EVAL_SWEEP_KEEP:+-p "$HOME"}) || exit 1
trap '[ -n "$EVAL_SWEEP_KEEP" ] && echo "eval_sweep: kept $W" || rm -rf "$W"' EXIT   # EVAL_SWEEP_KEEP=1 keeps the work directory (debugging)
bad=0
absof() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }

# One run over the files $@ (more than one only when linking); failures add to bad.
run_group() {
    rm -f "$W"/*
    : >"$W/exp.txt.unsup"
    f=$1
    link=
    state=
    [ -n "$EVAL_SWEEP_STATE" ] && state=--eval-state
    absl=$(absof "$1")
    gccx=
    [ -n "$EVAL_SWEEP_LIB" ] && gccx=-DSW_FORK
    if [ -n "$EVAL_SWEEP_LINK" ]; then
        link=--eval-link
        f="link of $# files from $1"
        absl=
        for g in "$@"; do absl="$absl $(absof "$g")"; done
    fi
    for g in "$@"; do
        # shellcheck disable=SC2086
        if "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS --verify-cfg "$g" 2>&1 </dev/null | grep -q 'verify-cfg:'; then
            echo "$g: verifier findings"
            bad=$((bad + 1))
        fi
    done
    if [ -n "$EVAL_SWEEP_SOLVE" ]; then
        # shellcheck disable=SC2086
        "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS $link $state --eval-sweep="$N,1234" --eval-solve="$EVAL_SWEEP_SOLVE" $absl 2>"$W/solve.err" </dev/null \
            | grep -E '^(file|fp|fn|thunk|call) ' >"$W/sweep.txt"
        grep -E '^cereal: ' "$W/solve.err" | sed "s|^|$f: |"
    else
        # shellcheck disable=SC2086
        "$C" check --target=x86_64-linux-gnu $CEREAL_FLAGS $link $state --eval-sweep="$N,1234" $absl 2>"$W/sweep.err" </dev/null \
            | grep -E '^(file|fp|fn|thunk|call) ' >"$W/sweep.txt"
    fi
    # The drivers (one per file: the file, its thunks, a table of them), the calls, and the evaluator's
    # results, in order.  A function is known by file number and name (statics may share names).
    awk -v lib="$EVAL_SWEEP_LIB" -v W="$W" -v calls="$W/calls.txt" -v expf="$W/exp.txt" -v src1="$(absof "$1")" -v rt="$D/sweep_rt.h" '
        function start(fi,    drv) {
            drv = W "/drv" fi ".c"
            if (!started[fi]++) {
                print "#define main sweep_orig_main_" fi > drv
                print "#include \"" src[fi] "\"" > drv
                print "#undef main" > drv
                print "#include \"" rt "\"" > drv
                if (fi > nfiles) nfiles = fi
            }
            return drv
        }
        BEGIN { fi = 1; src[1] = src1 }
        $1 == "file" { if (!($2 in idx)) idx[$2] = ++nfile; fi = idx[$2]; src[fi] = $2; next }
        $1 == "fn" { nfn++; sig[fi ":" $2] = 1; next }
        $1 == "fp" { start(fi); fpg[fi, ++nfp[fi]] = $2; fpn[fi, nfp[fi]] = $3; next }
        $1 == "thunk" {
            drv = start(fi)
            print substr($0, length($2) + length($3) + 9) > drv
            names[fi, ++nth[fi]] = $2
            next
        }
        $1 == "call" {
            name = fi ":" $2
            if ($4 == "=>") {
                print name " " $3 > calls
                print $5 > expf
                ncalls[name]++
                line[++n] = name " " substr($3, 1, 64)
            } else if ($4 == "unsupported") {
                unsup[name] = $4 " " $5
            } else {
                if (!lib) print "TRAP " name " " substr($3, 1, 64) " " $4 > "/dev/stderr"
                trapped[name] = 1
                ntrap++
            }
            next
        }
        END {
            mn = W "/drvmain.c"
            print "#include \"" rt "\"" > mn
            print "long time(long *t) { if (t) *t = 0; return 0; }\nlong clock(void) { return 0; }" > mn
            for (i = 1; i <= nfiles; i++) {
                if (!started[i]) continue
                print i, src[i] > (W "/srcs.txt")
                drv = W "/drv" i ".c"
                print "static const SwTh sw_tab[] = {" > drv
                for (t = 1; t <= nth[i]; t++) printf "{\"%d:%s\", t_%s},\n", i, names[i, t], names[i, t] > drv
                printf "};\nconst SwTh *sw_tab_%d(int *n)\n{\n    *n = sizeof sw_tab / sizeof *sw_tab;\n    return sw_tab;\n}\n", i > drv
                printf "const SwTh *sw_tab_%d(int *);\n", i > mn
                printf "void sw_fp_%d(void **t)\n{\n", i > drv
                for (t = 1; t <= nfp[i]; t++) printf "    t[%d] = (void *)%s;\n", fpg[i, t], fpn[i, t] > drv
                printf "}\n" > drv
                printf "void sw_fp_%d(void **);\n", i > mn
            }
            print "int main(int argc, char **argv)\n{\n    static SwTh all[1 << 16];\n    int n = 0, k, j;\n    const SwTh *t;" > mn
            for (i = 1; i <= nfiles; i++)
                if (started[i])
                    printf "    t = sw_tab_%d(&k);\n    for (j = 0; j < k; j++)\n        all[n++] = t[j];\n    sw_fp_%d(sw_fn);\n", i, i > mn
            print "    return sw_main(all, n, argv[1]);\n}" > mn
            for (l = 1; l <= n; l++) print line[l] > (expf ".calls")
            nf = 0; ok = 0
            for (s in sig) { nf++; if (ncalls[s] && !(s in trapped)) ok++ }
            printf "%d %d %d\n", nf, ok, nfiles > (expf ".count")
            print ntrap + 0 > (expf ".ntrap")
            for (s in unsup) printf "%s (%s)\n", s, unsup[s] > (expf ".unsup")
        }' "$W/sweep.txt" 2>"$W/trap.txt"
    [ -s "$W/trap.txt" ] && { sed "s|^|$f: |" "$W/trap.txt"; bad=$((bad + 1)); }
    read -r nf ok nfiles <"$W/exp.txt.count" 2>/dev/null || { nf=0; ok=0; nfiles=0; }
    unsup=$(tr '\n' ',' <"$W/exp.txt.unsup")
    mism=0
    if [ -s "$W/calls.txt" ]; then
        # each file's driver is compiled with that file's directory on the include path
        objs=
        fail=
        while read -r i g; do
            # shellcheck disable=SC2086
            gcc -O0 -fwrapv -w $CEREAL_FLAGS $gccx -I "$(dirname "$g")" -c -o "$W/drv$i.o" "$W/drv$i.c" 2>>"$W/gcc.err" || fail=1
            objs="$objs $W/drv$i.o"
        done <"$W/srcs.txt"
        # the files without a swept function are linked as they are (their main, if any, renamed)
        i=0
        for g in $absl; do
            i=$((i + 1))
            grep -q " $g\$" "$W/srcs.txt" && continue
            # shellcheck disable=SC2086
            gcc -O0 -fwrapv -w $CEREAL_FLAGS -Dmain=sweep_orig_main_x$i -c -o "$W/plain$i.o" "$g" 2>>"$W/gcc.err" || fail=1
            objs="$objs $W/plain$i.o"
        done
        # shellcheck disable=SC2086
        gcc -O0 -fwrapv -w $gccx -c -o "$W/drvmain.o" "$W/drvmain.c" 2>>"$W/gcc.err" || fail=1
        # shellcheck disable=SC2086
        [ -z "$fail" ] && ! gcc -o "$W/drv" $objs "$W/drvmain.o" -lm 2>>"$W/gcc.err" && fail=1
        if [ -n "$fail" ]; then
            echo "$f: gcc failed"; head -5 "$W/gcc.err"
            bad=$((bad + 1)); return
        fi
        if [ -n "$EVAL_SWEEP_LIB" ]; then "$W/drv" "$W/calls.txt" </dev/null >"$W/got.txt" 2>/dev/null; else "$W/drv" "$W/calls.txt" </dev/null >"$W/got.txt" 2>&1; fi
        drc=$?
        if [ "$drc" -ne 0 ] && [ -z "$EVAL_SWEEP_LIB" ]; then
            # the call after the last result line is the one that crashed or was missing from the table
            echo "$f: the gcc build ran badly (exit $drc, after $(wc -l <"$W/got.txt") of $(wc -l <"$W/calls.txt") calls; next: $(sed -n "$(($(wc -l <"$W/got.txt") + 1))p" "$W/calls.txt" | cut -c1-60))"
            bad=$((bad + 1)); return
        fi
        paste -d'\t' "$W/exp.txt.calls" "$W/exp.txt" "$W/got.txt" >"$W/all.txt"
        if [ -n "$EVAL_SWEEP_LIB" ]; then
            [ "$(wc -l <"$W/got.txt")" -ne "$(wc -l <"$W/calls.txt")" ] &&
                echo "$f: gcc printed $(wc -l <"$W/got.txt") result lines for $(wc -l <"$W/calls.txt") calls (program output in the way?)"
            awk -F'\t' -v f="$f" -v nt="$(cat "$W/exp.txt.ntrap")" '
                $3 == "!" { crash[$1]++; nc++; next }
                $2 == $3 { ne++; next }
                {
                    nd++
                    if (!dif[$1]++) {
                        k = 1
                        while (k <= length($2) && substr($2, k, 1) == substr($3, k, 1)) k++
                        w = k > 24 ? k - 24 : 1
                        ex[$1] = "..." substr($2, w, 48) " vs gcc ..." substr($3, w, 48) " (hex digit " k " of " length($2) ")"
                    }
                }
                END {
                    for (k in dif) printf "%s: differs %s x%d: evaluator, gcc: %s\n", f, k, dif[k], ex[k]
                    for (k in crash) printf "%s: gcc crashed %s x%d\n", f, k, crash[k]
                    printf "%s: lib oracle: %d calls finished by the evaluator, %d equal, %d differ, %d gcc crashed; %d evaluator stops\n", f, ne + nd + nc, ne, nd, nc, nt
                }' "$W/all.txt"
            return
        fi
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
        if [ -n "$EVAL_SWEEP_SOLVE" ] && [ "$(basename "$1")" = solve.c ]; then
            # every guarded function must have a call that took its guarded edge (result 1)
            miss=$(awk -F'\t' '{ split($1, a, " "); if (substr($2, 1, 8) == "01000000") hit[a[1]] = 1 }
                END { split("magic square sumsw neighbour mixed", w, " ");
                      for (i = 1; i in w; i++) if (!(("1:" w[i]) in hit)) printf "%s ", w[i] }' "$W/all.txt")
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
}

if [ -n "$EVAL_SWEEP_LINK" ]; then
    run_group "$@"
else
    for f in "$@"; do run_group "$f"; done
fi
[ "$bad" -eq 0 ]
