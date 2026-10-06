// flags: -Wall -O2
inline void d1 (float *p) { }
inline __attribute__((always_inline)) void d2 (float *p) { }
static inline void d3 (float *p);
static inline __attribute__((always_inline)) void d4 (float *p) { }
void d5 (float *p);
__attribute__ ((malloc (d1))) float* a1 (int);
__attribute__ ((malloc (d2))) float* a2 (int);
__attribute__ ((malloc (d3))) float* a3 (int);
__attribute__ ((malloc (d4))) float* a4 (int);
__attribute__ ((malloc (d5))) float* a5 (int);
__attribute__ ((malloc (d5))) inline float* a6 (int n) { return 0; }
__attribute__ ((malloc)) inline float* a7 (int n) { return 0; }
inline __attribute__ ((malloc (d5))) float* a8 (int n);
__attribute__ ((malloc (d5))) static inline float* a9 (int n);
