C=$1
# hover: a macro inside a nested macro argument (LIMIT through TWICE, SQUARE)
# answers; in an #if 0 region, a macro visible there answers (SQUARE) and one
# whose #define is itself skipped (GONE) says it is inactive code, naming the
# conditional: the innermost one around the group (#ifdef at 25 for the #else
# group and the #if 1 nested in it), not one inside it; active code (26) and a
# directive line (27) have no note.
for q in "def lsp.c:8:13" "def lsp.c:4:18" "def lsp.c:9:19" "def lsp.c:16:24" "def lsp.c:2:21" \
         "def lsp.c:1:12" "def lsp.c:19:12" "refs lsp.c:2:9" "refs lsp.c:15:9" "refs lsp.h:3:9" \
         "refs lsp.c:5:9" "refs lsp.c:2:16" "hover lsp.c:9:13" "hover lsp.c:10:13" \
         "hover lsp.c:2:21" "hover lsp.c:9:19" "hover lsp.c:19:12" "hover lsp.c:23:12" \
         "hover lsp.c:19:2" "hover lsp.c:26:2" "hover lsp.c:27:2" "hover lsp.c:28:6" \
         "hover lsp.c:30:6" \
         "visible lsp.c:14:1" "visible lsp.c:15:1" "expand lsp.c:9:13"; do
    echo "== $q"
    "$C" query $q lsp.c
done
