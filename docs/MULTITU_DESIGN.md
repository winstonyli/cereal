---
status: done
---

# Evaluating across translation units (`--eval-link`)

Goal: `cereal check --eval-sweep=N,SEED --eval-link a.c b.c ...` sweeps the functions of every file with
the callees defined in the other files available, so a call into another file runs its body instead of
stopping ("call of X, which has no body here"; about 11000 of 114096 stops on zstd and Lua, and a measured
ceiling of +28% edges for Lua, ~0 for zstd: `EVAL_DESIGN.md`, "Multi-TU ceiling"). The gcc oracle links the
same files, so every call is still compared.

Settled (the user's choice, 2026-10-22): one shared `Ev` for all files, each function remembering its own
checker. Not chosen: a unity build (static names clash), stubbed callees (the oracle cannot link them).

## 1. What is per file today

A file's `Checker` owns the type table (`TypeId`), the symbol table (`ref`, `fsym`, `SYM_LOCAL` ids) and the
interned names. A retained function (`Fn`) is a tree of `TNode`s whose `ty` and `sym` fields index those
tables, so a body can only run with its own checker. The evaluator reaches the tables through `v->c` (26
uses in `eval.c`, `eval_libc.c`, `eval_run.c`). Also keyed by a per-file number: `find_fn(v, fsym)`, `Sd.ref`
(static-storage objects), `Obj.fn` (a function's object stores `fsym + 1`), `Obj.ref`/`fname`/`sd1`.
What is already global: the object store (`Ev.obj`; a pointer is an object id and an offset), so scalars and
pointers cross files unchanged.

## 2. Design

1. **The frame's checker.** `Fn` gets `Checker *c`. `run_fn` sets `v->c = f->c` on entry and restores the
   caller's on exit, so every existing `v->c` use is right without an edit. (Rejected: threading a checker
   through every helper: 26 sites and every later one.)
2. **One identity per function and per datum.** `Obj.fn` stores the index into `Ev.fn` (global) instead of an
   `fsym`; `find_fn(v, fsym)` matches `(v->c, fsym)`; `Sd` gets `Checker *c` and is matched on `(c, ref)`.
   Statics stay per file by construction.
3. **Linking by name.** A symbol with external linkage that has no definition in the current file resolves,
   at its first use, to the definition of that name in another added file: functions to their `Fn`, objects
   to their `Sd`; cached per `(c, ref)`. Two definitions of one name (a conflict gcc would reject) stop the
   sweep with an error. A tentative definition and an `extern` declaration share the defining file's object.
4. **Lifetime and driver.** Today `frontend_run` frees each file's checker after `checker_finish`. With
   `--eval-link` the files run in one process, one after the other, each file's `tu` and checker kept (the
   `Fn` bodies point into them), the sweep runs once after the last file (built as `eval_sweep` on the first file's checker, replacing the per
   file call from `eval_finish`), and everything is freed at the end. Without the flag nothing changes.
5. **Records by value across files.** Argument and result bytes are copied as bytes. The two files' record
   types must have equal sizes (checked at the call; unequal: unsupported). Field offsets are assumed equal,
   as they are when both include the same header.
6. **The oracle.** Per file: gcc compiles the file with its thunks (as `eval_sweep.sh` does now, the thunk
   table made non-static and extern-declared) into an object; all objects are linked with one driver `main`
   (a file defining `main` is left out of the link and its functions are not swept). The sweep prints a
   `file PATH` line before each file's `fn` lines so the script can split them.

## 3. Checks

`tests/cfg_exec/link_a.c`, `link_b.c`: a function in `a` calling one in `b` that has a guarded branch, a
shared global, a static of the same name in both files, a function pointer from `a` called in `b`, a record
passed by value. Results must equal gcc's (the linked build). `tests/eval_sweep.sh` with several files runs
them as one link. The corpus: Lua 5.4 and zstd as one link each, with `sweep_meas.sh` (edges with and without
the link).

## 4. Review log

