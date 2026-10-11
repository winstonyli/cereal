# lib.sh - helpers for the cases of tests/fix/NAME.sh (run by fix_case.sh in
# a scratch directory $W; the files under test live in $W/t; $CEREAL is the
# program, $J the -j to use, $TESTS the tests directory).

# what the output shows of a path or a byte: the scratch name, CR, a BOM
norm() { sed -e "s|$W|<W>|g" -e 's/\r/<CR>/g' -e 's/\xef\xbb\xbf/<BOM>/g'; }

# remember the tree, so that the next `run` shows only what it changed
snap() {
    chmod -R u+rwx "$W/snap" 2>/dev/null
    rm -rf "$W/snap"
    mkdir "$W/snap"
    cp -a "$W/t/." "$W/snap/"
}

changes() {
    (cd "$W" && diff -ruN snap t |
        sed -e 's/^\(---\|+++\) \([^	]*\)	.*/\1 \2/' | norm)
}

# run ARGS: `cereal fix -j J ARGS` in t/; its exit status, output and the
# changes it made
run() {
    echo "\$ cereal fix $*"
    (cd "$W/t" && "$CEREAL" fix -j "$J" "$@" >"$W/out" 2>"$W/err" </dev/null)
    echo "exit $?"
    if [ -s "$W/out" ]; then echo "-- stdout"; norm <"$W/out"; fi
    if [ -s "$W/err" ]; then echo "-- stderr"; norm <"$W/err"; fi
    ch=$(changes)
    if [ -n "$ch" ]; then echo "-- changes"; echo "$ch"; fi
    snap
}

# put FILE (a path under tests/) into t/, as NAME if given
put() {
    mkdir -p "$W/t/$(dirname "${2:-$(basename "$1")}")"
    cp "$TESTS/$1" "$W/t/${2:-$(basename "$1")}"
    snap
}
