# files that are not edited: read-only, with other hard links, not regular
# (a fifo cannot be a source, so a symlink to a directory stands in), and
# outside --root; a symlink is followed and stays a link
. "$TESTS/fix/lib.sh"
body='#define SUM 1 + 2\nint v = SUM;\n'
mkdir -p t/real t/other
printf "$body" >t/ro.c
printf "$body" >t/hl.c
ln t/hl.c t/hl2.c
printf "$body" >t/real/target.c
ln -s real/target.c t/link.c
printf "$body" >t/other/out.c
printf "$body" >t/real/in.c
chmod a-w t/ro.c
snap
run ro.c
run hl.c
run link.c
(cd t && ls -l link.c | sed 's/.*link.c/link.c/')
run --root=real other/out.c real/in.c
chmod u+w t/ro.c
