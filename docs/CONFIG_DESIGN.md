---
status: active
---

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
`__STDC_VERSION__` is not a family member and not a variable of its own: it is
derived from the language group's row (3.8), which is the single encoding of
everything the std field determines.

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
  it would define are `OPAQUE` (inexact). **System headers** (found through the
  system search path, not a quoted include of the project) are folded in
  **host-concrete mode** at any depth: their conditions are evaluated under the
  entry's host valuation, their groups are not recorded as presence conditions,
  and their definitions are guarded only by the PC of the including point. The
  macros they define are therefore `environment`-kind (3.8) and cost no BDD
  size (S0b v2c, section 13). S0c reports exactness for the user-file closure
  and the full closure separately.
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
in reality). The cost query pins an OPAQUE atom at the concrete run's value for
that occurrence and never flips it (with no pin value it is free and reported
`unknown`); any result touching one is inexact. Environment `HAS` atoms are
pinned in the overlay phase and relaxed in the relaxation phases (3.8). An
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

### 3.8 Preconditions: the general explanation

The primary answer to "why is this group (or declaration, or symbol) not
present here" is its **precondition list**: the cheapest set of requirements
that must additionally hold for its PC to be true under the configuration's
valuation V. Every other explanation is derived from it.

    Requirement { clause, kind, status, source, via }

`status` is met, unmet or unknown in V; `source` is the directive (file, line)
that imposes it; `via`, when the atom's value is set by a define or include
event in another file, names that event (a label for the explanation, not a
kind: the solver never reorders includes).

**Kind of an atom** = the lowest-precedence Configuration field (3.7) that can
change its value, in this order:

| Kind | Changed by | Atoms |
|---|---|---|
| `config` | the overlay | free `I`/`CMP` atoms, and leaves the family table marks **overlay-flippable** |
| `language` | the language field (std, mode, extensions) | atoms the language field determines (`__STDC_VERSION__`, `__STRICT_ANSI__`, `linux` under `-std=c99`) |
| `target` | the target or compiler leaf | OS, architecture, data model, compiler identity and version leaves; **not** overlay-flippable by default |
| `environment` | the sysroot | `HAS` atoms (host-measured ones too: a missing header on this host is `environment`) |
| `opaque` | nothing known | OPAQUE atoms |

An atom that both the language field and the target determine (`linux`) has
kind `language` if some std value in the table changes it, else `target`. The
family table carries one **overlay-flippable** bit per leaf (default false for
OS and compiler leaves); section 6's "atoms an overlay can flip" and this table
use that bit and nothing else.

**The language group is a fourth block kind** (beside macro, family group and
one-atom `HAS` blocks). It is a one-hot group over the rows of the std table
(std year x GNU/ISO mode x extension set; the host probe supplies only the
default row, 3.7). *As built (Round 261): two independent one-hot family axes,
`std` (c89, c99, c11, c17, c23) and `mode` (gnu, iso), with the same machinery
as the target axes (table rows `__STDC_VERSION__` valued per std leaf,
`__STRICT_ANSI__` by mode); atoms of class `PC_LANG` weigh 100 (a config change
1, a target change 1000) and render as `language std = c23`. Not yet (PARKED.md): bare
`linux`/`unix` (needs a mode and an OS leaf at once), the extension set,
`__STDC_VERSION__` of C94.* The std-sensitive atoms are **derived functions of the row**,
not BDD variables of their own: `__STDC_VERSION__` thresholds, `__STRICT_ANSI__`,
and `I(linux)` = `leaf_linux AND row_defines_linux` (likewise for every
std-sensitive macro; 3.2 and 3.3 do not encode them separately). Choosing a row
costs one change; its first-occurrence position in the variable order is the
earliest occurrence of any macro it determines. S0b tests a PC that needs
`linux` under `-std=c99`. If V's actual flags match no table row (an arbitrary
extension bitset), the language block is not pinned and not costed: `language`
requirements are then `unknown` and the answer is inexact.

**Pinning** restricts each family group to V's leaf plus the leaves the table
marks overlay-flippable, with cross-axis constraints (3.3) checked against the
pinned compiler leaf; it never frees a non-flippable leaf.

**Computation**, in phases, all the DP of section 4 with its per-block
"deviated" flag:

Blocks: a one-atom `HAS` atom is its own block for the deviated flag. The
phase costs are folded into one scalar for the DP (class weights larger than the
maximum count of lower-class changes), so section 4's single-cost DP is reused.
A **requirement is typed by the highest-class block that deviates in the cube
for its clause** (an atom needing `linux` while V is windows deviates on the OS
block and the language row: that is a `target` requirement, not `language`);
the atom-level kind of the table only renders met/unmet.

1. *Overlay phase.* Pin every `target` and `environment` block (as above) and
   every OPAQUE atom that has a pin value (its value in the concrete run for
   that occurrence, 3.5); an OPAQUE atom with no pin value is free here and
   reported `unknown`. Find the cheapest cube over `config` blocks and the
   language block (tie-break config before language applies only here). If one
   exists, `REALIZED` is reachable and `enable` uses it.
2. *Relaxation phases, in class order.* If phase 1 has no cube, 2a relaxes only
   `environment` blocks (cost: environment changes, then overlay changes); if
   that has none, 2b also relaxes `target` blocks (cost lexicographic: target
   changes, then environment changes, then overlay changes). So `PARTIAL` is
   decided by an actual query (2a), not inferred from a cheaper `UNREALIZED`
   cube. Each requirement of the returned cube is typed.
   In 2b the compiler leaf is free and cross-axis constraints are just the
   always-conjoined family constraints; they are checked against the pinned
   compiler leaf only in phases 1 and 2a.
