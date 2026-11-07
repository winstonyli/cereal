// flags: -Wall
/* Exclusion pairs never conflict on a variable: gcc drops the inapplicable attribute first
 * ("attribute ignored"), so only the function pairs print a conflict. common/nocommon is the
 * one pair that applies to objects. */
int v1 __attribute__((noreturn, pure));
int v2 __attribute__((const, noreturn));
int v3 __attribute__((malloc, pure));
int v4 __attribute__((cold, hot));
int v5 __attribute__((always_inline, noinline));  /* gcc orders the two "ignored" lines as written */
int v6 __attribute__((returns_twice, noreturn));
int v7 __attribute__((alloc_size(1), const));
int v8 __attribute__((alloc_size(1), pure));
int v9 __attribute__((alloc_size(1), noreturn));
int v10 __attribute__((alloc_align(1), const));
int v11 __attribute__((alloc_align(1), pure));
int v12 __attribute__((alloc_align(1), noreturn));
int v13 __attribute__((warn_unused_result, noreturn));
int v14 __attribute__((const, malloc));
int v15 __attribute__((gnu_inline, noinline));
int v16 __attribute__((noipa, always_inline));
/* pins: these two pairs already skipped variables */
int v17 __attribute__((aligned(8), packed));
int v18 __attribute__((stack_protect, no_stack_protector));
/* the one object pair */
int v19 __attribute__((common, nocommon));
int v20 __attribute__((nocommon, common));
typedef int t1 __attribute__((common, nocommon));
struct s { int f __attribute__((common, nocommon)); };
void fn(int p __attribute__((common, nocommon)));
int v5b __attribute__((noinline, always_inline));
