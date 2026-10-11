#!/bin/sh
# fixgate.sh [DIR]: `cereal fix` against cereal's own check and gcc-13
# ($FIXGATECC).  Each case (one C file, copied to a scratch directory) is
# fixed twice, once for SAFE fixes and once with --unsafe, and then:
#   - a second fix run is a no-op (exit 0, nothing printed, file unchanged);
#   - `cereal check` and `cereal lint` of the result report no diagnostic
#     that the original did not (compared as "level: message", multiset,
#     lines and columns dropped; for --unsafe only errors count), and when
#     the fix changed the file, fewer diagnostics;
#   - gcc-13 -fsyntax-only still accepts what it accepted before;
#   - SAFE run only: `gcc-13 -O0 -c` (no -g) gives the same code (the .text
#     section: a stringized macro argument changes data), unless the fix
#     inserted an #include, which can redirect a function (scanf);
# Exits of cereal fix other than 0 and 1 are failures, except 3 (an input it
# cannot check, for example an #include it cannot find), which is counted as
# "unusable".
#   no argument: tests/check/fixit-*.c (the file's "// flags:" line, minus
#                -fdiagnostics-*) and the tests/lint inputs (cereal-flags:).
#   DIR:         a sample of the C files under DIR (FIXGATE_MAX, default 300,
#                evenly spaced in sorted order), flags in $FIXGATE_FLAGS
#                (default "-Wall -Wextra -std=gnu17"), includes found through
#                -I of the file's directory (nothing outside the scratch
#                directory is ever edited: --root).
# Prints "N cases, M failed, U unusable" and the failures.  FIXGATE_JOBS
# (default 8, at most 12) bounds the concurrency.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CEREAL=${CEREAL:-$ROOT/cereal}
GCC=${FIXGATECC:-gcc-13}
export ROOT CEREAL GCC

if [ "${1:-}" = --one ]; then   # --one FILE FLAGS [INCDIR]: prints "ok", "unusable" or the failures
    f=$2 flags=$3 inc=${4:-}
    name=$(basename "$f")
    w=$(mktemp -d "${TMPDIR:-/tmp}/fixgate-one.XXXXXX") || exit 9
    trap 'chmod -R u+rwx "$w" 2>/dev/null; rm -rf "$w"' EXIT INT TERM
    fail=
    note() { fail="$fail
    $*"; }
    I=
    [ -n "$inc" ] && I="-I$inc -I$inc/.."
    # shellcheck disable=SC2086
    diags() { # diags FILE: "id: message" lines of cereal's check, sorted
        (cd "$w/$1" && for m in check lint; do
            LC_ALL=C "$CEREAL" $m $flags $I "$name" 2>&1 </dev/null; done) |
            sed -n 's/^[^ :][^ ]*:[0-9]*:[0-9]*: \(warning\|error\): /\1: /p' | sort
    }
    # shellcheck disable=SC2086
    gccok() { (cd "$w/$1" && LC_ALL=C "$GCC" -fsyntax-only -w $flags $I "$name" >/dev/null 2>&1 </dev/null); }
    # shellcheck disable=SC2086
    obj() { (cd "$w/$1" && LC_ALL=C "$GCC" -O0 -c -w $flags $I -o o.o "$name" >/dev/null 2>&1 </dev/null &&
        objcopy -O binary --only-section=.text o.o text.bin); }
    for mode in safe unsafe; do
        mkdir "$w/$mode" "$w/$mode.orig" || exit 9
        cp "$f" "$w/$mode/$name"
        cp "$f" "$w/$mode.orig/$name"
        u=
        [ $mode = unsafe ] && u=--unsafe
        # shellcheck disable=SC2086
        (cd "$w/$mode" && timeout 300 "$CEREAL" fix -j1 $u --root="$w/$mode" $flags $I "$name" \
            >"$w/$mode.out" 2>"$w/$mode.err" </dev/null)
        rc=$?
        case $rc in
        0 | 1) ;;
        3) echo unusable; exit 0 ;;
        *) note "$mode: cereal fix exit $rc: $(head -c 300 "$w/$mode.err" | tr '\n' '|')"; continue ;;
        esac
        cp "$w/$mode/$name" "$w/$mode.fixed"
        # shellcheck disable=SC2086
        (cd "$w/$mode" && timeout 300 "$CEREAL" fix -j1 $u --root="$w/$mode" $flags $I "$name" \
            >"$w/$mode.out2" 2>"$w/$mode.err2" </dev/null)
        rc2=$?
        # a second run may skip again, but changes nothing (unless the first
        # one ran out of passes)
        if grep -q 'fixes remain after' "$w/$mode.err"; then
            :
        elif ! cmp -s "$w/$mode.fixed" "$w/$mode/$name"; then
            note "$mode: a second run changed the file again"
        elif [ $rc2 -gt 1 ]; then
            note "$mode: second run exit $rc2"
        elif grep -q '^cereal fix: [1-9][0-9]* edit' "$w/$mode.err2"; then
            note "$mode: the second run still applies edits: $(grep 'edit' "$w/$mode.err2" | head -c 200)"
        fi
        mkdir -p "$w/before" && cp "$f" "$w/before/$name"
        diags before >"$w/d.before"
        diags $mode >"$w/d.after"
        # Fixing an error can uncover warnings the broken code hid, and a MAYBE
        # fix can uncover a second bad conversion of the same printf call:
        # for those, only more errors count
        new=$(LC_ALL=C comm -13 "$w/d.before" "$w/d.after")
        if [ $mode = unsafe ] || grep -q '^error:' "$w/d.before"; then
            new=
            [ "$(grep -c '^error:' "$w/d.after")" -gt "$(grep -c '^error:' "$w/d.before")" ] &&
                new=$(LC_ALL=C comm -13 "$w/d.before" "$w/d.after" | grep '^error:')
        fi
        if [ -n "$new" ]; then
            note "$mode: new diagnostics: $(echo "$new" | head -3 | tr '\n' '|')"
        fi
        # (--unsafe may trade one warning for another, missing braces for
        # designated-init)
        if [ $mode = safe ] && grep -q '^cereal fix: [1-9][0-9]* edit' "$w/$mode.err" &&
            [ "$(wc -l <"$w/d.after")" -ge "$(wc -l <"$w/d.before")" ] &&
            [ "$(grep -c '^error:' "$w/d.after")" -ge "$(grep -c '^error:' "$w/d.before")" ]; then
            # (an error can become a warning: "error;" to "perror;")
            note "$mode: edits made, but no diagnostic fewer"
        fi
        if gccok before; then
            gccok $mode || note "$mode: $GCC rejects the result"
            # (an inserted #include may redirect a function, scanf to
            # __isoc99_scanf: the object code is then expected to differ)
            if [ $mode = safe ] && ! diff "$f" "$w/safe/$name" | grep -q '^> *#include' &&
                cp "$f" "$w/before/$name" && obj before && obj safe; then
                cmp -s "$w/before/text.bin" "$w/safe/text.bin" || note "safe: object code differs"
            fi
        fi
    done
    ed=; grep -qh "^cereal fix: [1-9][0-9]* edit" "$w"/safe.err "$w"/unsafe.err 2>/dev/null && ed=" edited"
    if [ -n "$fail" ]; then echo "FAIL $f$fail"; else echo "ok$ed"; fi
    exit 0
