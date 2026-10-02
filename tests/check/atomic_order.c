// flags:
char a;
int f(void) {
  __atomic_clear (&a, __ATOMIC_RELAXED);
  __atomic_thread_fence (__ATOMIC_SEQ_CST);
  return __atomic_test_and_set (&a, __ATOMIC_SEQ_CST) + __atomic_always_lock_free (1, 0);
}
