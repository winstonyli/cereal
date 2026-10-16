# Configurations and presence conditions (design)

Status: DRAFT 7, no code. Written Round 239 from the survey of Round 238
(HISTORY) and the user's direction (ROADMAP Track D). Drafts 2 to 7 answer the
adversarial reviews (section 12).

One mechanism that (1) indexes and checks every `#if` group under a
configuration that activates it, (2) explains why a symbol or group is active or
inactive and what would change that, and (3) is the mechanism a later back end
(compilation to a target) and VeriFast-style proving (an annotation view) reuse.

## 1. Goals, non-goals

Goals

- G1. Every declaration, definition and group has a **presence condition** (PC):
  a formula over named facts ("atoms") that holds exactly when the preprocessor
  keeps it.
- G2. Questions are answered from PCs: *why is this inactive*, *what turns it
  on*, *is it ever on*, *what differs between two configurations*.
- G3. A **configuration** is a first-class value: a valuation of the atoms plus
  the non-macro parts of a compilation (language level, extensions, target,
  sysroot, annotation view). A compile-database entry yields one; so does a
  derived "virtual" configuration that activates a group the entry's own does
  not.
- G4. Indexing, checking and rename run per configuration with the existing
  machinery (one `Options`, one unit, one manifest) and merge results by PC.
- G5. Nothing is specific to C conditionals: a back end or a prover supplies its
  own configuration fields; the solver and the queries do not change.

Non-goals

- Variational parsing or a variational preprocessor (TypeChef). Each
  configuration is preprocessed and checked on its own, by the real
  preprocessor. PCs are computed from directives, within stated limits (3.4),
  and **every suggestion a PC produces is verifiable by running the real
  preprocessor** (8, the oracle). That is how this design avoids the cost of
  symbolic execution of the include tree.
- Modeling an arbitrary foreign toolchain (7.3, 6).
- Changing what a normal check or compile does; everything is opt-in.

## 2. What exists (verified in the tree)

| Piece | Where | What it gives | What it lacks |
|---|---|---|---|
| Directive skeleton | `src/skel.[ch]` | every directive of a file, active or not: kind, depth, opener, tokens; define/undef/include positions; the include-guard shape (`guard_ifndef`, `guard_covers_file`) | `#pragma once`, `#include_next`, `__has_include` as structure; cross-file links |
| Condition analysis | `src/analysis/cond.c` | each condition as a tree; per-branch dead/redundant by enumeration over `defined(X)`, integer `X` (domain 0, 1, c-1, c, c+1), opaque calls | 16 variables (`Var vars[16]`), the product of domains at most 2^18 (else `unknown`), `dom[24]`, 64 conjuncts, 32 priors; one file; atoms independent; `#define`s not followed |
| Options | `src/driver.h`; `-D`/`-U` are in `o->pp` | `std_year`, `fingerprint`; built per compile-DB entry by `config_options_entry(cfg, entry_index, ...)` | target, extension set, annotation view; no identity for an entry that is not in the database |
| Host predefines | `tools/probe-host.sh` -> `build/gen/host_config.c` | the host compiler's `-dM` list, include dirs, `__has_*` answers, fixed at build time | any other compiler or target |
| Records, manifests | `src/c/cidxrec.[ch]` (`CREC_FORMAT` 4) | per-file content-addressed records, per-TU manifest keyed by `crec_tukey(main_path, flag_hash, build_id)`; `fingerprint` mixed with the format; differing header records kept as variants (B5 E5) | a record cannot say under which condition an event exists |
| Inactive regions | `ix->inactive`, `inactiveRegions`; `crename.c inactive_use` (a textual word scan of skipped ranges) | the groups this unit skipped | why, and what would change it |
| Project scope | `cereal.index.paths` (project.c) | which files are indexed | n/a (`cereal.index.configs` is new) |

Corpus (HISTORY Round 238, a throwaway script, not a tool in the tree): of 7,231
`#if`-family directives in libuv, Lua, zstd and cereal's `src`, 17% test a
predefined or platform atom. By axis: OS (`_WIN32` 391, `__APPLE__` 122,
`__linux__` 97, BSDs, `_AIX`, `__sun`), compiler (`__GNUC__` 206, `_MSC_VER`
111, `__clang__` 67), `__cplusplus` 101, `__STDC_VERSION__` 46. B5_DESIGN 12.2:
rename refuses 54 of 441 libuv functions for a use in an inactive group.

## 3. The model

### 3.1 Atoms

The BDD variables (section 4) encode only these **initial atoms** (family
leaves, 3.3, are an encoding of the boolean `I` atoms of a family: `I(_WIN32)`
is a function of the leaves); every other fact is a function of them (3.4):

