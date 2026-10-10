/* -include: its text is output, and it sees every -D/-U and -imacros macro */
#ifdef Q
int inc_sees_q = FROM_NESTED;
#else
int inc_misses_q;
#endif
