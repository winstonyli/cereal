---
status: done
---

# Lazy blobs: a pointer read as an integer

Goal: a sweep call that turns a pointer into a lazy blob into an integer (`(uintptr_t)p`, `p & 63`, a hash of `p`) runs on instead of stopping,
and the gcc side computes the same integer.
Mechanism today: `convert` (`src/c/eval.c:541`) raises `UNSUP "address as an integer"` for a non-NULL pointer converted to a non-pointer,
non-bool type while `v->lazy_fill` is set. Evaluator pointers are `pid << 32 | offset` (`pid()`, `eval_int.h:232`), gcc's are real addresses
from the call's `calloc` arena (`sw_load`, `tests/sweep_rt.h:49`).
Review rounds: 2 (fewer-concepts reviewer, adversarial reviewer); both found the pid/blob-index flaw (§1), the rest is folded in below.

## 1. Facts

- Spike (2026-11-10, evaluator side only, throwaway): integer `0x10000000 + pid * 0x1000000 + offset`. Edges taken, Lua + zstd linked, 24
  calls: seed 1234 6601 -> 6730 (+129, run `2026-11-10-addrspike-s1234`), seed 7 6575 -> 6688 (+113, `2026-11-10-addrspike-s7`). Agreement
  with gcc was not tested, and **the spike's model could not agree**: `pid` is the object id (`ev_new_obj`, `eval_run.c:848`), shared with the
  function's own locals and mallocs, while gcc's blob `i` is the *call-line order* (`sweep_rt.h:74-79`). The model needs the blob index
  (`lazy_index`, `eval_run.c:820`, `Lazy.bl`), which `convert` does not see. The gain is therefore an upper bound.
- gcc layout today: blob `i` at the running sum of `(size + SW_SLACK + 16 + 15) & ~15` in a `calloc` arena, so its address depends on the
  final sizes of earlier blobs and on `calloc` (varies per run). The evaluator cannot know final sizes at convert time.
- Blob count is capped at `LMAXBL` = 32 (`eval_run.c:670`); a blob is at most `EV_LAZY_CAP` = 65536 bytes.
- Non-lazy objects (own mallocs, locals, globals) have no gcc counterpart address; only lazy blobs are placed by the sweep on both sides.
- `lib_oracle.sh:21` runs under `ulimit -S -v` (8 GB + 4.4 GB per file); `SW_FORK` runs each call in a forked child with a 3 s alarm
  (`sweep_rt.h:176-195`). `asm_exec.py` only includes `sweep_rt.h` (line 80) and links without `--wrap`.
- `%p` prints `(pid<<16)+off` (`eval_libc.c:306-318`); pointer compare and subtraction use `pid()`/offset (`eval.c:628-653`). Neither is touched.

## 2. Design (a0: a fixed address model for lazy blobs, shared by both sides)

1. **Model.** `addr(k, off) = SW_BASE + k * SW_STRIDE + off`, `k` the blob's index in the call line. `SW_BASE` (0x80000000, 2 GiB: below 4 GiB the pid field is 0, which no evaluator pointer has, so the range is told apart exactly; see §6 for the two bases tried before), `SW_STRIDE` (16 MiB: a power
   of two far above `EV_LAZY_CAP + SW_SLACK + 16`, so `p & mask` agrees for every mask below it) live in one header, `src/c/sw_addr.h`,
   included by `eval.c` and `tests/sweep_rt.h`. Reservation: 32 x 16 MiB = 512 MiB virtual, nothing touched.
2. **Evaluator.** `Obj.lazy` (9 uses) changes from `bool` to `uint32_t` holding the blob index + 1 (0: not lazy), set in `lazy_new`: one field, no
   second one, no churn at the other uses. `convert`: a pointer into a lazy object becomes
   `addr(lidx, off)`; a pointer into any other object keeps the `UNSUP`. The integer is concrete (no shadow, no solver term).
3. **gcc side.** `sw_load` maps one range `[SW_BASE, SW_BASE + n * SW_STRIDE)` with `MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE`,
   checks that `mmap` returned exactly `SW_BASE` (older kernels treat the flag as a hint), and puts blob `i` at `SW_BASE + i * SW_STRIDE`.
   `sw_alo`/`sw_ahi` are the range (the free/realloc wrappers keep working). `sw_out` `munmap`s the range instead of `free(g->arena)`. `SW_SLACK`
   stays (it bounds what `sw_tag` accepts); the arena's per-blob padding goes. A taken range `abort()`s: that is `!` (crashed)
   in library mode and a visible failure otherwise, caught by the "0 gcc crashed" bar; no new skip token (a PIE driver has nothing there, §5; the non-PIE heap starts below 0x3f8ee000).
4. **An integer cast back to a pointer** (`(T *)((uintptr_t)p & ~63)`, `(T *)(a + n)`): a model address with a nonzero pid field (the first two bases, §6) would name a real object, so
   it would silently name object 2, not trap. Under i1, `ev_access` stops (`UNSUP`) on a pointer value in
   `[SW_BASE, SW_BASE + 32 * SW_STRIDE)`. Punning through memory (`memcpy`, a union) bypasses it (not scanned in the corpus, §5). See Decide (i).
