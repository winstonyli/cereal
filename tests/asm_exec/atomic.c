/* Atomics for tests/asm_exec.py: every row of the table, at every width, old and new values. */
signed char c8 = -5;
unsigned char u8 = 200;
short s16 = -300;
unsigned short u16 = 60000;
int i32 = 1000;
unsigned u32 = 4000000000u;
long long i64 = -5000000000LL;
unsigned long long u64 = 1ULL << 63;

#define RMW(T, G, N)                                                              \
T N##_fa(T k) { return __atomic_fetch_add(&G, k, 5) + G; }                        \
T N##_fs(T k) { return __atomic_fetch_sub(&G, k, 5) + G; }                        \
T N##_fn(T k) { return __atomic_fetch_and(&G, k, 5) + G; }                        \
T N##_fo(T k) { return __atomic_fetch_or(&G, k, 5) + G; }                         \
T N##_fx(T k) { return __atomic_fetch_xor(&G, k, 5) + G; }                        \
T N##_fd(T k) { return __atomic_fetch_nand(&G, k, 5) + G; }                      \
T N##_af(T k) { return __atomic_add_fetch(&G, k, 5) * 3 + G; }                    \
T N##_sf(T k) { return __atomic_sub_fetch(&G, k, 5) * 3 + G; }                    \
T N##_nf(T k) { return __atomic_and_fetch(&G, k, 5) * 3 + G; }                    \
T N##_of(T k) { return __atomic_or_fetch(&G, k, 5) * 3 + G; }                     \
T N##_xf(T k) { return __atomic_xor_fetch(&G, k, 5) * 3 + G; }                    \
T N##_df(T k) { return __atomic_nand_fetch(&G, k, 5) * 3 + G; }                   \
T N##_ya(T k) { return __sync_fetch_and_add(&G, k) + G; }                         \
T N##_ys(T k) { return __sync_fetch_and_sub(&G, k) + G; }                         \
T N##_yn(T k) { return __sync_fetch_and_and(&G, k) + G; }                         \
T N##_yo(T k) { return __sync_fetch_and_or(&G, k) + G; }                          \
T N##_yx(T k) { return __sync_fetch_and_xor(&G, k) + G; }                         \
T N##_yd(T k) { return __sync_fetch_and_nand(&G, k) + G; }                        \
T N##_za(T k) { return __sync_add_and_fetch(&G, k) * 3 + G; }                     \
T N##_zs(T k) { return __sync_sub_and_fetch(&G, k) * 3 + G; }                     \
T N##_zn(T k) { return __sync_and_and_fetch(&G, k) * 3 + G; }                     \
T N##_zo(T k) { return __sync_or_and_fetch(&G, k) * 3 + G; }                      \
T N##_zx(T k) { return __sync_xor_and_fetch(&G, k) * 3 + G; }                     \
T N##_zd(T k) { return __sync_nand_and_fetch(&G, k) * 3 + G; }                    \
T N##_ld(T k) { return __atomic_load_n(&G, 5) + k; }                              \
T N##_st(T k) { __atomic_store_n(&G, k, 5); return G; }                           \
T N##_xc(T k) { T o = __atomic_exchange_n(&G, k, 5); return o * 3 + G; }          \
T N##_ts(T k) { T o = __sync_lock_test_and_set(&G, k); return o * 3 + G; }        \
T N##_rl(T k) { __sync_lock_release(&G); return G + k; }                          \
T N##_ce(T k)                                                                     \
{                                                                                 \
    T want = G, to = k;                                                           \
    int ok = __atomic_compare_exchange_n(&G, &want, k, 0, 5, 5);                  \
    int ok2 = __atomic_compare_exchange(&G, &to, &want, 0, 5, 5);                 \
    return ok * 100 + ok2 * 10 + want + to + G;                                   \
}                                                                                 \
T N##_cf(T k)                                                                     \
{                                                                                 \
    T want = (T)(G + 1);                                                          \
    int ok = __atomic_compare_exchange_n(&G, &want, k, 0, 5, 5);                  \
    return ok * 100 + want + G;                                                   \
}                                                                                 \
T N##_cb(T k)                                                                     \
{                                                                                 \
    int a = __sync_bool_compare_and_swap(&G, G, k);                               \
    int b = __sync_bool_compare_and_swap(&G, (T)(G + 1), (T)0);                   \
    return a * 10 + b + G;                                                        \
}                                                                                 \
T N##_cv(T k)                                                                     \
{                                                                                 \
    T a = __sync_val_compare_and_swap(&G, G, k);                                  \
    T b = __sync_val_compare_and_swap(&G, (T)(G + 1), (T)0);                      \
    return a * 3 + b + G;                                                         \
}

RMW(signed char, c8, c8)
RMW(unsigned char, u8, u8)
RMW(short, s16, s16)
RMW(unsigned short, u16, u16)
RMW(int, i32, i32)
RMW(unsigned, u32, u32)
RMW(long long, i64, i64)
RMW(unsigned long long, u64, u64)
