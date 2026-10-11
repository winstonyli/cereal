# a directory the file cannot be replaced in: exit status 4, the file is
# untouched, no temporary file is left (skipped when the tests run as root)
. "$TESTS/fix/lib.sh"
if [ "$(id -u)" = 0 ]; then echo "(root: skipped)"; exit 0; fi
mkdir t/d
printf '#define SUM 1 + 2\nint v = SUM;\n' >t/d/u.c
printf '#define SUM 1 + 2\nint v = SUM;\n' >t/ok.c
snap
chmod a-w t/d
run d/u.c ok.c
(cd t && ls -A d)
chmod u+w t/d