5. **No format change.** Call line, draws and trap records are untouched, so recorded replays stay valid. Calls that used to stop on
   `UNSUP` now run, so their *results* in a replay differ, their draws do not.

**Cannot cover.** Addresses of non-lazy objects; `%p`; a result that depends on a lazy address used as an array index or loop bound (gcc may
SIGSEGV or hit the 3 s alarm where the evaluator traps or exhausts its step budget: both are sorted as "differs/crashed" by the first sweep,
not predicted here).

## 3. Decide

**Decide (placement of the model)**

- p1 **Shared header, fixed range on gcc** (recommended): items 1-5.
    - Pro: one definition; the free/realloc wrappers unchanged; 512 MiB virtual fits `ulimit -v`.
    - Con: a taken range skips the call; needs Linux 4.17+ (WSL has it).
- p2 **Index in the high word, no header** (the fewer-concepts reviewer's shape): blob `k` at `(k+1) << 32`.
    - Pro: no constant to share, it is the pointer form already in use.
    - Con: 32 x 4 GiB = 128 GiB virtual breaks `ulimit -S -v` in `lib_oracle.sh`; `p & mask` for masks of 2^32 and above would also agree but is never drawn.
- p3 **Park**: keep `UNSUP`.
    - Pro: nothing to maintain.
    - Con: leaves up to ~110-130 edges (unverified upper bound).

**Decide (integer cast back to a pointer)**

- i1 **UNSUP on a model-range integer used as a pointer** (recommended): a pointer built from an integer in `[SW_BASE, SW_BASE + 32 * SW_STRIDE)` stops, as it does today for any address.
    - Pro: small; no false differences; leaves the idiom `p & ~63` as a stop.
    - Con: that idiom (alignment) is common, so part of the gain stays lost.
- i2 **Map back to a pointer**: the inverse of §2.2 (index to pid through `Lazy.bl`, offset kept).
    - Pro: the alignment idiom runs.
    - Con: a second mapping and an index-to-pid table; reaches into `Lazy` from the pointer paths.

Searched: dropping the stride (running sum): not computable at convert time; dropping the header (p2): breaks the address-space limit; dropping the
`Obj` field: pid is not the blob index, so the index must be stored or the call line changed (a format change, rejected); keeping `calloc` aligned:
matches only low bits, not `(uintptr_t)p` or a hash. Removed from the first draft: the "pid = blob" assumption, the `valgrind` remark (no valgrind
use found in the lazy scripts), the `sw_tag` zero-mask remark (`sw_tag` does no masking; `sw_out` zeroes only link and zero cells, so a stored model
address is compared byte for byte already).

## 4. End-state fit, checks and exit

- **Where:** `sw_addr.h` (new), `Obj.lidx` set in `lazy_new` (`eval_run.c`), `convert` and the pointer-from-integer stop in `eval.c`, `sw_load`/`sw_out`
  in `sweep_rt.h`. **Replaces:** the `calloc` arena, `UNSUP "address as an integer"` for lazy pointers. **Deletes:** the arena padding arithmetic.
- Fixture `tests/cfg_lazy/lazy_addr.c`: `la_align` (`(unsigned long)p & 63`), `la_diff` (`(unsigned long)q - (unsigned long)p` over two
  blobs), `la_hash` (`(unsigned long)p >> 4` stored in a field), `la_back` (cast back, stops under i1); `eval_lazy.sh` (calls finish) and
  `eval_sweep.sh` (agrees with gcc, 0 differ).
- Gain must reproduce: seed 1234 >= 6601 + 60, seed 7 >= 6575 + 60 (lowered from the spike because the spike's model was unsound and i1 keeps the
  cast-back stop); otherwise report and ask.
- Gate, `lib_oracle.sh zstd 8` (0 differ, 0 gcc crashed; new differences from integers used as indices are findings, not noise), replay,
  `asm_exec.py` links, a second call in one non-fork process (the `munmap`).

## 5. Survey results (2026-11-10, read-only, for the j choice)

- **Cast-back sites** (clang AST of the sweep's Lua 5.4 and zstd 1.5.7 sources): 2 integer-to-pointer casts in all of it. Lua `lapi.c:478` is a C
  function pointer (never a lazy blob); zstd `zstd_cwksp.h:541` has no caller. The other pointer-to-integer sites are alignment/mask tests
  (zstd, 12, all feeding `ptr + add` / `ptr - rem` on the original pointer) and hashes (Lua, 4: `luaS_new`, `hashpointer`). So i2 buys about
  nothing on these two projects (inference from source; `memcpy`/union punning not scanned). Of the +129 at seed 1234, 97 edges are in the
  functions the baseline names under this stop (Lua 43: `lua_pushstring`, `auxgetstr`, `luaL_newstate`; zstd 54: `HIST_count_wksp`,
  `HUF_alignUpWorkspace`, `HUF_*CTable_wksp`); the rest is downstream, unverified.
- **Address space** (WSL2 kernel 6.18, `MAP_FIXED_NOREPLACE` works): the driver links PIE. A non-PIE heap start lands in `[0x10000000, 0x30000000)`
  in about half of 300 runs, so `SW_BASE = 0x10000000` is unsafe; **`SW_BASE = 0x200000000`** is free in both layouts. `ulimit -S -v` counts
  `MAP_NORESERVE`: 512 MiB mapped fine under the `lib_oracle.sh` limit and cost 512 MiB of it, 128 GiB failed (p2 is out).
- **Solver terms:** symbolic pointers use the same `pid << 32 | off` form (`eval.c:1567`, `eval_run.c:798`, `:989`); the converted integer must stay
  concrete at those sites. Not checked: qsort/bsearch handlers inside the swept library code.
- **`Obj.lidx`:** no blob-index field exists; `Lazy.bl` is file-static in `eval_run.c`. By hand count `Obj` is 68 bytes in 72, so a `uint32_t`
  should fit the tail padding (compile `sizeof(Obj)` to confirm). The alternative is a callback through the existing `lazy_fill` hook.

## 6. Result (2026-11-10, built: l1 = p1 + i1; plan `docs/LAZY_ADDR_PLAN.md`)

Built as specified except the base, which took three tries: `0x200000000` (spec: pid field 2 names a real object), `0x400000000000` (plan review: pid field 16384;
sweep `2026-11-10-addr-s1234`: 6633 edges, +32, and 82 Lua edges lost to "read/write through a model address" because a real object has id 16384 in a call
that makes thousands of objects), then **`0x80000000`**: below 4 GiB the pid field is 0, which no evaluator pointer has, so `MODEL_ADDR(p)` (`sw_addr.h`) tells
the range apart exactly. `Obj.lazy` is a `uint32_t` (blob index + 1); `ev_access` stops on a model address used as a pointer; `compare` stops on a model address
against a real pointer (final review, found by reading); `sw_load` maps the range with `MAP_FIXED_NOREPLACE` and aborts if it does not get it.

Lua + zstd linked, 24 calls: seed 1234 6601 -> 6702 (+101, run `2026-11-10-addr2-s1234`), seed 7 6575 -> 6669 (+94, `2026-11-10-addr2-s7`); the exit bar (+60 each) held. The
"address as an integer" stop is down to Lua 11 edges (`luaL_newstate`, `lua_resume`, `luaT_init`) and zstd 8 (`ZSTD_createCDict`, a pointer into the function's own block).
Replay equal (`CEREAL_LAZY_REPLAY=1` over `eval_lazy.sh`); gate 2406 passed (`2026-11-10-addr2-gate`); `lib_oracle.sh zstd 8` (`2026-11-10-addr3-liboracle`): 46317 calls
finished, 46317 equal, 0 differ, 0 gcc crashed (45321 before). The two seed sweeps ran before the `compare` stop; the gate and `lib_oracle` ran after it.

Not covered: a model address passed to `free`/`realloc` (the evaluator traps, gcc's wrapper ignores it); a model address reaching a pointer through a path that skips
`ev_access`, `compare` and pointer subtraction (none found by reading); a `MAP_FIXED_NOREPLACE` macro on a gcc without it (the driver is built with gnu99 or later);
addresses of non-lazy objects; map-back of an integer to a pointer (i2: Lua and zstd have no such cast on a path the sweep reaches, §5).

### 6.1 After the build (2026-11-10)

Deferred minors fixed (`la_re` RED 88 finished / 0 stops, then green): `realloc` of a model address stops (`eval_libc.c`, it was treated as NULL while gcc's wrapper returns it unchanged); `sweep_rt.h` guards `MAP_ANONYMOUS` and `MAP_FIXED_NOREPLACE` and says why it aborts; `la_neg` and `la_end` pin a negative offset and a pointer past a blob's end (equal to gcc). `free` of a model address needed nothing: both sides ignore it. Gate 2406 / 2405 sanitizer (`2026-11-10-addr4-gate`), `lib_oracle` zstd 46317 equal, 0 differ (`2026-11-10-addr4-liboracle`).

Surveys, both read-only, both **parked** (`PARKED.md` row 248):
- **Map-back (i2)**: 0 model-address stops in the two final seed sweeps; Lua and zstd have one integer-to-pointer cast between them (`zstd_cwksp.h:541`, no caller) and a function-pointer one. Smaller shape if ever built: decode in `convert` (blob index and offset from the integer), which would delete the `ev_access` and `compare` stops. Trigger: a model-address stop in a sweep or `lib_oracle` run on a new library. Static grep only (memcpy and union punning not scanned). The `compare` stop was added after the seed sweeps, so its count is in no log.
- **Addresses for non-lazy objects**: at most 19 of about 1100 untaken edges. Lua's 11 (`luai_makeseed` hashes addresses with `time(NULL)`) cannot match gcc at all; zstd's 8 are alignment arithmetic on its own block (`zstd_cwksp.h:265,279`), coverable only by a second range plus gcc-side malloc wrapping. An opaque integer that stops on any other use needs a shadow type on integers: rejected. Trigger: a library whose alignment checks on its own blocks dominate its stop list.
- Gap both found: `lib_oracle` prints no per-reason stop count, so its 582 stops are unattributed.
