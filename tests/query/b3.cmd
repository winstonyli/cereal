C=$1
# B3 (docs/B3_DESIGN.md) on ../symidx/b3.c.  type: a variable through a
# const typedef (limit: CI), a function-pointer typedef parameter (f:
# cmp_fn), a pointer typedef parameter (p: AnonP), a field of an anonymous
# member (b: int, none), a block enum constant and a variable of that
# nameless enum (both: the enum), whitespace (no entity).  visible: file
# scope only before run (make is declared, run not yet); in the for body
# its locals, the parameters and the static local; in the inner block the
# nameless enum and shade, not the names of the for loop.
F=../symidx/b3.c
for q in "type $F:10:4" "type $F:14:23" "type $F:26:24" "type $F:7:42" \
         "type $F:22:31" "type $F:23:24" "type $F:22:30" \
         "visible $F:14:1" "visible $F:19:9" "visible $F:23:9"; do
    echo "== $q"
    "$C" query $q $F
done
