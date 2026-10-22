---
status: draft
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
   `Fn` bodies point into them), the sweep runs once after the last file (`eval_sweep_all`, replacing the per
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
