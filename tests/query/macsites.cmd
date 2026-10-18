C=$1
# Q7 (docs/MACRO_SITES_PLAN.md): the uses a macro body spells are listed at
# each outermost invocation (WRAP, TWICE and NEST for callee, once per
# invocation even when the body spells it twice; LEN for the field), not at the
# body token (D1, open).  An invocation is still the macro for hover and definition, and a
# rename through a body or an invocation is refused (R4/R7).
for q in "refs macsites.c:1:5" "uses macsites.c:1:5" "refs macsites.c:2:22" \
         "highlight macsites.c:1:5" "callers macsites.c:1:5" \
         "hover macsites.c:7:23" "def macsites.c:7:23" "refs macsites.c:7:23" \
         "rename=other macsites.c:1:5" "rename=wid macsites.c:2:22"; do
    echo "== $q"
    "$C" query $q macsites.c 2>&1
done
