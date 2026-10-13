C=$1
# B5 (docs/B5_DESIGN.md 14.2): `index-summary --dump` of two units under
# ./.cereal (-nostdinc).  ok.c: every D8 key kind (external function and
# object, static function and object, tag, typedef, enumerator, macro),
# calls, xcalls (CALL_TWICE), declarations in the header, the config file in
# the manifest.  miss.c: two missing includes and status PARTIAL.
for f in ok.c miss.c; do
    echo "== $f"
    "$C" index-summary --dump $f
done
