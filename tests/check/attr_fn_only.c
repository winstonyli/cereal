// flags: -Wall
int v1 __attribute__ ((fd_arg (1)));
int *v2 __attribute__ ((nocf_check));
void (**v3) (void) __attribute__ ((nocf_check));
void (*v4) (int) __attribute__ ((nocf_check));
typedef int T1 __attribute__ ((warn_unused_result));
typedef void (*P1) (int) __attribute__ ((nocf_check));
int v5 __attribute__ ((format (printf, 1, 2)));
void f1 (char *p) __attribute__ ((fd_arg (1)));
void f2 (int fd) __attribute__ ((fd_arg (0)));
void f3 (int fd) __attribute__ ((fd_arg (2)));
void f4 (int fd) __attribute__ ((fd_arg ("x")));
void f5 (int fd) __attribute__ ((fd_arg_read (1)));
