# every fix-it test input of tests/check and tests/lint, fixed with --unsafe
# and fixed again: the first run's exit status and summary, and whether the
# second run is a no-op (exit 0, no output, nothing changed)
. "$TESTS/fix/lib.sh"
one() {
    f=$1 flags=$2
    name=$(basename "$f")
    rm -rf "$W/t" "$W/snap"; mkdir "$W/t" "$W/snap"
    cp -R "$TESTS/$(dirname "$f")/." "$W/t/" 2>/dev/null
    snap
    # shellcheck disable=SC2086
    (cd "$W/t" && "$CEREAL" fix -j "$J" --unsafe $flags "$name" >"$W/out" 2>"$W/err" </dev/null)
    rc1=$?
    sum=$(sed -n 's/^cereal fix: \([0-9]* edit.*\)$/\1/p;s/^cereal fix: would.*//p' "$W/err" | tail -1)
    (cd "$W/t" && "$CEREAL" fix -j "$J" --unsafe $flags "$name" >"$W/out" 2>"$W/err" </dev/null)
    rc2=$?
    again=noop
    if [ $rc2 != 0 ] || [ -s "$W/err" ] || [ -s "$W/out" ]; then again="exit $rc2: $(norm <"$W/err" | tr '\n' '|')"; fi
    echo "$f [$flags]: exit $rc1 ${sum:-no edits}; again: $again"
}
for f in "$TESTS"/check/fixit-*.c; do
    flags=$(sed -n 's|^// flags: *||p' "$f" | head -1 | tr ' ' '\n' | grep -v '^-fdiagnostics' | tr '\n' ' ')
    one "check/$(basename "$f")" "${flags% }"
done
for f in "$TESTS"/lint/*fixit*.c; do
    flags=$(sed -n 's|.*cereal-flags: *||p' "$f" | head -1 | sed 's| *\*/.*||')
    one "lint/$(basename "$f")" "$flags"
done