| Kind | Example | Meaning |
|---|---|---|
| `I(X)` | `I(_WIN32)` | X is defined **before the first line of the unit**: by the target's predefines or the command line |
| `CMP(X, op, c)` | `I(X) value >= 3` | the initial value of X; see 3.2 |
| `HAS(kind, name)` | `__has_include(<x.h>)`, `__has_attribute(noreturn)` | a fact about the configuration's sysroot and compiler (3.7) |
| `OPAQUE(text)` | `__GNUC_PREREQ(4, 1)`, arithmetic between macros, a condition this model cannot express | boolean, related to nothing else |

A `#if` condition is parsed (cond.c's parser, reused) into a tree over macro
names; 3.4 turns names into formulas over the atoms above. A PC mentioning an
OPAQUE atom, a cap or an unmodeled directive (3.5) is **inexact**.

### 3.2 Integer atoms

For a macro compared to constants the model keeps the initial value, as the set
of constants used with X (after unifying `#define X Y` chains) anywhere in the
query's problem plus 0 and 1 and the neighbours c-1, c, c+1 (cond.c's domain),
encoded as ordered threshold atoms. This set is **query-scoped**; stored PCs
never depend on it (section 5).
Allowed uses: truthiness (`#if X`), `==`, `!=`, `<`, `<=`, `>`, `>=` against an
integer constant, `defined(X)`. Everything else (`X & 4`, `X % 2`, `A + B`) is
OPAQUE. `!I(X)` forces every threshold of X to its "undefined is 0" value
(the C rule cond.c implements). An empty `#define X` followed by `#if X` is a
preprocessing error when that define is reached: the fold carries a third value
state `EMPTY` (defined, no value, reachable by an in-source `#define X`; a
command-line `-DX=` is treated as OPAQUE). Evaluating it in `#if` gives PC =
(normal formula) and not EMPTY(X), and an `error` diagnostic carries the EMPTY
valuations; never "proved dead" (3.5).

### 3.3 Families

An atom set can have structure. A **family** is a DAG of implications over
**leaves**, plus exclusion between the leaves of one group (one OS leaf, one
compiler leaf, one byte order). OS and compiler are separate axes joined by
**cross-axis constraints**, so that the table cannot propose an unrealizable
pair: the MSVC and clang-cl compiler leaves require the `_WIN32` OS leaf;
the MinGW OS leaf requires a gcc or clang compiler leaf; the Android OS leaf
requires clang; `_MSC_VER` with `__linux__` is unsatisfiable. Implications are
stated per leaf, not per macro: the gnu-mode clang leaf implies `__clang__` and
`__GNUC__ >= 4`; the clang-cl leaf implies `_MSC_VER` and `__clang__` but
**not** `__GNUC__`; the MinGW leaf implies `_WIN32` and `__MINGW32__`; the
Android leaf implies `__linux__` and `__ANDROID__`; the Cygwin leaf implies
`__unix__`. It is not "at most one of `__GNUC__`/`__clang__`": clang defines
both.

The table is built in, small, and written from the predefined-macro lists of
the compilers it names. Only the host compiler is probed at build time
(`probe-host.sh`); the entries for other compilers (gcc, clang, clang-cl, MSVC,
MinGW, Cygwin, Android) are hand-written from their documentation and are
checked by the oracle only on the host. A PC proved `false` is therefore
"unsatisfiable **relative to the family table**" (3.5). Users extend the table
in settings. A PC is conjoined with the family constraints of the atoms it
mentions before it is solved. Boolean members are encoded as one-hot **leaves**
per exclusion group, including a residual leaf for an ancestor that can hold
alone ("linux other than Android", "unix other") and an explicit "none"
(freestanding); a boolean ancestor (`__unix__`) is a function of its leaves, so
setting a leaf is one change. **Integer-valued** members (`__GNUC__`,
`_MSC_VER`) keep their own threshold blocks (3.2); their family constraints are
implications over those thresholds, per leaf (the gnu-mode clang leaf implies
`__GNUC__ >= 4`; the clang-cl leaf does not).
`__STDC_VERSION__` is encoded once, as integer thresholds; it is not also a
family member.

### 3.4 Macro definitions: what a name means at a point

Two modes, each part of an answer labelled with the mode that produced it:

- **Concrete**: the unit's own preprocessor run under its one valuation. Exact
  for that valuation, and nothing else: it knows no counterfactual (a skipped
  `#define`, an include never opened). Serves `holds`, and `why_off`'s
  "currently" half.
- **Symbolic**: the fold below over skeletons of the include closure. Needed
  for `sat`, `enable`, `diff`, `cover` and the "needs" half of `why_off`. This is
  the costly mode; S0 measures it (time per entry, exactness including system
  headers). Where it would be unsound it says `inexact`.

The value of `defined(X)` at a point p is **derived**, not free:

    D(X, p) = fold over the define/undef events of X before p, in include order
    starting from  I(X)
    each event e contributes: (PC_local(e) and Reach(e)) selects the new state

