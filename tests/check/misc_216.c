// flags: -Wtraditional
#define foo bar
 #define foo bar
#pragma once_not
 # pragma foo
#if 0
#elif 1
#endif
 #if 1U
 #endif
#if +1
#endif
#define s1(h) "h3" 'h'
#define fl(x) x
int fl;
int z = sizeof(int);
