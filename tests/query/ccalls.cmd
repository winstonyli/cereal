C=$1
# C call hierarchy (docs/B4_DESIGN.md 5.7): callers and callees of a function
# (a system, an implicit and a declaration-only callee; a call spelled in a
# #define body, an xcall; recursion; a call in a file-level _Static_assert),
# of file-level variables (the caller of a call in an initializer, one per
# declarator), of an item in a header, and a strong macro name (the macro
# output stands).
for q in "callers ccalls.c:12:5" "callees ccalls.c:13:5" "callers ccalls.c:8:5" \
         "callers ccalls.c:13:5" "callees ccalls.c:9:5" "callees ccalls.c:10:5" \
         "callees ccalls.c:10:30" "callers ccalls.c:7:5" "callees ccalls.c:8:5" \
         "callees ccalls.h:2:19" "callers ccalls.c:6:5" "callers ccalls.c:17:28" \
         "callers ccalls_sys/fakelib.h:1:15" "callees ccalls.c:5:5" \
         "callers ccalls.c:9:5" "callees ccalls.c:3:9" "callers ccalls.c:4:9"; do
    echo "== $q"
    "$C" query $q ccalls.c -isystem ccalls_sys
done
# a called field of a system struct keeps its lazy DEF event (the csx_field role mask)
echo "== field cb"
"$C" check --target=x86_64-linux-gnu --dump-symbols ccalls.c -isystem ccalls_sys 2>/dev/null | grep -F ' field cb -> '
