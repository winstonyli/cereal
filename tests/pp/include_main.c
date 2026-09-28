#include "inc/guarded.h"
#include "inc/guarded.h"
#include "inc/once.h"
#include "inc/once.h"
#define HDR "inc/guarded.h"
#include HDR
#define ANG <stddef.h>
#include ANG
#if __has_include("inc/once.h") && !__has_include(<nonexistent_header.h>)
has_include_ok
#endif
__FILE__ __LINE__ __STDC_VERSION__ __STDC__
#line 100 "renamed.c"
__FILE__ __LINE__
guarded_value once_value