fi

jobs=${FIXGATE_JOBS:-8}
[ "$jobs" -gt 12 ] && jobs=12
list=$(mktemp "${TMPDIR:-/tmp}/fixgate-list.XXXXXX") || exit 9
res=$(mktemp "${TMPDIR:-/tmp}/fixgate-res.XXXXXX") || exit 9
trap 'rm -f "$list" "$res"' EXIT INT TERM
if [ -z "${1:-}" ]; then
    for f in "$ROOT"/tests/check/fixit-*.c; do
        fl=$(sed -n 's|^// flags: *||p' "$f" | head -1 | tr ' ' '\n' |
            grep -v '^-fdiagnostics' | tr '\n' ' ')
        printf '%s|%s|%s\n' "$f" "${fl% }" "" >>"$list"
    done
    for f in "$ROOT"/tests/lint/*.c; do
        fl=$(sed -n 's|.*cereal-flags: *||p' "$f" | head -1 | sed 's| *\*/.*||')
        printf '%s|%s|%s\n' "$f" "$fl" "$(dirname "$f")" >>"$list"
    done
else
    flags=${FIXGATE_FLAGS:-"-Wall -Wextra -std=gnu17"}
    max=${FIXGATE_MAX:-300}
    find "$1" -name '*.c' | LC_ALL=C sort >"$list.all"
    total=$(wc -l <"$list.all")
    step=$(( (total + max - 1) / max ))
    [ "$step" -lt 1 ] && step=1
    sed -n "1~${step}p" "$list.all" | while read -r f; do
        printf '%s|%s|%s\n' "$f" "$flags" "$(dirname "$f")"
    done >"$list"
    rm -f "$list.all"
fi
n=$(wc -l <"$list")
# one case per line: FILE|FLAGS|INCDIR
tr '\n' '\0' <"$list" |
    xargs -0 -P "$jobs" -I{} sh -c '
        IFS="|"
        set -- $1
        exec sh "$0" --one "$1" "${2:-}" "${3:-}"' "$0" {} >"$res" 2>&1
bad=$(grep -c '^FAIL' "$res")
unusable=$(grep -c '^unusable' "$res")
echo "$n cases ($(grep -c "^ok edited" "$res") with edits), $bad failed, $unusable unusable"
grep -A8 '^FAIL' "$res" | grep -v '^--$' | grep -v "^ok" | head -200
[ "$bad" -eq 0 ]