3. *No cube.* Phase 1 empty means "not achievable by overlay" (`enable`
   returns that with the phase-2 list attached). "Impossible" is answered only
   when no phase has a cube, the PC is exact and has no OPAQUE atom; with a
   pinned OPAQUE atom or an inexact PC, no cube means `unknown` (3.5).

**Alternatives.** Rank 1 is the guaranteed cheapest cube. Further ranks come
from enumerating BDD paths in cost order, expanding each to a **prime cube**
(dropping literals in a fixed variable order, since BDD paths are disjoint but
not prime), deduplicating, and continuing until K=3 distinct primes or a path
cap (default 64); the list may therefore be shorter than K, and ranks 2 and 3
are "next cheapest found", not guaranteed. The cost is computed after
expansion. `cover`'s candidate overlays are the per-group phase-1 cubes (up to
K) and merges of them; a merged valuation (V plus two cubes) is **evaluated
against every group's PC**, and coverage is counted from that evaluation, not
from cube membership, since a merge can falsify a group that V or an earlier
cube satisfied. The coverage guarantee holds only for groups with a phase-1
cube (section 6 eligibility says so). S0b exercises all phases.

What it subsumes (one mechanism instead of several):

- `why_off` is the unmet requirements of the cheapest cube, rendered (met ones
  shown only on request); `enable` is the phase-1 cube as an overlay, and if
  phase 1 has no cube it answers "impossible" and attaches the phase-2 list.