Round 1 (against the code): (a) `Obj.fn` holds an `fsym`: a function pointer made in file A and called in B
would be looked up in B's numbering: item 2 changes it to the `Ev.fn` index. (b) `Sd`, labels and string
objects: labels are keyed by `Ev.fn` index already; string objects per `Fn`; `Sd` needs `c`. (c) `ev_of(c)`
makes one `Ev` per checker and the checker hooks (`ev_add_func`, `ev_add_data`) fill it while the file is
checked: with the flag `ev_of` must return the one shared `Ev` and set `v->c` to the file being added.
(d) The frame switch (item 1) must also cover the host library's callbacks and `ev_object`, which run
outside `run_fn`: they read `v->c` from the file that asked, so the entry points from the checker set it.
(e) Typedef'd record types with the same name in two files are different `TypeId`s: harmless, since nothing
compares ids across files (item 5 compares sizes).
Round 2: (f) cost of keeping every checker alive: Lua 5.4 (33 files) and zstd (40) are small; stated as a
limit for large programs. (g) A function in file B called from A before B was added: resolution happens at
the sweep, after all files are in. (h) The checker's own use of the evaluator during checking (constant
folding, `tast.c:1345`) runs while only the files so far are added and must keep working with a single file:
unchanged by the flag.

## 5. Result (2026-10-22; built on `6fe8f35`, gate row `multitu-link` in `docs/runs.md`)

As built (differences from section 2):
- `Fn.c`, `Sd.c`, `Obj.fc`, `Fr.up_c`; `run_fn` switches `v->c` and restores it. Function objects hold
  `(file, symbol)` rather than an `Ev.fn` index (the same identity, no table lookup per call).
- **A hash index of the functions** by `(file, symbol)` and, for external definitions, by name (`fn_sync`,
  `find_fn`, `find_def` in `eval.c`). `find_fn` was a linear scan, too slow once one `Ev` holds the 3000
  functions of a library. A name defined twice is `dup_def`; the link stops with exit 2 ("multiple
  definition"). Not entered by name: functions of system headers and `inline` ones (every file has its own
  copy: the `<emmintrin.h>` intrinsics made zstd report duplicates).
- `function_obj` canonicalizes to the defining file's object, so a function pointer made in one file and
  compared or called in another is the same object. A declaration with no definition anywhere (a host
  function) stays per file.
- Objects: `static_obj` resolves a global without a definition here, or with a tentative one, to the best
  definition of the name (defined, else the first tentative one) and caches it in the declaration's
  `Sd.alias`. `sd_obj` sizes an object in its defining file's checker.
- **`struct EvLink`** (`eval.h`) holds the shared `Ev`, the kept checkers and the callers' sources; reached
  through `CheckOptions.link` / `FrontendOpts.link`. The first version used three file-scope variables and
  the gate's globals check (`bench/tools/globals.sh`) rejected them.
- Driver: `--eval-link` (needs `--eval-sweep`, plain file arguments; forces one job). `parse_one` keeps each
  file's `TU` on the heap, `frontend_run` keeps its checker, `eval_finish` queues it, and `evlink_end` sweeps
  once after the last file, then frees checkers, `Ev` and sources. The sweep prints `file PATH` before each
  file's functions. The tentative definitions are registered when the file ends, so the sweep (now after all
  files) sees them: a `int x;` is a real object, not a stub.
- Records by value: `link_sizes_ok` compares argument and result sizes on a call across files ("unsupported"
  when they differ). Field offsets are assumed equal.
- Oracle (`tests/eval_sweep.sh`, `EVAL_SWEEP_LINK=1`): one driver per file (the file, its thunks, a table
  named `N:name`: statics may share names), a generated `main` merging the tables; a file without a swept
  function is compiled as it is; every file's `main` is renamed.

Checks: `tests/cfg_link/link_a.c`, `link_b.c` (a cross-file call into a guarded branch, same-named statics, a
function pointer from `a` called in `b`, a record by value both ways, a global defined in `a` and read in
both) equal gcc's link, 11 of 11 functions, with and without the solver; stage 13e1 of `tests/run.sh` also
requires the guarded call (`12345`) among the printed ones. One file at a time nothing changes (46 eval tests,
the cfg_exec sweeps with the solver equal to gcc). Gate: 2313 passed in both builds.

