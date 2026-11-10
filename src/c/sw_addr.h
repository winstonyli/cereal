/* sw_addr.h - where a lazy sweep blob lives, on both sides (docs/LAZY_ADDR_DESIGN.md).
 *
 * The evaluator's pointers are `pid << 32 | offset`; the gcc driver's are real addresses.  A pointer into a lazy blob that a function
 * turns into an integer (`(unsigned long)p & 63`, a hash of p) gets the model address, and the driver maps blob k there, so both sides
 * compute the same integer.  The base's high word (16384) is above any object id, so a model address that comes back as a pointer is
 * "no object" in the evaluator, never a real one; the evaluator stops on it (ev_access).  Plain types: the driver includes this too. */
#define SW_BASE 0x400000000000ull
#define SW_STRIDE 0x1000000ull
#define SW_END (SW_BASE + 32 * SW_STRIDE)     /* 32 = LMAXBL, the most lazy blobs of a call */
#define SW_ADDR(k, off) (SW_BASE + (unsigned long long)(k) * SW_STRIDE + (unsigned long long)(long long)(off))