- **Realizability** (section 6) is the worst of (the class of the best
  alternative cube) and (the unit's actual build result): a phase-1 cube gives
  `REALIZED`; unmet `environment` gives at best `PARTIAL`; unmet `target`
  gives `UNREALIZED` on this host; `unknown` gives no unit. A missing include
  at build time downgrades `REALIZED` to `PARTIAL` whatever the cube says.
- The OS-versus-feature distinction of S0a (13) is not a special case: an
  OS-axis refusal is "unmet: target = apple", a feature-axis one is
  "unmet: config USE_FOO".
- Hover text (7.1), code-action text, rename refusal text (7.2) and the
  `cereal/configExplain` reply are renderings of the list.
- A back end or prover (7.3, 7.4) asks the same question with its own
  configuration: its fields change which atoms are `config`, `language` or
  `target`, not the computation.

`language` requirements are not macro overlays: `flag_hash'` of a virtual entry
(3.7) is recomputed over the std argv words as well as the overlay, so the
identity triple is unchanged, and a `language`-only unit is as exact
as the std table or a re-probe for that std (the host probe covers only the
default std); without one it is `inexact`.

What it cannot cover: requirements that are not about presence (a type error in
an active group), and anything that depends on an inexact PC beyond `unknown`.

## 4. Solver

Reduced ordered BDDs over the initial atoms (3.1), one shared node table per
query, with the family constraints conjoined. **Provisional** (decision 1): the
choice rests on the spike S0 (section 8). Node cap per query (default 200,000);
past it the answer is `unknown`. The variable order is by **block** (a macro, a family
group whose leaves encode several macros, a one-atom `HAS` block, or the
language group of 3.8, whose row determines several macros), first occurrence of the block in
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
| `preconditions(PC, V)` | the cheapest cubes (up to 3) that make PC true, each requirement typed by kind and status (3.8) | list of Requirement lists |
| `why_off(PC, V)` | `preconditions` restricted to the unmet requirements, rendered | a list |
| `enable(PC)` | the phase-1 cube (3.8) as an overlay plus language fields | overlay; "not achievable by overlay" with the phase-2 list attached; "impossible" only per 3.8; `unknown` for an inexact PC |
| `diff(PC, V1, V2)` | the atoms that differ and decide it | list |
| `cover(PCs)` | a small set of valuations such that every satisfiable PC holds in one | list, greedy but a **pure function of its inputs**: groups in a fixed order (`main_path`, then group key), "uncovered" defined globally and updated in that order, each overlay chosen largest-cover-first with ties by key, at most B kept per entry (the first B chosen; B = `cereal.index.maxOverlays`, default 8, section 13). Stability comes from keeping an already-built unit that is still in the output, never from letting built units steer the choice |

**Cost** is counted per **block**, not per threshold atom: a block is a macro
(raising X from 1 to 5 is one change, `-DX=5`) or a family group (setting a
leaf is one change; its overlay may need several words, e.g. `-U__unix__
-D__MVS__` for an overlay-flippable leaf pair, and the tie-break counts overlay
words; non-flippable leaves are changed only in the relaxation phases, 3.8).
The cost of a path is the number of macros whose assignment differs from V
(a skipped block is a don't-care and costs nothing; a block entered with V's
own assignment costs nothing). Because a macro's atoms are contiguous in the
order (above), this is a DP over the BDD with a per-block "deviated from V yet"
flag, not a plain edge-weighted shortest path. S0b implements exactly that
query, not only the diagram. Among cubes of equal cost (phase-lexicographic, 3.8): lower kind first (config,
language, target, environment; `opaque` never costs), then fewer overlay words,
then alphabetical.

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
restricted to `config`-kind atoms (3.8: those the family table marks
overlay-flippable, and free atoms) and `language` fields, produces overlays; each becomes a **virtual compile entry**: the
real entry's flags plus the overlay. Budget (one rule, replacing two): total
virtual units at most `cereal.index.configs` times the number of real entries
(default 0 = off; proposed on-value 0.5, to be set from S0a's count of overlays
`cover` needs on libuv, 364 for 62 units at B=8, section 13); per entry at most B (default 8); only for groups that are exact
and satisfiable with a **phase-1 cube** (`maybe`, `unknown` and groups that need
`environment` or `target` changes are excluded from the budget and from S2's
numerator; they get explanations only), not true under the entry's own configuration, in
`cereal.index.paths` scope. Allocation: entries are served in the fixed `main_path` order (so a header edit
does not change which entries get units), until the global cap is reached.

Realizability of a virtual unit is determined by the kinds of the unmet
requirements of the group it is meant to activate (3.8):

| Class | Meaning | Used for |
|---|---|---|
| `REALIZED` | only `config`/`language` unmet; preprocesses and checks without missing includes | indexing, rename, diagnostics |
| `PARTIAL` | an `environment` requirement unmet: missing system includes tolerated (lenient mode); user code parsed, types from the missing headers unknown | indexing of declarations whose type does not depend on them; no diagnostics; rename never (the rename gate cannot hold with unknown types) |
| `UNREALIZED` | a `target` requirement unmet: not run, needs a target model we do not have | explanations only |

(An `opaque` requirement makes the class `unknown`: no unit is built. A
`REALIZED` unit reached through a `language` change is `inexact` unless the std
table or a re-probe covers that std, 3.8.)

Consequence for the OS axis (the largest in the survey, 13): on a Linux host, a
Windows-only or macOS-only group has an unmet `target` requirement, hence
`UNREALIZED`, so rename through it stays refused and the precondition list
("unmet: target = apple") is the deliverable. S4's rule is set on the
explanation, not on rename edits (8).

Implemented (Round 272): `cereal.index.configs` (a ratio, default 0 = off) and
`cereal.index.maxOverlays` (default 8). With `configs` > 0 the index job of a real unit also folds
its presence conditions (the fold is only in the job then: it costs 20-25% of one) and writes the
overlays `cover` picks to a sidecar `ovl/<tukey>.ovl` (one overlay per line, before the manifest,
so a manifest implies its sidecar; `index-summary --cover N`). A real unit that is fresh but has no
sidecar is stale as `cover` and runs again once. When every real unit is fresh or backing off
(or at startup, if all are fresh), `pstate_allocate` gives the budget (ratio times the real
units) to the real units in main-path order, each up to `maxOverlays` of its sidecar, and makes
one virtual unit per overlay: the entry's flags plus the overlay words (`index-summary --entry N
--overlay WORDS`), its own manifest under `crec_tukey(main, flag_hash', build_id)`, scheduled after
all real units, and dropped when its sidecar no longer names it. The sidecar is kept by GC as long
as its real unit is live. A virtual unit whose check ends PARTIAL, FAILED or OVER_LIMIT is not wanted
(Round 275): the realizability class is the measured result, since no rule on the macro names finds
such an overlay (Round 274). The rename gate (Round 276): the project gate no longer refuses a name
found in an inactive `#if` group of a user file at once; each unit reports those uses (`CRenInactive`)
and the files its index has, and a use is accepted when another unit that passed the gate (a real one,
or a fresh sound virtual unit listing the edited files, checked with its overlay) reads that file and
compiles the position (`c_rename_uncovered`). A virtual unit that fails its check only covers
nothing. Measured on libuv (441 external functions, `rename_libuv.py`): 382 renames, 54 refused for
an inactive group, 5 for a macro, with `configs` 0 and 0.5 alike: libuv's inactive groups are other
platforms' branches (`__APPLE__`, `__sun`, `_AIX`, `_WIN32`), target atoms that `enable` never flips.
The coverage helps where the group is a project option (`DEBUG`, `USE_X`), as in the `renamecfg` test. Not done: `cereal.index.configs` is read at startup only.

Implemented (Round 280, S4): a project rename refused for an inactive group now says what would turn
the group on, in the same message: `... which cannot be checked (`#ifdef __APPLE__`: to compile it:
target `os = apple`)`. After `resolve_inactive` has picked the first hit no unit covers, `lsp_rename_gate`
rebuilds that one unit (`gate_unit`: the same options as the check) with the fold (`c_rename_explain`,
`analysis_attach_pc`, `pc_explain`) and renders one line (`pc_explain_needs`; the cubes are the ones
hover shows, via the shared `put_cube`). Successful renames pay nothing; a refusal pays one more
preprocess of one unit. The line names the directive, because `os = other` alone does not say which
system. Cases: alternatives joined by `or`; `its condition cannot hold` (unsat); `it depends on a test
the analysis cannot evaluate` (inexact); `no way to turn it on was found`. The cubes of an
`explain` never use the guard of a header that has a system header below it (`explain_fixed`; the fold
records those files in `PcUnit.sysbelow` while it folds): the falsifier found 14 libuv refusals where
`defined(UV_H)` was offered and the group stayed off, because the system headers are replayed from
the real run and a later include of the same one, skipped by its guard there, is not read again.
Gates on libuv (Round 280): all 54 inactive-group refusals carry a list (52 `target`, 2 option; none
missing, no unsat/opaque); `s4_falsify.py` (the real preprocessor, a marker line before the use, the
family table ignored, system headers the host lacks stubbed) confirms 51, cannot judge 2 (the group
tests `O_CLOEXEC`, which the host's own headers define and no flag undoes) and skips 1 (`__cplusplus`,
a language atom); none is refuted. (c): rename through inactive groups gains 0 functions here, as the
S2 measurement already showed (the 54 are target atoms; the two option lists are `__PASE__` and
`__cplusplus`, which `enable` does not flip). Not done: the code action carries no list (hover does,
for the open unit); the CLI `query rename=` path still refuses without one.

Implemented (config header, 2026-10-17): the configuration header is a file named by `-imacros` in
`.cereal` (or the compile flags); no new key. A macro defined while an `-imacros` frame is open gets
`Macro.cfg` (set in `do_define` from the frame's `quiet`, which only `-imacros` sets), and the three
overlay checks in pc.c (`unit_defines`, `enable_fixed`, `pc_req_via`) treat it like a command-line
macro: a preconditions list names `defined(FEATURE_X)` instead of "the unit defines `FEATURE_X`", and
`enable`/`cover` may flip it. `predefined` is unchanged, so the index, hover and rename still see the
header's macros as the project's own. Test: `tests/lsp/pccfg` (falsified: without `cfg` the lists say
"the unit defines").

Implemented (late overlay layer, same day): gcc runs `-imacros` after every `-D`/`-U` (checked with
gcc 15.2), cereal ran them as written. `tu_init` now follows gcc (`-D`/`-U`, `-imacros`, `-include`), and
an overlay's words follow `-fcereal-late-macros` (`config.c: options_with`), which marks the later
`-D`/`-U` `CmdlineMacro.late`: they are applied last, so an overlay beats the header while a user's own
`-D` before `-imacros` loses to it, as in gcc. Tests: `tests/run.sh` pp/imacros_conf (against gcc),
`proj_test renamehdr` (a virtual unit compiles a group the header turns off, so the project rename goes
through). A virtual unit's overlay over a header macro prints gcc's "redefined" warning into the unit's
diagnostics, which nothing shows.

Implemented (code action, same day): `refactor.cereal.enableGroup` edits the header instead of
guessing a flags file (LSP.md, Code actions). `pc_enable_group` now prefers a cube without a negative
value, so `#if FEATURE_X` offers `= 1`, not `= -1` (this also changes the words `cover` writes for such
groups). The remaining edit-target options are parked (PARKED.md).

