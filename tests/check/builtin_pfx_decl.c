// flags: -std=gnu99 -Wbuiltin-declaration-mismatch
extern void __clear_cache (char*, char*);
void __builtin_prefetch (const char *, ...);
void __builtin_trap (int);
void trap (int);
