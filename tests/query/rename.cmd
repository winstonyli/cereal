C=$1
# Rename of C names (B2 phase 4; B2_DESIGN.md "Phase 4 design addendum").
# Accepted: a local, a parameter, a static and an external global used only
# in this file, a field (member access and designators), a tag (to a name
# the ordinary namespace already has), a typedef, a label, the outer of two
# shadowing locals, a local declared in a macro argument (the argument is
# edited), a no-op, and names other namespaces have (a label to a
# parameter, a field to a global).
# Refused from the index: a #define body name, a name formed by ##, a
# function declared in a header, a predeclared typedef, an implicit function
# declaration, a macro at the cursor.  Refused by the name: a keyword (also
# a GNU one), not an identifier, a macro name.  Refused by inactive code:
# the old name (sum) and the new name (later) in helper's #if 0, which does
# not block names of other functions.  Refused by the second check: a local
# renamed to another local of its scope, two parameters with one name, a
# shadowing local capturing a use of a global, a macro argument token that
# also names another entity, an inner local capturing a use of a global, a
# duplicate member, a duplicate label, a typedef renamed to a variable used
# where it is, a local capturing a #define body's use of a global, and a
# renamed macro argument pasted into an undeclared name.
for q in "rename=in2 rename.c:19:13" "rename=first rename.c:15:23" \
         "rename=count rename.c:10:12" "rename=grand rename.c:11:5" \
         "rename=px rename.c:31:8" "rename=total rename.c:8:8" \
         "rename=len_t rename.c:9:23" "rename=done rename.c:36:14" \
         "rename=w rename.c:45:9" "rename=loc2 rename.c:50:10" \
         "rename=counter rename.c:10:12" \
         "rename=x rename.c:3:24" "rename=w rename.c:13:10" \
         "rename=w rename.c:14:5" "rename=w rename.c:61:5" \
         "rename=w rename.c:63:12" "rename=w rename.c:50:5" \
         "rename=while rename.c:30:10" "rename=typeof rename.c:30:10" \
         "rename=9a rename.c:30:10" "rename=TWICE rename.c:30:10" \
         "rename=acc rename.c:17:9" "rename=later rename.c:19:13" \
         "rename=k2 rename.c:33:23" "rename=k rename.c:38:1" \
         "rename=counter rename.c:8:20" \
         "rename=p rename.c:30:10" "rename=b rename.c:15:23" \
         "rename=total rename.c:47:13" "rename=zz rename.c:68:9" \
         "rename=counter rename.c:19:13" "rename=y rename.c:8:20" \
         "rename=again rename.c:38:1" "rename=p rename.c:9:23" \
         "rename=gy rename.c:54:13" "rename=w2 rename.c:13:5"; do
    echo "== $q"
    "$C" query $q rename.c 2>&1
done
