// flags: -Wall
#if -1 > 0U
#endif

#if 0U + -1
#endif

#if 0U * -1
#endif

#if 1U / -2
#endif

#if -1 % 1U
#endif

#if 1 ? 0U : -1
#endif

#if 1 ? -1 : 0U
#endif
#if 0 && (-1 > 0U)
#endif
#if (-1) > 0U
#endif
#if -1 * 2 > 0U
#endif
#if -1 == 0U
#endif
