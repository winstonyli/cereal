C=$1
for q in "def lsp.c:8:13" "def lsp.c:4:18" "def lsp.c:9:19" "def lsp.c:16:24" "def lsp.c:2:21" \
         "def lsp.c:1:12" "def lsp.c:19:12" "refs lsp.c:2:9" "refs lsp.c:15:9" "refs lsp.h:3:9" \
         "refs lsp.c:5:9" "refs lsp.c:2:16" "hover lsp.c:9:13" "hover lsp.c:10:13" \
         "hover lsp.c:2:21" "visible lsp.c:14:1" "visible lsp.c:15:1" "expand lsp.c:9:13"; do
    echo "== $q"
    "$C" query $q lsp.c
done
