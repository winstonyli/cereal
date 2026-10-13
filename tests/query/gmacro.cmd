C=$1
# A function defined by a macro invocation (docs/B4_DESIGN.md 15, G1): the
# root scope of DEFINE_FN(made) covers its argument token, so made is the
# caller of the xcall of leaf, and leaf's incoming calls list it.
for q in "callees gmacro.c:3:11" "callees gmacro.c:4:11" "callers gmacro.c:1:5" \
         "callers gmacro.c:3:11" "callees gmacro.c:5:5"; do
    echo "== $q"
    "$C" query $q gmacro.c
done
