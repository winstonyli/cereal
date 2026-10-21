#!/bin/sh
# eval.sh CEREAL: the evaluator (src/c/eval.c, --eval-main) against gcc.
# tests/eval/NAME.c: main()'s value must equal the
# gcc -O0 -fwrapv build's; trap_NAME.c: evaluation must stop with the reason
# named in its first line ("// trap: <text>"), not give a value.
C=${1:?cereal binary}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d) || exit 1
cat >"$W/drv.c" <<'DRV'
#include <stdio.h>
int eval_main_(void);
int main(void) { printf("%d\n", eval_main_()); return 0; }
DRV
pass=0; fail=0
for f in "$HERE"/eval/*.c; do
    n=$(basename "$f" .c)
    case $n in trap_*) fl=--eval-main;; *) fl=--eval-wrap;; esac
    out=$("$C" check $fl -std=gnu11 "$f" 2>&1 </dev/null | grep '^eval ')
    case $n in
    trap_*)
        want=$(sed -n '1s|^// trap: ||p' "$f")
        case $out in *"$want"*) pass=$((pass + 1));; *) fail=$((fail + 1)); echo "FAIL $n: want '$want', got '$out'";; esac;;
    *)
        w=$(echo "$out" | sed -n 's/^eval main = //p')
        { gcc -O0 -fwrapv -w -Dmain=eval_main_ -c -o "$W/$n.o" "$f" && gcc -O0 -o "$W/$n" "$W/$n.o" "$W/drv.c"; } || { fail=$((fail + 1)); echo "FAIL $n: gcc"; continue; }
        g=$("$W/$n" </dev/null)
        if [ -n "$w" ] && [ "$w" = "$g" ]; then pass=$((pass + 1))
        else fail=$((fail + 1)); echo "FAIL $n: cereal '$out', gcc $g"; fi;;
    esac
done
rm -rf "$W"
echo "eval: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