Implemented (toggles, same day): a first trial on lua54 (a header with six options, 13 directives in five
files) offered 0 actions: real options are `#ifdef X` / `#ifndef X` toggles, not valued lines. The action
now also appends `#define X 1` to the first `-imacros` file (`PP.cfg_file`) for a macro nobody defines,
and deletes the header's `#define` line for a `-U`. Checked with the real preprocessor on lua54: removing
`LUA_USE_JUMPTABLE` turns on the `lvm.c:38` group, adding `LUA_USE_C89` turns on `lobject.c:155` (marker
absent before, present after); the other directives were already active, so 3 of 13 got an action.

Implemented earlier (Round 264): `enable` (one group to command-line words) and `cover` (greedy, no merges
of cubes yet), both oracle-checked by re-running the preprocessor (`bench/pc_enable.py`). An overlay never
sets a macro the unit defines itself (a guard, or a default tested before its `#define`) or a target, env,
opaque or unknown requirement; those groups are not enable-able, and `cover` leaves them out.

## 7. Consumers

### 7.1 Language server

- Hover and code action on an inactive region render the precondition list
  (3.8), one line per requirement with its kind: "inactive: unmet (config)
  `X >= 3`, currently X is 2 (set by `-DX=2` in compile_commands.json)" or
  "unmet (target) `__APPLE__`"; met requirements are shown on request. The code action offers the
  overlay only when every unmet requirement is `config` or `language`.