Rules and limits:

- **Include order**: in symbolic mode, the textual order of the skeleton along
  the include closure that resolves with the entry's search paths. An include
  whose edge PC is not true under the entry's valuation (not reached in the
  concrete run) is followed symbolically only to depth 2; beyond that the macros
  it would define are `OPAQUE` (inexact). System headers (glibc `features.h`
  and its arithmetic on `__USE_*`, `__GLIBC_PREREQ`) are expected to be mostly
  inexact; S0c reports exactness for the user-file closure and the full closure
  separately.
- **Constant-valued defines** (`#define X`, `#define X 3`, `#define X Y` with Y
  an atom) update both `D` and the value. A define whose body is anything else
  (an expression with arithmetic, a function-like macro) makes `D(X, p)` still
  exact (it is defined) and the **value** OPAQUE.
- `#undef`, redefinition of a `-D` macro (`_GNU_SOURCE`), `#ifndef G / #define G`
  and include guards are all instances of the same fold.
- Expansion depth is capped at 8; `__COUNTER__`, `__LINE__`, `__FILE__` in a
  condition are OPAQUE.
- `#include_next`, a computed include (`#include MACRO`) and an include
  controlled by a `HAS` atom: the target file is unknown without a concrete run,
  so the includes it brings are skipped and the PCs of what it would define are
  **inexact**. They are exact when a concrete unit exists (below).

### 3.5 Exactness

Every PC carries a flag **exact** or **inexact** (an OPAQUE atom, a depth or size
cap, or a skipped include). Queries report it. An inexact PC is never used to
refuse or accept silently: "not satisfiable" on an inexact PC is `unknown`, and
"satisfiable" is `maybe` (an OPAQUE atom is free in the solver but may be forced
in reality). The cost query pins an OPAQUE or environment `HAS` atom at the
concrete run's value for that occurrence (or answers `unknown` if there is
none) and never flips it; any result touching one is inexact. An
exact unsatisfiable PC is a proof only relative to the family table (3.3).

### 3.6 Presence condition of a point, and what is stored

Two parts, kept apart:

- **Local PC** of a point in file F: the conjunction of the conditions of the
  groups around it inside F (an `#elif` chain contributes the negation of the
  earlier branches), written over **(macro name, directive index) pairs**: each
  name occurrence carries the index of the directive that tests it, because a
  condition is evaluated where it is written, not where the point is
  (`#ifdef FOO / #undef FOO / int x;` keeps `int x` live; an include guard's
  body does not see its own `#define G`). It depends on F's text only.
