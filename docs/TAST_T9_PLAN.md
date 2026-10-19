---
status: draft
---

# TAST stage T9: plan (initializers and aggregates to explicit stores)

T8 gave every function a control-flow graph whose blocks still hold the
structured form's `local` (with `elem` kids), aggregate-typed values and
`call` nodes. A back end cannot take those. T9 removes the last constructs
that are not a load, a store or an arithmetic node; ABI classification stays
a later stage (T10). Grounded on a `--dump-cfg` of a small scratch file (struct, array and string initializers, aggregate assignment, a call with a struct argument), 2026-10-18; the file was not kept.

## 1. What the graph holds today

From the dump: `local p #2 #4 #6 struct P` with `elem +0 #1`, `elem +4 #3`,
`elem +8 #5` as kids; `local a #10 #12 int[4]` (two elements for four, the
rest implicitly zero); `local q #15 #17` where `#15` is `elem +0 (str ..)`
for a `char[3]` taken from a string; `local r #20` where `#20` is a struct
`load`; `store r (call mk)` and `call take (load r) ..` with struct values;
member access as `field +k` over a `sym`, array indexing as `addr`/`ptradd`/
`deref`.

So three things are not yet core: (a) initializer lists (`elem`, implicit
zero fill, `str` into arrays); (b) values of aggregate type (`load` of a
struct, struct as a `store` source, call argument and call result); (c) the
`data` statements are static data and stay.

## 2. Form

Two new statement ops. `copy dst src N` (`dst` and `src` are address-typed
values, `N` the byte count, no overlap) and `fill dst byte N`; no other new
node. The rules:
- `local x e1..en` with `elem` kids becomes `local x` (no init) followed by,
  in order: a zero fill of the object when the initializer does not cover it
  (`fill dst 0 N`, below), then one `store (field/ptradd lvalue) v` per
  `elem`. A `str` element is a `copy` from the string's data with the
  remaining bytes zeroed. Scalar `local x v` keeps its init (it is already
  one store).
- A struct or array `load` used as a value is replaced where it is consumed:
  `store dst (load src)` becomes `copy (addr dst) (addr src) N`; a call
  argument of aggregate type stays a `load` here (T10 decides how it is
  passed); a call result stored to an lvalue becomes `call` with a hidden
  destination, again T10. Until T10, T9 leaves aggregate `call` operands
  and results alone, and its verifier only forbids aggregate values
  *elsewhere*.
- `fill` is for zeroing (because a
  large `= {0}` must not become a thousand stores).
- Order: C leaves the evaluation order of initializer elements
  indeterminately sequenced; keep source order, as the structured form does.

Spill/`reg` interplay: an initializer element that contains a split is
already lowered by T8b before T9 sees it (the `elem` kids are values, so
they are in place by then). T9 runs on the T8 graph, block by block, and
never changes a terminator.

## 3. Verifier additions (`--verify-cfg`)

- No `elem` node remains; no `local` has kids other than one scalar init.
- Every aggregate-typed value is a `load` consumed by a `call` operand, a
  `ret`, or a `copy` source (the T10 boundary); nowhere else.
- `copy`/`fill` sizes equal the object size on both sides and stay inside
  the object (the layout the checker already computed).

## 4. Check against execution

Section 5 of the T8 plan models one cell per object and byte offset, which
cannot see a `copy` that moves bytes or a zero fill that covers padding. T9
therefore needs the oracle's byte-level memory (the parked row "Execution
oracle for floating point, aggregate copies, ..."): objects become byte
arrays, integers are stored little-endian at their offset, a read compares
the bytes written. Then `tests/cfg_exec/` gains a struct/array
initializer file compared with gcc, including partially initialized
objects and nested aggregates. Pointer provenance stays one object, as
now. Floating point stays out (it needs the byte encoding of doubles; the
interpreter could use `struct` packing, left for a later round).

## 5. Order and gates

1. Byte-level memory in `cfg_exec.py` first, on the existing T8 files
   (their results must not change): a refactor with a check already in
   place.
2. Lower `local` initializers (scalars unchanged, lists to stores/`fill`,
   `str` to `copy`); goldens in `tests/cfg/` regenerated; oracle files.
