C=$1
for q in "callees calls.c:3:9" "callers calls.c:2:9" "callers calls.c:1:9" \
         "callees calls.c:6:9" "callers calls.c:4:9" "callees calls.c:4:9" \
         "deps calls.c:7:9" "deps calls.c:8:9" "callers calls.c:7:5"; do
    echo "== $q"
    "$C" query $q calls.c
done
