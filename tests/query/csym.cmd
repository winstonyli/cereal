C=$1
# C names (B2 phase 2).  Merge: an expanded macro (LIMIT) answers; a name
# with only macro history (width, #undef'd) and a plain C name (total) go
# to the C index; a #define body token names one entity per expansion (_t)
# and is no use (refs lists nothing).  uses: refs without declarations;
# highlight: this file only, write/read.  hover (phase 3), the same merge:
# LIMIT's #define; width's C text with "(also a macro name)"; a function,
# variables, a parameter, a field, a tag, a macro-argument use (local) and
# the body token's two entities with one text.
for q in "def csym.c:15:32" "refs csym.c:15:32" "highlight csym.c:4:9" "uses csym.c:4:9" \
         "def csym.c:15:16" "refs csym.c:15:16" \
         "def csym.c:15:24" "decl csym.c:15:24" "refs csym.c:15:24" "uses csym.c:15:24" \
         "highlight csym.c:15:24" "refs csym.c:11:5" "decl csym.c:11:5" \
         "refs csym.c:13:13" "refs csym.c:10:16" "refs csym.c:10:8" \
         "def csym.c:6:23" "refs csym.c:6:33" \
         "hover csym.c:15:32" "hover csym.c:15:16" "hover csym.c:11:5" \
         "hover csym.c:15:24" "hover csym.c:11:19" "hover csym.c:14:20" \
         "hover csym.c:10:8" "hover csym.c:14:29" "hover csym.c:6:23"; do
    echo "== $q"
    "$C" query $q csym.c
done