- Symbol hover: "defined if A, B and C; A and B are defined here, C is not",
  with the kind of C.
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
| S0 | **Spikes before any commitment.** (a) classify libuv's 54 A3.9 refusals by atom axis (OS / compiler / feature / other); (b) build BDDs for all groups of libuv, Lua and zstd with both variable orders: max nodes, total time; (c) count how many groups' conditions stay exact under 3.4 (user-file closure and full closure separately), and time the symbolic fold per entry; (d) how many overlays `cover` needs on libuv; (e) re-run (a) enumerating **every** inactive group per refused function, typed by 3.8 kind and phase | none | (a) reports the feature-axis count; (b) max nodes < 20,000 per group **after substitution** of derived formulas, total under 10 s including the cost query, else revisit decision 1; (c) at least 90% exact on the user-file closure, fold under 1 s per entry |
| S1 | BDD solver; PCs and queries for the open unit; family table; hover and `cereal/configExplain`; cond.c rewritten on top | none | `lint` goldens unchanged except the reviewed `unknown` ones (every cond.c unknown source: variables, 2^18, `dom[24]`, 64 conjuncts, 32 priors, 64 frames, unmodeled conditions); the oracle agrees on at least 98% of the *decidable* groups (below) |
| S2 | virtual entries, `cover`, realizability classes, `cereal.index.configs` | none | exact groups of phase-1 (`config`/`language`) kind holding an `inactive_use` hit are covered by a `REALIZED` virtual unit at a rate set from S0a's groups-per-refused-function distribution so that S4's function-level 80% is reachable (criterion per function: all its groups covered by a `REALIZED` unit and gate-passing; 80% of the S4 denominator functions must satisfy it, using S0a's actual groups-per-function distribution, with the gate measured on a synthetic rename of those functions since S4 itself comes later); wsym p95 and builder p50 not regressed, **measured with `cereal.index.configs` at the on-value** |
| S3 | `CREC_FORMAT` 5: Local PC ids on events, edge PCs in manifests ([plan](S3_PLAN.md): spike run 2026-10-17; decided against, see PARKED.md S3-records) | yes | symcov unchanged; store size +10% or less, counting PC ids, instance trees and untaken edges |
| S4 | precondition lists wherever a group is refused or shown (rename refusal text, hover, code action); rename through inactive groups via `REALIZED` units where the lists allow | no | (a) every one of the 54 libuv A3.9 refusals carries a precondition list; the report counts the `opaque`-typed and undecidable ones, and the oracle confirms, where only phase-1 requirements are unmet, that satisfying exactly them activates the group; (b) a falsifier for `target`-typed atoms: run the real preprocessor with the atom flipped, ignoring the family table, and confirm the refusal is family-forced, not mis-typed; (c) rename edits, a small bonus on this corpus (S0e: at most 9 of 54, probably fewer) and not a goal: the denominator is the refused functions whose every inactive group is phase-1 (from the re-run S0a, S0e); if that is under 10 functions, the rule is the absolute count of them that now succeed, with no new false edit |
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

1. Solver: BDD (recommended), with family and threshold constraints applied
   per group at query time (only the clauses that share a variable with the
   group), system headers folded host-concretely (3.4), and the variable order
   completed by a first fold pass before the final one (S0b v2, section 13), or
   SAT plus MaxSAT for cost (smaller, but exact minimal change needs a cost
   optimizer).
2. First stage: S0 spikes, then S1 in process with no format change
   (recommended).
3. Families: small built-in hierarchy, extensible in settings (recommended).
4. Derived configurations: opt-in `cereal.index.configs`, default off until S2's
   rule holds (recommended). Decided (Round 279): the default stays 0. The rule holds
   (gate p95 and builder p50 within limits at 0.5), but the index costs about 6-10x
   (1,770-unit wsym project, 3 threads: 15-18 s to 85-90 s warm) and the query p95 rises slightly, for a
   rename benefit seen only on option-style groups (libuv: none).
5. Rename through `PARTIAL`: never (recommended), since R6 cannot hold with
   unknown types.
6. No canonical unit; records hold Local PCs only (recommended; replaces draft
   1's rule).

## 11. Parked

Moved to ../PARKED.md (config and presence conditions).

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

Round 8 (Sonnet, on section 3.8 only; 2 blocking). Blocking: `reach` cannot be a
cube kind because the solver never reorders includes (now a `via` label, kinds
are defined by the Configuration field that changes an atom); the two-phase
computation was under-specified (phase 1 pins non-overlay blocks, phase 2
relaxes lexicographically, K irredundant cubes, `enable` is the phase-1 cube).
Should-fix: kind as a function of the atom, overlay-flippable bit in the family
table (3.8, 6); OPAQUE pinned versus unknown (3.8); language fields are not
macro overlays and enter the unit hash (3.8); S4 vacuity and unmeasured
denominator, falsifier for target-typed atoms, S0e re-run (8, 13); realizability
as worst of cube class and build result (3.8). Nits: tie-break order, "cheapest"
not "minimal", met items on request, UV_H partition.

Round 9 (Sonnet, on 3.8; 1 blocking). Blocking: the language field is one
block of std-table rows costed as one change, side-effected atoms "determined
by the row" (3.8). Should-fix: 3.5 versus 3.8 on environment atoms; OPAQUE with
no pin and "impossible" only for exact PCs (3.8, 3.5); pinning per leaf with the
flippable bit and a flippable example in 4; phases 2a (environment) and 2b
(target) so PARTIAL is a real query; k-best then prime-cube expansion; stale
`reach` in 6 and `cover` candidates; tie-break wording; `flag_hash'` over std
words.

Round 10 (Sonnet, on 3.8; 1 blocking). Blocking: the language row is a fourth
block kind, std-sensitive atoms are derived from it, not variables (3.3, 3.8, 4).
Should-fix: requirement typed by the highest-class deviating block (3.8); V's
row not in the table leaves language unpinned and unknown (3.8); K alternatives
only rank 1 guaranteed, fixed prime expansion, path cap (3.8); cover counted by
evaluating merged valuations, eligibility needs a phase-1 cube (3.8, 6);
"not achievable by overlay" versus "impossible" (3.8, 4). Nits: scalar class
weights, `HAS` one-atom blocks, 2b cross-axis, language-only exactness (3.8, 6).
Review of 3.8 stopped here by judgment: remaining risk is implementation
detail that S0b exercises (all phases) better than another read.

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
`UV_UNIX_INTERNAL_H_`, `PTHREAD_BARRIER_SERIAL_THREAD`), 8 with no OS
atom (`uv__accept4`, `UV_USE_SIGCHLD`, `O_CLOEXEC`, `UV__CPU_AFFINITY_SUPPORTED`,
and `UV_H`/`__cplusplus`, the dead `extern "C"` guard). Reading: at least 46 of
54 (85%) are OS-axis; the 8 "feature" ones are themselves derived from OS macros
by `#define`s in headers (so only the fold of 3.4 can make them satisfiable),
or dead. **Consequence: the number of refusals needing only `config`/`language`
is somewhere from 0 to 8 of 54 (unmeasured: the 8 may be `target` kind once the
fold resolves them), so an 80% rule on that subset is not meaningful as
written; rename through inactive groups on this corpus is almost entirely the
OS axis, which is `target` kind and `UNREALIZED` on a Linux host (6).** These
S0a axes (OS/compiler/feature) are not the 3.8 kinds; S0a must be re-run
enumerating every inactive group of each refused function, classified by kind
with both phases. The value of the mechanism on this corpus is therefore the
explanation (7.1: "needs `__APPLE__`") and group-aware refusal text, not
rename edits; S4 should be re-scoped to measure the explanation (every refusal
names the atoms it needs) unless a sysroot-carrying cross configuration is
added. Not run yet: S0b (BDD size), S0c (fold exactness and cost), S0d, S0e.
After the user asked to generalize to "this group needs these preconditions to
be active", S4 was re-scoped to the typed precondition list (3.8, 8).

S0e (Round 242; throwaway script, a textual three-valued evaluator under the
host gcc's predefines, not the design's fold; 55 names in the list, one more
than the 54 refusals, so counts are approximate). For each refused function,
every textual occurrence in an `#if` chain that evaluates false under Linux/gcc,
grouped by chain and typed by atom kind (include guards ignored): **46 functions
have at least one inactive group of `target` kind** (an OS or compiler
predefine), 1 only a `config`/derived-looking group (`uv_tty_set_mode`, the
C++ `extern "C"` guard, effectively dead), and 8 show no group that evaluates
false (their atoms are macros libuv defines itself, unknown to a textual
evaluator: the cases only the fold of 3.4 can resolve). Groups per function:
min 0, median 1, max 7. Reading: S4(c)'s denominator (every inactive group
`config`/`language`) is **at most 9 of 54 and probably fewer**, below the
10-function threshold, so rename-edit gains are small on this corpus and S4(a)
(typed lists for all refusals) is the deliverable. The 8 unresolved functions
are the first real test of the symbolic fold (S0c).

