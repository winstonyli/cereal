#if V >= 3
#elif V >= 2
#else
#endif
#if V < 1 || defined(W)
#endif
#if !defined(W) && (V == 4 || V != 4)
#endif