- **Reach** of F in a unit: the PC of the include edge that brought F in, per
  unit (the edge's own local PC and Reach, recursively), in the unit's context.

The full PC of a point in a unit is `Reach(F) and Local(p)` with every
(name, directive) pair replaced by the derived formula `D(X, at that directive)`.
The fold position is (**inclusion instance**, directive index): the instance is
the path of include edges from the unit's main file, so a file included twice
(a guarded header seen again, an X-macro file) is composed once per instance,
and `#ifndef G` has `D(G)` false at the first and true at the second. A header
included from several units has the same Local PC in all of them; only Reach
and the derived formulas differ. There is **no canonical unit**: nothing about
a header is computed relative to an unrelated unit.

Include guards and re-inclusion: an ordinary covering guard needs no extra rule
(its `#ifndef G` Local PC, evaluated per instance, does it). `#pragma once` is
modelled as a **synthetic guard macro** keyed by file identity (device and
inode, as the preprocessor resolves it, so two path spellings give one
identity; stored as a per-unit **identity-class number** in the instance tree,
assigned when the unit was built, because raw device/inode is not portable
(Windows has file index and volume serial) and changes when an editor rewrites
the file), defined when the pragma is reached: an instance is present iff its
Reach holds and the synthetic macro is not yet defined, the same fold as an
ordinary guard (a first inclusion whose Reach is false never defined it). A guard that does not cover the file
(`guard_covers_file` false) gives no guard rule: every inclusion contributes
and the file is recorded as "re-includable" (X-macro style).

### 3.7 Configuration

    Configuration {
        target:    triple, data model (sizes, alignments), endianness
        language:  std year, GNU/ISO mode, extension set (a bitset)
        overlay:   macro assignments on top of the target's predefines
        sysroot:   include directories, system headers, __has_* answers
        annot:     none | verifast   (what the lexer keeps; no atom)
        origin:    compile-DB entry index, plus overlay hash for a virtual one
    }

A configuration induces a valuation of every initial atom: the target's
predefine list **for its language field (std year, GNU/ISO mode: `-std=c99`
removes `linux`/`unix` and sets `__STRICT_ANSI__`; the host probe covers only
the default standard, so other standards are measured by re-probing or are
table entries)** with the overlay on top. `HAS` atoms of a configuration whose
sysroot is the host's are **measured** (run the preprocessor on a probe);
for another sysroot they are **environment** atoms: free, reported as such by
`why_off`, never silently assumed.

Hash. `Options.fingerprint` is a sequence-sensitive hash of the argv words
that affect diagnostics, and also the cell-cache key (`cell_config`). Fields
with no argv form (target, extension set, annotation view) are folded into
`o->fingerprint` itself, so the cell caches and `crec_flag_hash` (which mixes
the fingerprint with `CREC_FORMAT`) both separate configurations. For a host
configuration with no overlay and default such fields the fingerprint is
unchanged (a default adds nothing), so caches and manifests stay valid **until
S3**, whose `CREC_FORMAT` bump invalidates them once, on purpose. An overlay is
canonicalized (sorted, deduplicated) before hashing.

Virtual entries are never written to the compile database. Their identity is
(`main_path`, the real entry's flag hash, overlay hash), the same inputs as the
tukey plus the overlay, not a positional index, so a compile-database reorder
cannot misattach it and two files with equal flags stay distinct. The virtual
unit's manifest is keyed by `crec_tukey(main_path, flag_hash', build_id)` where
`flag_hash'` is recomputed from the real entry's flags plus the overlay, so no
key scheme changes. Manifest candidates are enumerated today from compile-database entries only
(pstate.c); the scheduler additionally enumerates virtual units from the
`cover` result, and GC treats a manifest as live if either enumeration owns it.
The scheduler addresses a unit by that triple; a compile-database reload
that changes or removes the entry drops its virtual units, and GC removes
manifests no live unit owns.

## 4. Solver

Reduced ordered BDDs over the initial atoms (3.1), one shared node table per
query, with the family constraints conjoined. **Provisional** (decision 1): the
choice rests on the spike S0 (section 8). Node cap per query (default 200,000);
past it the answer is `unknown`. The variable order is by **block** (a macro, or a family
group whose leaves encode several macros), first occurrence of the block in
file order, **with all atoms of one block contiguous** (needed for the per-macro
cost below); S0 measures it against a size-sorted alternative.

cond.c's enumerator is replaced by this. `lint` runs the solver in a mode with
no family constraints and no fold, as cond.c does today (defines not followed,
no predefines), so its goldens must be unchanged **except** where cond.c
answered `unknown` (too many variables, the 2^18 bound, `dom[24]`) and the BDD
now answers; those are listed and reviewed one by one. Family and fold results
are new diagnostics, off by default in `lint` and reported separately.

| Query | Meaning | Result |
|---|---|---|
| `sat(PC)` | some valuation keeps it | yes / no / maybe / unknown (3.5) |
| `holds(PC, V)` | under valuation V | bool |
| `why_off(PC, V)` | the cheapest change to V that makes PC true, with each part's reason (fixed by target, set by the overlay, free, environment) | a list |
| `enable(PC)` | the cheapest overlay of free atoms | overlay, or "impossible: needs X, fixed false by the target" |
| `diff(PC, V1, V2)` | the atoms that differ and decide it | list |
| `cover(PCs)` | a small set of valuations such that every satisfiable PC holds in one | list, greedy but a **pure function of its inputs**: groups in a fixed order (`main_path`, then group key), "uncovered" defined globally and updated in that order, each overlay chosen largest-cover-first with ties by key, at most 4 kept per entry (the first 4 chosen). Stability comes from keeping an already-built unit that is still in the output, never from letting built units steer the choice |

**Cost** is counted per **block**, not per threshold atom: a block is a macro
(raising X from 1 to 5 is one change, `-DX=5`) or a family group (setting a
leaf is one change; its overlay may need several words, `-U__linux__ -D_WIN32`,
and the tie-break counts overlay words).
The cost of a path is the number of macros whose assignment differs from V
(a skipped block is a don't-care and costs nothing; a block entered with V's
own assignment costs nothing). Because a macro's atoms are contiguous in the
order (above), this is a DP over the BDD with a per-block "deviated from V yet"
flag, not a plain edge-weighted shortest path. S0b implements exactly that
query, not only the diagram. Ties: free before fixed, then fewer macros, then
alphabetical.

`why_off` may report a change that is **blocked** (the target fixes it or a
family forbids it): "needs `_WIN32`; this configuration's OS is linux".

## 5. Where PCs live

1. **In memory, per unit** (S1): Local PCs from the skeleton, derived formulas
   from the symbolic fold (3.4); the concrete run supplies the current
   valuation and Reach for the include edges it took. No format change.
   Queries: hover, a code action on an inactive region.
2. **In records** (S3, `CREC_FORMAT` 5): each DECL, REF and CALL event gets a
   **Local PC id**: a canonical hash of the Local PC in a **query-independent
   form**: a normalized DAG over predicates `(name, directive index, op,
   constant)`, children sorted, `and`/`or`/`not` flattened (no BDD, so no
   variable order or threshold set enters the bytes). Bodies sit in a per-record
   table; directive indexes are per file and a function of its text. Threshold
   BDDs are rebuilt from these predicates at query time (3.2). Records hold
   **no Reach**.
3. **In the manifest**: the inclusion-instance tree (per instance: parent
   instance, include directive index, the edge's Local PC id, the resolved path
   and content hash, the identity-class number), **including untaken edges**
   (guarded-out re-inclusions, `#if`-off and `HAS`-controlled includes) down to
   the depth-2 limit of 3.4, plus the configuration hash and the family-table
   version. Records and manifests hold no file text: if the file at a stored
   content hash is gone or changed at query time, the unit is marked stale and
   its answers inexact; the fold is never silently recomputed from other text.

Define, undef and include events with their Local PC ids are **not** stored in
records: derived formulas (3.4) are rebuilt at query time by re-skeletonizing
each file of the instance tree from its content hash (the skeleton is cheap and
content-addressed, so this is sound). S3's +10% size rule covers only the PC ids
on events and the instance trees.

The full PC of a record event in a unit is composed at query time from the
record's Local PC, the manifest's edge PCs along the include chain, and the
unit's derived formulas (per inclusion instance, 3.6). Records stay
content-addressed, but a record holds events only for code its unit parsed, so
its bytes depend on the valuation as well as the text; E5 variants (the same
header seen under different configurations, B5_DESIGN) therefore remain
per-valuation. Merge rule: the same event across variants is identified by
(declaration key, Local PC id), the id being the structural hash so it matches
across records. **PCs are indexed by owning unit**: the family of full PCs of an
event is `{PC_u}`, one per (variant, owning unit u), each composed with u's
manifest edges and derived formulas, and within u a disjunction over the
inclusion instances that parsed it. `sat`, `enable`, `cover` and virtual
entries work on one `PC_u` at a time, in u's atom space (its flags, `-D`s,
sysroot and target, so an overlay is always relative to a real entry; a unit
that never includes the header contributes no `PC_u`). The disjunction over
units is used only for "is it ever present" display, never to compute an
overlay. Events in groups no unit activated are in no record; that is what
virtual units are for.