S0b/S0c prototype (Round 243; throwaway Python, ROBDD plus a symbolic fold over
the quoted-include closure with no depth cap, one variable per macro in
first-occurrence order, every comparison literal `X op c` an independent
variable, no family constraints, no system headers). Per unit, node cap 200,000:

| Corpus | Units | Groups | Max nodes, one group | Max node table, one unit | Time (all units) | Slowest unit |
|---|---|---|---|---|---|---|
| libuv (src, src/unix) | 62 | 30,839 | 44 | 22,965 | 2.7 s | 0.14 s |
| Lua 5.4 | 35 | 19,279 | 79 | 38,966 | 4.6 s | 0.45 s |
| zstd amalgamation | 1 | 1,145 | 126 | 13,259 | 0.1 s | 0.09 s |

No unit came near the cap. Size and time are therefore not the risk for
decision 1: a BDD is affordable (the stop rule of S0b, under 20,000 nodes per
group and under 10 s total, holds with a wide margin on these inputs).
**Not measured, and where the real risk lies:** (1) exactness is overstated: the
prototype counts a group exact when no OPAQUE variable is in its support
(libuv 99%, Lua and zstd 89%), but macros that system headers define are free
initial atoms here, which the design counts as inexact; the honest figure needs
system-header handling. (2) The threshold encoding of 3.2 and the family
constraints would add relations between variables and could change sizes; not
modelled. (3) The cost DP and the phased cheapest-cube query (3.8) were not
built. (4) 25% of libuv groups are constant (true/false), mostly include
guards and unreachable include branches. Decision 1 stays "BDD" with size and
time confirmed; the open items move to the first implementation stage.

S0b/S0c v2 prototype (Round 244; same throwaway Python, now with system headers
followed, `#pragma once` as a synthetic guard, threshold atoms `GE(X,c)` with
implications conjoined into one constraint BDD, OS and compiler family
constraints, a block-contiguous variable order found by retrying the fold with
the names seen so far, and the phased cost DP of 3.8). Node cap 3,000,000 per
unit. `SD` is the system-include depth cap (quoted includes are always
followed). Exactness = no OPAQUE atom in the group's support, system-header
macros now being real definitions.

| Corpus, SD | Units over cap | Fold | Cost queries | Max nodes, one group | Max table, one unit | Exact |
|---|---|---|---|---|---|---|
| libuv, 1 | 0 of 62 | 3.3 s | 5.7 s | 504 | 137,941 | 92% |
| Lua 5.4, 1 | 0 of 35 | 4.5 s | 8.5 s | 10,113 | 809,536 | 78% |
| zstd, 1 | 1 of 1 | over cap (16 s) | none | none | over 3M | none |
| libuv, 2 | 5 of 62 | 68 s | 170 s | 75,912 | 2,972,063 | 83% |
| Lua 5.4, 2 | 1 of 35 | 44 s | 96 s | 63,644 | 2,204,083 | 72% |
| libuv, unlimited | first unit over cap after about 1,000 files and 3,600 variables | | | | | |

Findings:

- **Following system headers is the blow-up driver, not the BDD.** On three
  libuv units the table was about 15k nodes at SD=1, about 200k at SD=2 and over
  3M at SD=3 or unlimited. Quoted-only closure (v1) was tiny. Decision 1 (BDD)
  stands only together with a **system-include depth cap of 1** (a system header
  included directly by a user file is folded; what it includes is not), which
  is the depth rule 3.4 already allows; deeper system macros become `environment`
  atoms.
- With SD=1 the S0b stop rule (under 20,000 nodes per group, under 10 s
  including the cost query) holds for libuv (9.0 s) and marginally fails for Lua
  (13.0 s, though the per-unit maximum is 0.4 s fold); zstd (one amalgamated
  unit that inlines many headers) still overflows. Cause for zstd not
  isolated; candidates are the constraint conjunction against thousands of
  `GE` atoms and the order. Mitigation to test in S1: keep `C` out of the
  per-group BDDs and apply it at query time.
- **Honest exactness** is 92% (libuv) and 78% (Lua) at SD=1, below the S0c
  target of 90% for Lua. The 8% to 22% inexact groups are function-like macro
  calls, `__has_include`, shifts, and arithmetic; Lua also has 665 computed
  includes.
- **Constraints matter:** 17% of libuv groups (8,099 of 47,415) and 26% of Lua
  groups are unsatisfiable once family and threshold constraints are applied,
  i.e. dead under any valuation, which the constraint-free v1 counted as
  satisfiable.