Corpus (Lua 5.4 without `onelua.c` and `ltests.c`, zstd without its assembly, N=24 with the solver; the lists
differ from the earlier rows, so those numbers do not carry over): file by file 3799 of 8207 edges taken
(46.3%), 532 queries; as one program per library 4341 of 8655 (50.2%), 673 queries (runs
`sweep-base-nolink-24`, `sweep-link-24-2`). +542 edges taken (+14%) and 448 more edges in the population
(functions that stopped on a missing callee now finish). The ceiling measured on amalgamations was +28% for Lua,
about 0 for zstd.

Negative: a library-scale gcc oracle (Lua and zstd linked as one program, calls compared) does not work: the
link builds, but gcc segfaults on generated pointer state (unrelated objects, zeroed contexts; `lua_settop`
after 171 of 9218 calls, zstd after 16149 of 25430), which the evaluator survives or traps on. Single-file Lua
cannot even be linked. The oracle stays on `tests/cfg_exec` and `tests/cfg_link`; the libraries are for
measurement. Run rows `link-oracle-n4`, `n4b`, `n4c`; the script was deleted.

Update 2026-10-24 (lazy blobs): the library oracle works with a forked child per gcc call
(`bench/tools/lib_oracle.sh`, `EVAL_SWEEP_LIB=1` in `tests/eval_sweep.sh`; `--eval-state`, library
linked as one program; the program's own stdout is discarded, since Lua's printing shifted the result lines).
zstd N=24 (`liboracle-lazy-zstd`): 72387 calls the evaluator finished, 72245 equal, 142 differ, 0 gcc crashes;
the differences are `ZSTD_cwksp_bytes_to_align_ptr` (the alignment of a raw address) and `ZSTD_DCtx_get_bmi2`
(`cpuid`). Lua N=1 (`liboracle-lazy-lua-res`): 46428 calls, 41942 equal (90.3%), 1304 differ, 3182 gcc crashed
(`__st` state functions and plain ones); not yet classified, so a mismatch there is not yet a finding.
Cause of most of it: the state builders that yield no state (`getco` aborts, `lua_tothread` returns NULL) still
ran in the evaluator and changed the main state, while the gcc side replays only the builders that gave a state;
`state_run` now undoes a builder that gives none (`snap_restore`). Lua N=1 after that (`liboracle-lazy-lua-undo`):
45267 calls, 44054 equal (97.3%), 229 differ, 984 gcc crashed. Left: `FILE*` and io-library functions (a lazy blob
as a stdio handle), string interning, and cells where the program stored a pointer (the evaluator holds an id, gcc
an address), uninitialized memory (gcc 0xaa fill, evaluator zero), `lua_absindex` on a drawn state.
Perturb check (same day, kept files of `liboracle-lazy-lua-undo`, gcc driver run twice with `MALLOC_PERTURB_` 1 and 254):
of the 229 differences only 11 depend on uninitialised heap (`boolF`, `open_func`, `luaL_addlstring`, `auxgetinfo`,
`lua_rawsetp`, `luaD_pretailcall`); 218 are stable, 195 of them `io_tmpfile` (a real file, host-dependent by design) and
about 23 others to look at one by one (`lua_absindex` x10, `getS`/`getF`, `stringK` with a fixed 0xaa fill,
`luaH_newkey`, `lua_xmove`, `luaD_seterrorobj`). A mask of never-written bytes would remove at most 11, so it is not built.
Pointer difference (same day): a difference that is not a multiple of the element size is undefined; gcc -O0 shifts
(floor) and the evaluator divided (truncate), which gave the 10 `lua_absindex` differences (the sweep draws offsets
in multiples of 8, `StackValue` is 16 bytes). `TO_PTRDIFF` now stops such a call. Lua N=1 oracle (`ptrdiff-liboracle-lua`):
45194 calls, 43996 equal, 216 differ, 982 gcc crashed. Cost, lazy with solver N=24 (`ptrdiff-solver-lazy`): untaken Lua
1551 -> 1575, zstd 1682, total 3233 -> 3257 (+24, 0.7%); still below eager 3897.
The rest of the stable differences (same day, decoded from the kept files): `lua_xmove` and `luaD_seterrorobj` are
calls whose two `lua_State*` parameters alias or overlap (the drawn objects share a blob), where the result depends on
copy order; `stringK` reads the uninitialised padding of a local `TValue` (gcc -O0 leaves 0xaa, the evaluator zero);
`luaH_newkey` x2 reads garbage; `luaF_getlocalname` x1 (evaluator NULL, gcc an unknown pointer) is unexplained. None
is an evaluator bug that a fix would remove, so the oracle gate for zstd allowlists only the two machine-reading
functions and Lua stays a measurement.
Stage 3 (function pointers drawn as functions, `ff63c5d`): solver sweep with lazy (`fnptr-solver-lazy`) Lua 1575 -> 1531
untaken, zstd 1682, total 3213; Lua library oracle N=1 (`fnptr-liboracle-lua`) 45621 calls, 44458 equal (97.5%), 217 differ,
946 gcc crashed (982 before; the strcmp-in-`luaS_new` class shrank), 2 evaluator stops. The new differences are few and
look like pointer-dependent values (`luaZ_fill` 0x89 vs 0xc9, `inclinenumber`, `getS`/`getF`/`luaF_getlocalname` returning
NULL where gcc returns a pointer outside the blobs); not yet looked at one by one.
Stage 4 and after (2026-10-25, `ac00c6a`): Lua oracle N=1 (`final-liboracle-lua`) 45531 calls, 44383 equal, 211 differ, 937 gcc
crashed, 2 stops (stage 3: 217 / 946). Two false classes found by reading the differences and fixed: the tag of a forged
pointer (null + 24 is not NULL; `getS`, `getF`, `luaF_getlocalname`), and pointer order across different objects
(`getfreepos`), which now stops like a subtraction and retries with the blobs shared. The compare trap costs coverage:
solver sweep `final-solver-lazy` Lua 1540 + zstd 1760 = 3300 untaken (3213 before), 5209 edges taken (5288); the new
reference. Left, not evaluator bugs as far as read: `boolF` (gcc leaks stack addresses), `lua_xmove`/`luaD_seterrorobj`
(overlapping aliased `lua_State` parameters), `io_tmpfile` (`FILE*`), `luaH_newkey` (garbage); not yet read one by
one: `luaZ_fill`, `addk`, `open_func`, `inclinenumber`, `lua_rawsetp`, `f_call`, `auxgetinfo`. `d1e07af` (stop on a
pointer difference that is not a multiple of the element size) stays: undefined behaviour, and it removed the
`lua_absindex` differences. The 7 call-through-non-function stops (`luaM_saferealloc_` x2, `luaU_undump` x4,
`close_func` x1) come from the sharing draw: with `LHEAD` 128 a shared pointer lands on `LHEAD + 8k`, so `g` can point at
`L`'s `l_G` slot (field 24, offset 152) and `g->frealloc` then reads a data pointer. Inferred from offsets, not
reproduced; a fix is parked. The 4096-byte limit was dropped later (2026-10-26, `nolim2-*`): oracles unchanged, 5790 of 11120 edges taken.
Address-dependence (2026-10-25, `addr-gate2`): two stops replace per-name allowlisting. A pointer the program stores into a drawn
object is marked as a pointer (init 2, as `lazy_fill` marks the ones it draws), so reading its bytes through a `char` stops
(`luaZ_fill` read `*buff` where `buff` pointed at the Zio's own `p`; fixture `tests/cfg_lazy/lazy_ptrbyte.c`, checked by
`tests/eval_lazy.sh`); and converting a non-NULL pointer to an integer stops ("address as an integer"). Lua oracle N=1
(`addr-liboracle-lua`): 37964 calls, 9 differ (211), 336 gcc crashed (937); zstd (`addr-liboracle-zstd`) 6 differ, all
`ZSTD_DCtx_get_bmi2`. Cost: solver sweep (`addr-solver-lazy`) Lua 1579 + zstd 1852 = 3431 untaken (3300), 5118 edges
taken (5209). Left in Lua: `boolF` x6 (gcc leaks stack addresses), `lua_upvaluejoin`, `lua_setiuservalue`, `getF`
Tried and reverted (2026-10-25, `share-solver-lazy`, `share-stuck`): a share draw that refuses a target overlapping a drawn pointer
or the slot being drawn (new object instead). It cleared 5 of the 7 call-through stops (`luaM_saferealloc_`, `luaU_undump`;
`close_func` stays, now 2 edges, a call through NULL) and the gate was green, but coverage fell: 3472 untaken (3431), 5075
edges taken (5118). The "wrong" shares carry real coverage, so the stops stay as a documented limit.
(a `FILE*`), not read yet.
`lua_setiuservalue` was a false difference (`nullneg-liboracle-lua`): `L->top--` on a NULL top makes NULL - 16, which the evaluator
holds with a zero high half and gcc as 0xfffffffffffffff0; the final-state dump now sign-extends such a stored pointer. Lua
oracle N=1 is 8 differ: `boolF` x6 (gcc leaks stack addresses), `getF` (`FILE*`: `feof` on a drawn blob; its result is a pointer into the blob in the evaluator, NULL in gcc), `lua_upvaluejoin` was the write to a `static const` null pointer that Lua's `getupvalref` returns the address of: gcc crashes, the
evaluator wrote it and leaked the value into later calls (`0x6b77` was a stale pointer id). A const static is now read-only
(`tests/cfg_lazy/lazy_constwrite.c`); Lua oracle N=1 (`constw-liboracle-lua`) 37920 calls, 7 differ (`boolF` x6, `getF`), 294 gcc crashed.
The 852 edges behind `luaV_execute` (`oob-stuck`, trap message now carries offset and sizes): the call stops at a 4-byte read at offset
268435576 (0x10000078) of a 144-byte blob, the code array. Every attempt traps there, so it is the drawn bytecode, not a
bookkeeping error: a random instruction's jump operand moves `pc` far outside the 36 instructions drawn. The solver does not
treat "the pointer stays inside its object" as a constraint, so it cannot steer past it. Parked in `PARKED.md`.
(Corrected 2026-10-28: no jump runs. That stop is `getbaseline` reading `abslineinfo` at an index from an unsigned -1, and most
calls of `luaV_execute` stop earlier or later, at the computed goto of its dispatch, which the evaluator did not support until
2026-10-28; see `PARKED.md`.)
The 982 gcc crashes of `ptrdiff-liboracle-lua` (kept files, one call per class under gdb; function counts from `got.txt`):
744 are `FILE*` functions (`skipcomment`, `skipBOM`, `test_eof`, `read_all`, `read_chars`, `getF`) where glibc
aborts with "invalid stdio handle" on a drawn blob used as a `FILE`; about 150 (`luaS_new`, `lua_pushstring`,
`resume_error`) die in `strcmp` inside `luaS_new` on a drawn string-cache entry; `lua_settop`, `luaD_closeprotected`
and `luaG_errormsg` callers reach `luaD_throw` with no protected call, which calls `abort()` (the program's own
panic path; the evaluator finishes these, so each is a reported difference only if the abort is not also a
stop on its side); 42 `lua_upvaluejoin` fault on a garbage upvalue pointer. So none is a wrong evaluator result: the
gcc side is the one that cannot run the drawn state. A gate leg would need the `FILE*` calls excluded by type.

Limits: a source manager reserves 4 GB of address space per file (`srcmgr.c` `REGION_SIZE`) and a link keeps
every file's, so `ulimit -v` must grow by that per file (`sweep_meas.sh` does); Lua (32 files) and zstd (26)
fit. Compile-database entries are not supported (their options are freed with the job). Static objects are
indexed like the functions (`sd_sync`, `sd_find`, `sd_named`; same corpus numbers as before the index: runs `sweep-link-24-2`, `sweep-link-sdindex`).

Review log: (a) the first `dup_def` check read the field before the index had seen the last files; `ev_dup_def`
syncs first. (b) The system-header `inline` duplicates (above). (c) A gcc build that crashes now names the
call that was running (`tests/eval_sweep.sh`).
