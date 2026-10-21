#!/bin/sh
# eval.sh CEREAL: `cereal run` (src/c/eval.c, eval_libc.c) against gcc.
# tests/eval/NAME.c: main() runs in the evaluator and in a gcc -O0 -fwrapv build, each
# wrapped so that its value is printed after the program's own output; stdout and the
# exit status must be equal.  args_NAME.c: run as it is, with the arguments in its
# first line ("// args: a b c"), both sides.  trap_NAME.c: evaluation must stop with
# status 125 and the reason named in its first line ("// trap: <text>") on stderr, not
# give a value.
C=${1:?cereal binary}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d) || exit 1
pass=0; fail=0
for f in "$HERE"/eval/*.c; do
    n=$(basename "$f" .c)
    case $n in
    trap_*)
        want=$(sed -n '1s|^// trap: ||p' "$f")
        "$C" run -std=gnu11 "$f" >"$W/out" 2>"$W/err" </dev/null; st=$?
        if [ $st -eq 125 ] && grep -qF -- "$want" "$W/err"; then pass=$((pass + 1))
        else fail=$((fail + 1)); echo "FAIL $n: want '$want' and status 125, got status $st: $(head -c 300 "$W/err")"; fi;;
    *)
        src=$f
        args=
        case $n in args_*) args=$(sed -n '1s|^// args: ||p' "$f");; esac
        if [ -z "$args" ] && [ "${n#args_}" = "$n" ]; then
            cat >"$W/w.c" <<EOF
#define main eval_main_
#include "$f"
#undef main
#include <stdio.h>
int main(void) { int r = eval_main_(); printf("%d\n", r); return 0; }
EOF
            src=$W/w.c
        fi
        gcc -O0 -fwrapv -w -o "$W/exe" "$src" || { fail=$((fail + 1)); echo "FAIL $n: gcc"; continue; }
        # shellcheck disable=SC2086
        "$W/exe" $args >"$W/gout" 2>/dev/null </dev/null; gs=$?
        # shellcheck disable=SC2086
        "$C" run --eval-wrap -std=gnu11 "$src" -- $args >"$W/cout" 2>"$W/cerr" </dev/null; cs=$?
        if [ $gs -eq $cs ] && cmp -s "$W/gout" "$W/cout"; then pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL $n: cereal status $cs, gcc $gs; $(head -c 300 "$W/cerr")"
            diff "$W/gout" "$W/cout" | head -5
        fi;;
    esac
done
rm -rf "$W"
echo "eval: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