- **Phase distribution** (nonconstant groups inactive under the host
  valuation, SD=1): libuv 28,806, of which 19,341 (67%) have a phase-1 overlay
  cube and 9,465 (33%) need a target change (phase 2); none was unsatisfiable.
  Phase-1 cost histogram (deviated blocks): 1: 15,249, 2: 3,381, 3: 638, 4 or
  more: 73. Lua: 16,937 inactive, 14,124 phase 1 (83%), 2,813 phase 2; costs
  1: 6,140, 2: 3,194, 3: 1,811, 4 or more: 2,979. The cost query runs at about
  0.2 ms per inactive group.
- Not covered: rank 2 and 3 cubes, `cover`, the `unknown` versus `impossible`
  split, and any measurement of the cost query's guarantee (only the
  rank-1 DP was built).

S0b/S0c v2b (Round 244, same prototype): the constraint BDD is no longer conjoined
into every group. The constraints are kept as a list of small clauses and each
group is conjoined only with the clauses that share a variable with its support
(direct sharing only, no transitive closure; the unsatisfiable-group counts are
identical to the global-conjunction run at SD=1, so nothing was lost there).

| Corpus, SD | Units over cap | Fold | Cost queries | Max nodes, one group | Max table, one unit | Exact |
|---|---|---|---|---|---|---|
| libuv, 1 | 0 of 62 | 3.9 s | 1.0 s | 166 | 186,651 | 92% |
| Lua 5.4, 1 | 0 of 35 | 4.2 s | 0.6 s | 179 | 70,825 | 78% |
| zstd, 1 | 0 of 1 | 14.6 s | 0.8 s | 5,362 | 2,863,026 | 76% |
| libuv, 2 | 0 of 62 | 48.6 s | 10.9 s | 10,996 | 2,485,252 | 82% |
| Lua 5.4, 2 | 0 of 35 | 32.0 s | 7.4 s | 3,586 | 1,901,347 | 72% |

- **The constraints, not the BDD, caused the second blow-up.** Max nodes per
  group fell 3x (libuv, 504 to 166) and 56x (Lua, 10,113 to 179), the cost
  queries 5x to 14x, and zstd and every depth-2 unit now fit under the cap.
- S0b's stop rule (under 20,000 nodes per group, under 10 s with the cost query)
  now passes at SD=1 for libuv (4.9 s) and Lua (4.8 s); zstd takes 15.4 s and
  its table is close to the cap, so it fails the time limit. At SD=2 groups
  stay under 20,000 nodes but the **fold** (up to 12 s for one unit) exceeds the
  budget; depth 1 stays the recommendation.
- zstd's cost is the fold of its 67 folded files (1,576 conditions, 76% exact),
  not the queries; the fold is what a stage-S1 implementation must make cheap
  (hash-consed shared PCs per inclusion instance).
- Phase counts (inactive groups needing an overlay versus a target change) at
  SD=1 are unchanged: libuv 19,341 and 9,465, Lua 14,124 and 2,813; zstd 1,685
  and 862 of 2,547.

