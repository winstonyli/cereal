#include "inc/good.h"
#include "inc/good.h" /* expect: duplicate-include */
#include "inc/typo_guard.h"
#include "inc/noguard.h"
#include "inc/macros_only.h" /* expect: unused-include */
#include "inc/collide_a.h"
#include "inc/collide_b.h"
#define NEVER_USED 1 /* expect: unused-macros */
#define USED 2
int x = USED + GOOD + NOGUARD + COLLIDE_A;
