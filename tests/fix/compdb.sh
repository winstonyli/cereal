# compile_commands mode: a header included by several units is fixed once;
# a fix the units do not all report (D5) is not made
. "$TESTS/fix/lib.sh"
cat >t/h.h <<'EOF'
#define SUM 1 + 2
EOF
cat >t/a.c <<'EOF'
#include "h.h"
int a = SUM;
EOF
cat >t/b.c <<'EOF'
#include "h.h"
int b = SUM;
EOF
cat >t/g.h <<'EOF'
#ifdef A
#define TWO 1 + 1
#endif
EOF
cat >t/c.c <<'EOF'
#include "g.h"
int c;
EOF
cat >t/d.c <<'EOF'
#include "g.h"
int d;
EOF
db() {
    printf '[\n' >t/cdb.json
    sep=
    for e in "$@"; do
        printf '%s{"directory":"%s/t","command":"cc %s","file":"%s"}\n' \
            "$sep" "$W" "${e#*:}" "${e%%:*}" >>t/cdb.json
        sep=,
    done
    printf ']\n' >>t/cdb.json
}
db 'a.c:-Wall -c a.c' 'b.c:-Wall -c b.c'
snap
run -Wall --compile-commands cdb.json
run -Wall --compile-commands cdb.json
db 'c.c:-Wall -DA -c c.c' 'd.c:-Wall -c d.c'
snap
run -Wall --compile-commands cdb.json
db 'c.c:-Wall -DA -c c.c' 'd.c:-Wall -DA -c d.c'
snap
run -Wall --compile-commands cdb.json