S0b/S0c v2c (Round 245, same prototype): ranks 2 to 3, `cover`, the fold profile,
and system headers folded **host-concretely** (conditions inside a system
header evaluated under the host valuation, its groups not recorded, definitions
guarded by the including point's PC) at unlimited depth. Depth-1 symbolic runs
of v2b are the comparison.

Fold profile (zstd, depth 1, symbolic): per pass the table holds 2.3M nodes, but
only 38% are live at the end (libuv units 21% to 25%): a manager without garbage
collection wastes most of its table. The cost is concentrated: `stddef.h`
alone is included 10 times per pass and each inclusion adds 0.45M to 0.54M
nodes (`__need_*` multi-inclusion macros); in Lua's `onelua.c`, 834 of 1,110
inclusions add 0 nodes because the guard makes the re-inclusion empty. So
sharing PCs per inclusion instance would help little (re-inclusions are already
cheap where a guard exists); the cost is symbolic folding of compiler and libc
headers.

| Corpus (host-concrete system headers, unlimited depth) | Units over cap | Fold (two passes) | Max nodes, one group | Max table, one unit | Exact | Groups |
|---|---|---|---|---|---|---|
| libuv | 0 of 62 | 50.5 s (slowest unit 1.3 s) | 119 | 183,410 | 99% | 30,839 |
| Lua 5.4 | 0 of 35 | 20.4 s (slowest unit 2.1 s) | 127 | 71,866 | 86% | 19,279 |
| zstd | 0 of 1 | 1.6 s | 124 | 40,660 | 85% | 1,145 |

(For zstd symbolic at depth 1 the fold was 14.6 s with 2.86M nodes and 76%
exact; the group counts now equal v1's because system-header groups are no longer
recorded.) Fold time is now dominated by the number of folded files (libuv 31,281
inclusions, 29,836 of system headers), not by BDD size. S0b's stop rule
(under 20,000 nodes per group) holds with a 100x margin; S0c's "under 1 s per
entry" holds on average for Lua (0.6 s) and libuv (0.8 s) but not for their
slowest units (2.1 s, 1.3 s) or zstd (1.6 s), all two-pass Python figures;
exactness is above 90% only for libuv (the user-file closure was
not separated, so Lua's 86% includes its 665 computed includes). The depth cap
is no longer needed.

**Ranks and `cover`.** Rank-1 cost from the path enumeration equals the DP cost
for every phase-1 group (0 mismatches in 19,341 + 14,124 + 1,685 groups) **once
the variable order is complete before the final fold**: with variables created
lazily during the fold the blocks are not contiguous and the DP overcounted
(cost 2 for a cost-1 cube); the two-pass fold fixes it and is a requirement of
the order rule. The path cap (64) was never reached.

| Corpus | Phase-1 groups | With at least 2 primes | With at least 3 | `cover`: overlays per unit (mean, max) | Units needing 4 or fewer | Groups covered by the first 4 |
|---|---|---|---|---|---|---|
| libuv, host-concrete | 1,306 | 73 (6%) | 8 (0.6%) | 5.9, 6 | 1 of 62 | 1,185 of 1,306 (91%) |
| Lua, host-concrete | 8,861 | 5,724 (65%) | 3,829 (43%) | 17.3, 51 | 3 of 35 | 4,090 of 8,861 (46%) |
| zstd, host-concrete | 280 | 100 (36%) | 57 (20%) | 5.0, 5 | 0 of 1 | 278 of 280 (99%) |
| libuv, depth-1 symbolic | 19,341 | 4,122 (21%) | 695 (3.6%) | 6.5, 15 | 1 of 62 | 18,927 of 19,341 (98%) |
| Lua, depth-1 symbolic | 14,124 | 7,637 (54%) | 4,636 (33%) | 18.0, 51 | 3 of 35 | 9,193 of 14,124 (65%) |

- The number of phase-1 groups drops 15-fold on libuv once system headers are
  concrete (19,341 to 1,306), and the phase split becomes what S0a predicted:
  10,782 of 12,088 inactive libuv groups (89%) need a target change (phase 2).
  The depth-1 symbolic figure counted system-header groups, which are noise for a
  user-facing question.
- **The "at most 4 overlays per entry" budget of section 4 is too small for
  Lua** (46% of its phase-1 groups covered by 4) and is just enough for libuv
  (91%). Lua's groups are mostly independent language-feature switches
  (`LUA_COMPAT_*`, `LUA_USE_*`), each needing its own overlay. Either raise the
  budget per entry (cost: more virtual entries) or accept that `cover` is a
  best-effort tool and report the uncovered share.
- `cover` as measured is greedy over the 1,000 most frequent group cubes with
  merging of non-conflicting cubes; it covered every phase-1 group eventually (0
  uncoverable) except 58 of 1,685 in zstd at depth 1, which disappear under
  host-concrete headers.
- Not covered: `unknown` versus `impossible` for phase-2 groups and ranks beyond
  the path cap; the prototype's `cover` timings (1.7 s libuv, 170 s Lua) include
  a regex per host-valuation lookup that a real implementation would precompute.

**Decisions from the v2c data (Round 246).**

- *Overlay budget.* Coverage of phase-1 groups by the first k greedy overlays
  (host-concrete system headers):

  | k | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
  |---|---|---|---|---|---|---|---|
  | libuv (1,306) | 71% | 81% | 91% | 100% | 100% | 100% | 100% |
  | Lua (8,861) | 18% | 29% | 46% | 69% | 90% | 98% | 100% |
  | zstd (280) | 67% | 90% | 99% | 100% | 100% | 100% | 100% |

  The default per-entry budget B becomes **8** (libuv and zstd fully covered,
  Lua 69%), configurable as `cereal.index.maxOverlays` up to 64. `cover` always
  reports the uncovered share per entry, and an uncovered group gets an
  explanation but no virtual unit (the same path as a `target` group). The global
  `cereal.index.configs` ratio still bounds the total (libuv at B=8 needs about
  6 virtual units per real one, so the proposed on-value 0.5 would serve only
  the largest-gain overlays; on-value to be set in S2 from a measured build cost).
- *Exactness is conditional on skipped includes.* Exactness (no OPAQUE atom in
  the group's support) is 99% (libuv), 86% (Lua) and 85% (zstd), but a computed
  or unresolved quoted include that the fold cannot follow means every later
  group is exact only **in worlds where that include is not taken**: libuv 1,703
  and Lua 8,683 of the exact groups follow one (35 active computed includes in
  Lua, 1,025 unresolved in libuv, mostly missing platform headers); only 14% of
  Lua's exact groups precede all of them. The right model is not per-group
  inexactness but an **assumption set** A per unit: each skipped include
  contributes `not cur` (its edge PC), PCs and queries are exact under A, and
  `enable`/`why_off` print A ("assuming `LUA_USER_H` is undefined"). A group whose
  PC implies `cur` is inexact. S1 stores A with the unit; the S0c rule "90% exact
  on the user-file closure" is restated as "90% of groups exact, and A listed".
  Causes of true inexactness (no OPAQUE-free PC): libuv 286 groups (176 other, 110
  `__has_*`), Lua 1,757 (1,681 other, 72 function-like calls, 4 shifts), zstd
  121 (72 `__has_*`); "other" is mostly arithmetic and macros defined by
  non-numeric bodies and is the first thing S1 should break down.

**The "other" inexact bucket, broken down (Round 247).** Lua's 1,681 "other" groups
(1,406 of them depend on one shared opaque atom) come from two conditions that
the prototype's evaluator cannot read: `__FLT_EVAL_METHOD__ == -1` (unary
minus) and `'A' == 65 && '0' == 48` (character constants), each 17 times; about
250 more compare macros that alias another macro (`LUA_INT_TYPE == LUA_INT_INT`);
a handful are arithmetic on shift results (`(b) >= 3`). libuv's 176 are
`NSIG <= SIGWINCH` (122) and `O_NDELAY != O_NONBLOCK` (53), comparisons between two
macros whose values the host knows. None needs new theory. S1's condition
evaluator gets three rules: (a) integer literals include unary minus and
character constants; (b) an object-like macro whose body is a single identifier
or integer is chased to its value; (c) a comparison of two integer atoms is a
case split over their value tables (the same table `numcmp` already walks). With
them the remaining true inexactness is `__has_*` and function-like macro calls.
The prototype also named opaque atoms by text, so one unreadable condition made
every dependent group look inexact; S1 names an opaque atom per directive.