3. Aggregate assignment to `copy`; the T10 boundary verifier rule.
4. Corpus: lua54 and libuv lower with 0 findings and 0 unsupported; count
   `copy`/`fill` nodes and the largest `fill` (spike below).
5. Gate before a tag, as for T8.

**Spike first (read-only, cheap):** count, over lua54 and libuv, the local
initializer lists by size and the aggregate assignments, to see whether
`fill` is needed at all or whether stores alone would do (an array of 4096
zeros decides it).

**Spike result (2026-10-18, throwaway script over the dumps of the T8b
corpus run; not kept).** Counts of `local`s and aggregate operations in the
lowered graphs:

| | lua54 | libuv |
|---|---|---|
| locals without init | 1492 | 1260 |
| scalar init | 3474 | 89 |
| initializer lists (all `elem` kids) | 0 | 3 (1 array of 2, 2 structs), at most 2 elems |
| aggregate `store` / `load` / call returning one | 0 / 0 / 0 | 33 / 65 / 6 |

So in these two corpora the initializer lowering is almost unexercised (3
lists, none with an implicit zero tail), `fill` has no corpus case at all,
and the volume is in aggregate assignment and aggregate loads (all libuv).
Consequences: (a) `fill` can be dropped from the first cut and added when
a unit needs it (the repo's own tests keep list initializers covered, see
`tests/tast/`); (b) the corpora cannot be the evidence for initializer
lowering, so the oracle's struct/array files carry that burden; (c) the
risk sits in step 3 (aggregates), not step 2. The script counted a `local`
as a list only when every kid is an `elem`, and a type as aggregate when it
names `struct`/`union` or ends in `[N]` and is not a pointer; types such as
`char[]` of unknown length or a typedef hiding a struct are read from the
printed type, which the dump expands, so they are counted; the numbers are
otherwise unverified by a second method.

## 6. What is out

- ABI classification of arguments and results (T10); `va_arg` and
  intrinsics lowering; `setjmp`.
- Static data (`data` stays as T3 produces it); VLAs; floating-point
  byte encoding in the oracle.
- Any optimization (store merging, redundant zero elimination).

## 7. Decisions needed

1. Direction: this plan continues the back-end path (T9 initializers, T10
   SysV classification, T11 emission, checked by running compiled
   lua54 tests against a gcc build). The alternative consumer of the T8
   graph is the dataflow diagnostics of ROADMAP Track D
   (`-Wuninitialized`, `-Wimplicit-fallthrough`, `-Wnull-dereference`),
   which needs no T9. **Recommendation:** T9, because the graph is useful
   to the back end only after it, and the diagnostics can be built on the T8
   graph at any time without waiting. Pro: one path to a runnable
   artifact. Con: ~weeks before anything user-visible.
2. `copy`/`fill` as statement ops versus expanding every aggregate
   operation into stores. **Recommendation:** statement ops (one concept
   each, small output, easy for QBE `blit`); expansion would multiply
   nodes for big objects.
3. Whether `str` initialization of arrays uses `copy` from a `data` string
   or per-byte stores. **Recommendation:** `copy`.

4. Back end target (decided 2026-10-18, by the user): keep the emitter
   open to several back ends over the same lowered graph; lean toward
   direct object/machine code. QBE is one candidate, not assumed. T9 does
   not depend on it; revisit at T10 (classification is where a back end's own
   ABI handling would overlap).

## 8. Review log

- **Round 1 (self).** Caught: T8's oracle cannot check `copy`/`fill` (so a
  byte memory is step 1, not an afterthought); `fill` may be unnecessary
  (spike decides); aggregate call operands cannot be settled before T10,
  so T9's verifier rule is a boundary, not a ban.
- **Round 2.** Checked the claim "T9 never changes a terminator": the
  `local` rewrite inserts statements in a block only, so true; but a
  `switch` jumping past a `local` (Duff) must not skip a *zero fill* that
  C requires only when the declaration is reached: C says the object is
  indeterminate after such a jump, so no fill is needed there; verifier
  need not model it. Open: whether `str` can contain embedded zero bytes
  (the dump holds `616200`, bytes explicit, so `copy` size is safe).
- **Round 3.** Nothing material beyond the spike and the oracle's byte
  memory; the dump in section 1 was read, not assumed, but no TAST source
  was changed or run for T9 yet, so every claim about *how* it lowers is
  untested.
