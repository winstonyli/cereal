// flags: -Wall -DDUP=1 -DDUP=2 -imacros cmdline_inc/mac.h -include cmdline_inc/warn.h
/* -include/-imacros files are ordinary user files: warnings are shown, and a
 * redefinition on the command line warns at <command-line> (no position). */
int use(void) { return CL_MAC + CL_FROM_HDR; }