## 6. Derivation of configurations (virtual entries)

For a project, `cover` runs per real entry u over the `PC_u` of the **groups,
taken at skeleton level**: every group containing an `inactive_use` hit (a
textual word hit, no resolved symbol needed) or a parsed event of a symbol in
scope. Their Local PCs come from the skeleton (in memory until S3), so a group
no unit ever activated is included. A use inside a function body guards a
statement, not a declaration, and is covered the same way. Overlays are
restricted to atoms the target lets an
overlay flip, produces overlays; each becomes a **virtual compile entry**: the
real entry's flags plus the overlay. Budget (one rule, replacing two): total
virtual units at most `cereal.index.configs` times the number of real entries
(default 0 = off; proposed on-value 0.5, to be set from S0a's count of overlays
`cover` needs on libuv); per entry at most 4; only for groups that are exact
and satisfiable (`maybe` and `unknown` groups are excluded from the budget and
from S2's numerator), not true under the entry's own configuration, in
`cereal.index.paths` scope. Allocation: entries are served in the fixed `main_path` order (so a header edit
does not change which entries get units), until the global cap is reached.

Realizability of a virtual unit:

| Class | Meaning | Used for |
|---|---|---|
| `REALIZED` | preprocesses and checks without missing includes | indexing, rename, diagnostics |
| `PARTIAL` | missing system includes tolerated (lenient mode); user code parsed, types from the missing headers unknown | indexing of declarations whose type does not depend on them; no diagnostics; rename never (the rename gate cannot hold with unknown types) |
| `UNREALIZED` | not run: needs a sysroot or target model we do not have | explanations only |

Consequence for the OS axis (the largest in the survey): on a Linux host, a
Windows-only or macOS-only group is `PARTIAL` at best, so rename through it
stays refused and the explanation ("needs `_WIN32`") is the deliverable. S4's
stop rule is therefore set on the feature axis only (8).

## 7. Consumers

### 7.1 Language server

- Hover and code action on an inactive region: "inactive: needs `X >= 3`;
  currently X is 2 (set by `-DX=2` in compile_commands.json)".
- Symbol hover: "defined if A, B and C; A and B are defined here, C is not".
- Definition and references across configurations carry the PC; the default
  shows the union, tagged.
- `cereal/configExplain` for tests and tools.

### 7.2 Project index and rename

Rename through an inactive group (A3.9) changes from "refuse" to: allowed only
if **every** occurrence in an inactive group within scope lies in a group whose
PC is exact and satisfiable and is active in some `REALIZED` unit (real or virtual) that
passed the rename gate (B5_DESIGN R6): the unit is `REALIZED`, its post-rename
check is clean, and the new name is conflict-free in it. `inactive_use` also scans the skipped
ranges for the **new** name and refuses on a hit (a collision in a group no unit
activates is otherwise unchecked). Any other occurrence (a group seen only by `PARTIAL`
units, over budget, unsatisfiable, or inexact) keeps today's refusal, as
`inactive_use` gives it. `PARTIAL` never permits a rename. S4 must test each
of these cases for no new false edit. R4/R7 (macros) stay refused until the macro-spelling part of the same
format bump is used; that is a separate design.

### 7.3 Back end (compilation)

A configuration carries the target, so layout, ABI classification and predefines
are one input. Until a target model exists the only target is the host (x86-64
SysV); a configuration naming another target is `UNREALIZED` for checking.

### 7.4 Proving (VeriFast-style)

`annot = verifast` is a configuration field that makes the lexer keep `//@` and
`/*@ ... @*/` comments as tokens for an annotation parser. The macro `VERIFAST`
is an ordinary `I(VERIFAST)` atom, set by the overlay of the proof
configuration; `#ifndef VERIFAST` code is inactive there and active in the
compile configuration, from the same mechanism. The proof checker is out of
scope; the requirement here is only that an annotation mode is a configuration
field and that PCs of annotation-guarded code work.

## 8. Stages, the oracle, and stop rules

| Stage | Content | Format | Stop rule |
|---|---|---|---|
| S0 | **Spikes before any commitment.** (a) classify libuv's 54 A3.9 refusals by atom axis (OS / compiler / feature / other); (b) build BDDs for all groups of libuv, Lua and zstd with both variable orders: max nodes, total time; (c) count how many groups' conditions stay exact under 3.4 (user-file closure and full closure separately), and time the symbolic fold per entry; (d) how many overlays `cover` needs on libuv | none | (a) reports the feature-axis count; (b) max nodes < 20,000 per group **after substitution** of derived formulas, total under 10 s including the cost query, else revisit decision 1; (c) at least 90% exact on the user-file closure, fold under 1 s per entry |
| S1 | BDD solver; PCs and queries for the open unit; family table; hover and `cereal/configExplain`; cond.c rewritten on top | none | `lint` goldens unchanged except the reviewed `unknown` ones (every cond.c unknown source: variables, 2^18, `dom[24]`, 64 conjuncts, 32 priors, 64 frames, unmodeled conditions); the oracle agrees on at least 98% of the *decidable* groups (below) |
| S2 | virtual entries, `cover`, realizability classes, `cereal.index.configs` | none | exact feature-axis groups holding an `inactive_use` hit are covered by a `REALIZED` virtual unit at a rate set from S0a's groups-per-refused-function distribution so that S4's function-level 80% is reachable (criterion per function: all its groups covered by a `REALIZED` unit and gate-passing; 80% of the S4 denominator functions must satisfy it, using S0a's actual groups-per-function distribution, with the gate measured on a synthetic rename of those functions since S4 itself comes later); wsym p95 and builder p50 not regressed, **measured with `cereal.index.configs` at the on-value** |
| S3 | `CREC_FORMAT` 5: Local PC ids on events, edge PCs in manifests | yes | symcov unchanged; store size +10% or less, counting PC ids, instance trees and untaken edges |
| S4 | rename through inactive groups via `REALIZED` virtual units | no | on the functions among the 54 whose every inactive group is exact, satisfiable and feature-axis (S0a reports that count, the mixed-axis count and the inexact count): refusals fall by at least 80% with no new false edit |
| S5 | target and annotation fields wired to a back end / proof tool | per consumer | out of scope here |

**The oracle.** For a decidable group, `why_off`/`enable` yields an overlay; run
the real preprocessor with that overlay (`-D`/`-U`, plus the family implications
as written in the table) and read the unit's skipped-group list. The group must
be active. *Decidable* = the PC is exact, all its atoms are free or fixed by the
host (no `HAS` environment atoms, no `#error` branch, no group that includes a
missing header). The oracle is one-sided (it checks "enable works"), so S1
adds a **differential check**: run the real preprocessor under sampled
valuations and compare its skipped-group list with every group's PC evaluated
under the same valuation. Sampling is solver-guided, because random draws almost
never activate a narrow conjunction: per group, the single-atom flips of V and
the `enable` overlay of every group the PC claims unsatisfiable after relaxing
one conjunct; draws respect the family table. The claim is "finds
contradictions in the sampled valuations"; with every result it reports group
coverage (groups active in at least one run) and the share excluded as
undecidable or inexact. The oracle and check validate the solver, the
derived formulas and the fold of 3.4; they do **not** validate the family table (it uses the same table
to write the `-U`s), which is validated only against the host's own predefined
list, and for other compilers by review.

## 9. Risks

- R1. Derived formulas (3.4) across includes. Mitigation: the concrete run for
  the open unit, depth-2 symbolic following, the inexact flag, the oracle, S0c.
- R2. Family table is knowledge: wrong for a compiler we do not probe.
  Mitigation: small, documented, user-extensible; unsat proofs are relative to
  it (3.5).
- R3. `HAS` atoms for a foreign sysroot are unknowable: environment atoms.
- R4. PC table size and BDD blow-up. Mitigation: S0b, the node cap, `unknown`.
- R5. Scope creep toward variational parsing. Rule: anything that needs an AST
  shared between configurations is out of this design.
- R6. The OS axis is mostly `PARTIAL`/`UNREALIZED` on this host; the deliverable
  there is the explanation, not an edit (6).

## 10. Decisions needed (recommendations first)

1. Solver: BDD, provisional on S0b (recommended), or SAT plus MaxSAT for cost
   (smaller, but exact minimal change needs a cost optimizer).
2. First stage: S0 spikes, then S1 in process with no format change
   (recommended).
3. Families: small built-in hierarchy, extensible in settings (recommended).
4. Derived configurations: opt-in `cereal.index.configs`, default off until S2's
   rule holds (recommended).
5. Rename through `PARTIAL`: never (recommended), since R6 cannot hold with
   unknown types.
6. No canonical unit; records hold Local PCs only (recommended; replaces draft
   1's rule).

## 11. Parked

- Variational parsing: R5.
- Arithmetic atoms: OPAQUE; rare in the corpus.
- A configuration-space view: S1's result is the data; the view is a client
  matter.
- Cross-target type checking: needs the target model of 7.3.
- `__COUNTER__`/`__FILE__`/`__LINE__` in conditions: OPAQUE.

## 12. Review log

Round 1 (Sonnet reviewer, read-only; fact-checked section 2 against the tree and
attacked the design). Section 2 facts: all CONFIRMED except that `-D`/`-U` live
in `o->pp` and that `cereal.index.paths` is in project.c (both corrected), and
that cond.c has further caps (`dom[24]`, 64 conjuncts, 32 priors; added).
Findings and what changed:

- B1 Reach in a stored PC breaks record sharing: records now hold only Local PCs;
  Reach is per unit in the manifest (3.6, 5).
- B2 3.4 and 3.6 are circular (a variational preprocessor): the design now
  states the limits (depth 2, inexact), uses the concrete run for the open unit,
  and makes the real preprocessor the oracle (1, 3.4, 8).
- B3 initial state, `#undef` and value propagation missing: BDD variables are
  now only initial atoms `I(X)`, `HAS`, `OPAQUE`; the rest is derived by a fold
  (3.1, 3.4).
- B4 "at most one" families are false (clang defines `__GNUC__`; clang-cl;
  Android; Cygwin): hierarchy with leaf exclusion (3.3); unsat proofs relative to
  the table (3.5).
- B5 S4's rule unreachable on the OS axis: S0a classifies the 54 first; S4's rule
  is on the feature axis only; PARTIAL never permits rename (6, 7.2, 8).
- B6 include-guard rule wrong for X-macro, non-covering guards, `#pragma once`,
  path spellings: rewritten (3.6).
- S1 contradictory stop rules: one budget rule, goldens "unchanged except the
  reviewed unknown ones" (4, 6, 8). S2 oracle does not validate the table and
  has no denominator: both stated, "decidable" defined (8). S3 cost per macro
  (4). S4 integer atoms: allowed uses listed, others OPAQUE, thresholds include 0
  and 1 (3.2). S5 `HAS` atoms: measured on the host sysroot, environment for
  others (3.7). S6 canonical unit: dropped (3.6, decision 6). S7 virtual entries'
  identity, manifest keys, fingerprint unchanged for default fields (3.7).
  S8 BDD size claim unmeasured: S0b and decision 1 provisional (4, 8). S9 edge
  cases (`#include_next`, computed includes, `__has_include` controlling an
  include, `__COUNTER__`): inexact rules (3.4, 11).
Round 2 (Sonnet). New blocking: N1 (conditions must be evaluated at their own
directive, not at the point; names carry directive indexes: 3.6, 5), N2 (a
concrete run yields values, not formulas: two modes, symbolic fold costed in S0:
3.4, 5, 8), N3 (rename must keep the refusal for any occurrence no passed
`REALIZED` unit covers: 7.2). Should-fix: lint goldens versus new semantics (4);
per-macro cost versus BDD (contiguous blocks, leaves-only family variables,
cost query in S0b: 3, 4); one-sided oracle (differential check: 8); fingerprint
canonicalization, "valid until S3", virtual-unit enumeration (3.7); budget
default (6); `__STDC_VERSION__` single encoding (3.3). Nits: error not proof
(3.2); S0b measures after substitution. Not verified: the E5 "variants" term
appears only in docs (B5_DESIGN), not in src; `include_next` is not
distinguishable in the skeleton (it is already treated as inexact).

Round 3 (Sonnet; no blocking). Should-fix: fold position is (inclusion
instance, directive index) (3.6); family encoding needs residual and "none"
leaves, integer members keep threshold blocks (3.3); cost is a DP with a
deviated flag (4); differential check made solver-guided and its claim reworded
(8); target/extension/annotation folded into `o->fingerprint`, which is also
the cell-cache key (3.7); "gate" defined, B5_DESIGN R6 cited (6, 7.2); record
variants stay per-valuation with a merge rule (5). Nits: modes labelled per part
(3.4); S2 stop rule measured (8); lint unknown list completed (8); guard rule
only for `#pragma once` and non-covering guards (3.6).

Round 4 (Sonnet; no blocking). Should-fix: cover set must contain groups with
uses, not only declarations, and S2 above S4 (6, 8); family as per-leaf DAG,
clang-cl has no `__GNUC__` (3.3); Local PC id is a structural hash and the
merged PC is a disjunction over (variant, unit, instance) (5). Nits: cost unit
is a block (4); OPAQUE/HAS never flipped, sat on inexact is `maybe` (3.5);
`error` is PC-conditioned (3.2).

Round 5 (Sonnet; no blocking). Should-fix: PCs indexed per owning unit, cover
and enable per entry (5, 6); stored PCs in a query-independent predicate DAG,
`#define X Y` unified, thresholds query-scoped (3.2, 5); cover set at skeleton
level so groups with no events are included (6); S2 rate derived from S4's,
measured with the feature on (8); OS/compiler cross-axis constraints (3.3).
Nits: OPAQUE pinned, `maybe` in the table and budget (3.5, 4, 6); `EMPTY`
state (3.2); virtual-unit identity by flag hash (3.7); status line.

Round 6 (Sonnet; 1 blocking). Blocking: virtual-unit identity lacked
`main_path` (3.7). Should-fix: define/undef events and the instance tree were
not persisted (5: instance tree in the manifest, derived formulas by
re-skeletonizing); `#pragma once` is a synthetic guard (3.6); S4 denominator and
gate-pass rate (8); canonical cover and allocation (4, 6). Nits: `-DX=` EMPTY
(3.2); 3.1 wording; new-name scan in skipped ranges (7.2); language field in the
valuation (3.7).

Round 7 (Sonnet; no blocking). Should-fix: instance tree includes untaken
edges, stale-hash rule (5); `#pragma once` identity as a per-unit class number
(3.6, 5); cover a pure function with fixed allocation (4, 6); S2 criterion per
function, gate measured synthetically (8). Nits: blocks are macro or family
group (4); real or virtual units in 7.2.

Stopping rule applied: seven rounds; findings went from 6 blocking (round 1) to
none material to the model in rounds 5 to 7, only implementation-level gaps.
Further review has lower yield than the S0 spikes (8), which test the claims the
reviews cannot (BDD size, fold cost, exactness).

Round 1 nits:
- N1 `ENV` atom: dropped; `VERIFAST` is an ordinary `I(VERIFAST)` (7.4). N2
  virtual entries do not edit the compile database (3.7). N3 relative proof
  (3.5). N4 decision 1 provisional (4, 10).

## 13. Spike results

S0a (Round 240; `rename_libuv.py --verbose` on libuv, 441 functions, 54 refused
for A3.9; each refusal names only the first inactive use, so groups per function
is not measured). Each refusal classified by the atoms of the `#if` chain around
that use: 20 OS only, 26 OS plus a guard or derived macro (include guards
`UV_UNIX_INTERNAL_H_`, `UV_H`, `PTHREAD_BARRIER_SERIAL_THREAD`), 8 with no OS
atom (`uv__accept4`, `UV_USE_SIGCHLD`, `O_CLOEXEC`, `UV__CPU_AFFINITY_SUPPORTED`,
and `UV_H`/`__cplusplus`, the dead `extern "C"` guard). Reading: at least 46 of
54 (85%) are OS-axis; the 8 "feature" ones are themselves derived from OS macros
by `#define`s in headers (so only the fold of 3.4 can make them satisfiable),
or dead. **Consequence: S4's feature-axis subset is about 4 functions, so S4's
80% rule is not meaningful as written; rename through inactive groups on this
corpus is almost entirely the OS axis, which is `PARTIAL`/`UNREALIZED` on a
Linux host (6).** The value of the mechanism on this corpus is therefore the
explanation (7.1: "needs `__APPLE__`") and group-aware refusal text, not
rename edits; S4 should be re-scoped to measure the explanation (every refusal
names the atoms it needs) unless a sysroot-carrying cross configuration is
added. Not run yet: S0b (BDD size), S0c (fold exactness and cost), S0d.
