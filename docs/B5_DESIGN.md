# B5: workspace-wide index over compile_commands

Status: design, 2026-10-12. Not started. ROADMAP B5 points here.

Scope (ROADMAP B5): a background index of every C entry in
`compile_commands.json`, persisted on disk, answering:

- workspace/symbol over the whole project;
- cross-file references;
- cross-file incoming and outgoing calls;
- definition and declaration across TUs (cheap once the above exists);
- rename across TUs, last.

Line citations are to HEAD d2aa55b unless noted. Another agent's uncommitted
G1 edit touched `src/c/csymidx.c`, `src/lsp/{features.c,lsp.h,server.c}` on
2026-10-12; nothing here depends on it.

Contents:

1. Goals and non-goals
2. Decisions (user D1-D8, design E1-E14)
3. What exists today
4. Prerequisites (D5 and what else B5 needs first)
5. Summary schema and file layout
6. The store: cache directory, writes, eviction, several servers
7. Invalidation
8. Scheduler
9. Subprocess fallback
10. Query layer
11. Features
12. Edge cases
13. Progress and settings
14. Testing
15. Slices, estimates and stop rules
16. Risks
17. Open questions
18. Parked
19. Review log

---

## 1. Goals and non-goals

Goals:

- **Whole-project answers.** Every C entry of the compile DB is indexed
  in the background. Answers cover files the user never opened.
- **Persistence.** A restart re-validates the cache with `stat` and hashes
  rather than re-checking everything.
- **Open units stay authoritative.** An open file's live index (B2/B3)
  keeps answering everything it answers today, with today's policies.
  Project data adds files the open units do not see.
- **The editor stays responsive.** Indexing never delays the builder
  thread's diagnostics or the protocol thread (D4).
- **Bounded memory.** Per-job RSS is checked, oversize TUs go out of
  process, and the in-memory merged index for 1,000 TUs stays under
  100 MB (stop rule, 15).

Non-goals:

- **Type hierarchy (D7).** Dropped.
- **Full event streams for non-open files (D3).** Hover, semantic tokens,
  inlay hints and highlights stay open-unit features.
- **Indexing unsaved text of files that are not open.** The background
  index reads disk; open units overlay (E3).
- **C++.** Only entries that `entry_is_c` accepts are indexed (compdb.h).

## 2. Decisions

### User decisions (2026-10-12; recorded, not reopened)

**D1. Process model: hybrid.**

- An in-process indexing pool runs min(cores/4, 4) threads at low
  priority.
- Each job runs under a FatalTrap (`ThreadPool.trap_fatal`, thread.c:103)
  and is cancelled on edits.
- A TU falls back to a subprocess (`cereal index-summary FILE`) in three
  cases: it exceeds a size threshold, it fails, or it crashes.

**D2. Summary format.**

- Binary.
- One summary per file, keyed by (path, content hash, flag hash).
- Shared headers are stored once. This design reads D2 as: one summary
  per TU (its main file), with header data content-addressed so each
  distinct header record is stored once (5.1, assumption A1 in 17).

**D3. Content: exports only.**

- A summary holds:
  - external definitions and declarations;
  - call triples keyed by (name, linkage);
  - references by identity key.
- It does not hold the full event stream. Full events stay for open units.

**D4. Priority.** Pool and priority as in D1. Indexing never starves the
builder or the protocol threads.

**D5. First step.**

- Fix the `localtime` race in `pp_options_finish` (pp.c:706-722) with
  `localtime_r`, or with an inherited stamp as compdb replay does
  (main.c:383-384 in HEAD, cited as 380-381 in the brief).
- Update the stale "static state" comments: frontend.h:39-40,
  server.c:57-59 and B2_DESIGN.md:976. This design adds two more, in 4.1.
- Add a gate check that fails on a new mutable non-TLS global, using `nm`
  or `objdump` over the objects. It must be an allowlist; see 4.1 for why
  "none at all" cannot hold.

**D6. Cross-TU rename comes last.**

- Each affected TU gets a re-check gate (the crename.c method).
- A symbol whose header offset is ambiguous across TUs is refused.

**D7. Type hierarchy is dropped.**

**D8. Identity keys across TUs.**

| What | Key |
|---|---|
| Functions and objects | (name, kind, external linkage) |
| Types, fields and tags | (tag name, defining file + offset) |
| Static and inline-in-header declarations | (declaration file, offset) |
| Events in shared headers | deduplicated by (file, offset, len) |

### Design decisions (this document)

| # | Decision | Section |
|---|---|---|
| E1 | Index jobs get their own `ThreadPool` (`S.ipool`). They never go on `S.pool`, because `group_wait` runs any queued job. | 4.2, 8.1 |
| E2 | `config_options_for` gains a variant that allocates in a caller's arena, so index workers can build options without a lock. | 4.3 |
| E3 | Background jobs read disk only. Open units overlay their live index at query time. | 8.4, 10.3 |
| E4 | One position mapping for all stale or dirty text: `cindex_text_edit` from the text a record was built from to the text the client sees. Positions inside the damage are dropped. | 10.3 |
| E5 | Content-addressed per-file records plus a per-TU manifest. Identical header records from different TUs share one file on disk. Differing ones are kept as variants, which is how multi-config ambiguity is detected. | 5.1, 12.1 |
| E6 | Freshness check: `stat` (size, mtime_ns) first, then the content hash. The include list is stored per TU, together with the unresolved includes. | 7 |
| E7 | One periodic `stat` sweep, plus a sweep on didSave, finds disk changes. `didChangeWatchedFiles` is parked. | 7.3, 18 |
| E8 | One byte budget per job decides in-process versus subprocess versus not indexed. The same mechanism handles over-size TUs, the RSS budget and repeated failures. | 8.5, 9 |
| E9 | The scheduler keeps its own priority queue and feeds the FIFO pool one job per free slot. | 8.2 |
| E10 | The identity key is computed by one shared function (`cindex_key`, in src/c), used both by the writer and by the server for the open unit at the cursor. | 5.3 |
| E11 | Answers union open-unit events with mapped project events, deduplicated by (path, off, len, key). This is not "open unit replaces summary". | 10.3 |
| E12 | Macros: definitions are indexed project-wide, for workspace/symbol. Macro references across TUs are parked. | 12.2 |
| E13 | One writer per cache directory, chosen by advisory `flock`. Other servers read. | 6.5 |
| E14 | The cache key includes a build id: a hash of the running executable. A rebuilt cereal re-indexes. | 5.4 |

## 3. What exists today (verified 2026-10-12)

### Index data (src/c/csymidx.h)

- **`CIndex`:**
  - events `CIdxEvent{off, decl, file:u16, len:u8, flags:u8}`, sorted by
    (file, off);
  - decls `CIdxDecl{name, hover, scope, type, parent, kind, linkage, flags}`;
  - scopes, whose roots are the external declarations;
  - `xcalls`, the string pool, and the CSR `by_decl`.
- **`CIdxFile{path, size, hash, stale, edited, dmg_*}`.**
  - The list covers only the files that have events.
  - `hash` is `cindex_hash` = `hash64(text, n, 0x63696478)`
    (csymidx.c:1259).
- **Decl ids are dense and local to one `CIndex`.** They cannot cross TUs,
  hence D8's keys.
- **Text edits (B3 12):**
  - `cindex_text_edit(a, na, b, nb, &e)` gives the common prefix and
    suffix, widened to whole identifiers (csymidx.h:206-216).
  - `cindex_edit_map` maps an old position monotonically.
  - E4 reuses both.
- **Calls:**
  - `cindex_calls_to` and `cindex_calls_in`;
  - `CIdxCall{other, other_ev, file, len, off}`;
  - caller attribution walks the scopes (`caller_of`, csymidx.c:2202).

### Running a TU

- **`frontend_run(TU*, path, FrontendOpts*)`** (frontend.h):
  - with `check` and `cidx` set, it builds the index;
  - with `cells_par == NULL` it runs the sequential path, with no
    `par_run` and no pool;
  - a set `pp.cancel` stops it at the next token.
- **The checker's state is per-thread.**
  - HISTORY Round 204 (3859-3867) made per-check mutable globals
    `__thread`, made `cfg_gen_next` atomic, and gave compdb replay one
    `__DATE__`/`__TIME__` stamp for all entries.
  - TSan reports 0 warnings: Round 204 (~3881) and fix mode at
    -j2/4/8/12 (HISTORY:4204).
- **Concurrent checks already run.**
  - `check --compile-commands -j N` runs TUs concurrently through
    `run_jobs` (main.c:423-482).
  - Entries are deduplicated by (file, fingerprint) in `compdb_plan`
    (main.c:569).

### Pool and options

- **`ThreadPool`** (thread.c):
  - a FIFO queue with no priorities;
  - `group_wait` runs *any* queued job on the waiting thread
    (thread.c:215);
  - `trap_fatal` ends only the failing job (thread.c:98-122), and records
    the failure in its `JobGroup`. `group_wait` then calls `fatal()` on
    the waiting thread (thread.h:108-110).
  - The trapped job's partial state is leaked (LSP.md:132).
  - `pool_init(p, n)` spawns n-1 workers, because the submitting thread
    is expected to help in `group_wait` (thread.c:149).
  - There is no thread-priority API, no completion callback and no
    reader-writer lock: thread.h wraps pthread mutexes and condition
    variables only.
- **The server's pool** is created with `pool_init(&S.pool, 0)` (one
  thread per core; HEAD server.c:1636) and `trap_fatal = true`.
- **`Options.fingerprint`** (compdb.c:303-323):
  - a hash of the entry's directory plus every argument for which
    `option_affects_diagnostics` holds;
  - response files are expanded first;
  - `.cereal` layers arrive as extra arguments, so they are included;
  - the server uses it as the cells' configuration key.

### Language server

- **Config** (lsp/config.c):
  - `config_init` loads `compile_commands.json` or
    `build/compile_commands.json` under the root, once. It is never
    reloaded.
  - `config_options_for` (config.c:131-151) allocates in the shared
    `c->arena`, which is not thread-safe.
  - It runs `entry_options` → `options_finish` → `pp_options_finish` on
    every call.
  - Today only the builder thread calls it (HEAD server.c:416, 617, 648,
    694).
- **The server** (src/lsp/server.c, HEAD):
  - a builder thread plus the protocol thread;
  - request policies CP_NONE/ANY/POS/WAIT/VIEW/SNAP (`REQS`, 1175-1226);
  - `DeferredReq` for work the builder must do (70-78);
  - units live until exit; didClose reschedules from disk;
  - the 4 MiB check limit, `CHECK_MAX_BYTES` (324), overridable with
    `CEREAL_LSP_CHECK_MAX`.
- **Not handled today:**
  - didSave and `$/cancelRequest` are ignored (1730);
  - responses to server requests are dropped (1664-1667);
  - there is no workDoneProgress and no `didChangeWatchedFiles`;
  - the server does send requests (`send_refresh`, around 571-593).
- **workspace/symbol** (B4c; features.c 615-812, HEAD):
  - `ws_unit` collects user macros and C decls per open unit, one per decl
    at its definition if any, filtered by `c_symbol_event`;
  - `ws_cmp` ranks;
  - output is capped at `WS_LIMIT` = 100;
  - duplicates are dropped by (path, off, name);
  - locations come from `json_location` with the candidate's snapshot.
- **Rename** (B2 Phase 4, crename.c):
  - `c_rename` checks the unit twice, as it is and with the edits through
    an overlay (`run_overlay`, 26), and compares (`differs`, 235);
  - `map_off` and `unmap_off` (89, 105) map one main-file edit list;
  - edits are main-file only: B2 D3 refuses names visible outside the file
    ("needs the project index (B5)").
- **The cell cache is in memory only** (C_DESIGN 2). There is no on-disk
  index format yet. `file_write_atomic` exists (common.c:331: temp file in
  the same directory, fsync, rename).

### csum.h summaries (TYPES.md 90-172)

- They describe each checker unit: an external declaration, not a TU.
- They carry structural 64-bit digests independent of interner ids
  (`SumEntry.type_dig`, flags such as `SF_WEAK` and `SF_INLINE`), in a
  text serialization.
- They do not hold positions or events, so they cannot replace B5
  records. A type-digest cross-TU mismatch check is parked (18).

## 4. Prerequisites

### 4.1 D5: thread-safe options, honest comments, a globals gate

**The localtime race.**

- `pp_options_finish` calls `localtime(&now)` unless `date_str` is
  already set (pp.c:706-722). `pp_init` also calls it when `date_str` is
  unset (767-768).
- In the server, every `config_options_for` reaches it, and B5 calls that
  from index workers.
- Fix: `localtime_r`/`gmtime_r` into a local `struct tm`.
- The inherited stamp is the alternative. It works only for callers that
  remember to copy it, as main.c:383-384 does, so `localtime_r` is
  recommended: it fixes every caller once.
- Update pp.h:368-370 ("Once per process, before any TU"). The server
  already breaks that rule.

**Stale comments to update.**

| Location | Stale text |
|---|---|
| frontend.h:39-40 | "the checker keeps static state, so two checks must not run at once" |
| server.c:57-59 | `RenameJob`: "the checker keeping static state" |
| B2_DESIGN.md:976 | "the checker keeps mutable static state (cattr.c, ...)" |
| lsp.h:129-133 | `lsp_check_rename`: "the checker keeps static state: two checks must not overlap" |
| LSP.md:286 | rename "keeps static state" |
| compdb.h:66 `entry_options` | "safe on any thread". False until the localtime fix. |

The true reason the builder runs these checks is ordering and resource
use: one check at a time beside the edits. Concurrent checks are safe
once the localtime race above is fixed.
The deferral itself stays (B5d moves only cross-TU rename checks; 11.5).

**The globals gate is an allowlist, not "no symbols".**

Measured with `objdump -t` over `~/cereal-t/build/**/*.o`, 2026-10-12:

| Section | Symbols | Mutable? |
|---|---|---|
| `.bss` | 30 | yes |
| `.data` | 1 | yes |
| `.data.rel.ro.local` | 109 | no: read-only after relocation (`nm` prints `d`) |
| `.tbss` + `.tdata` (TLS) | 30 + 3 | no: per thread |

The spike's TLS count was 34; this build has 33. A check writes none of
the 31 mutable symbols except through a once-init or an atomic. Each is
one of:

- once-initialized;
- atomic;
- CLI state set before threads start;
- fault-injection state;
- language-server state outside the checker, whose thread discipline B5
  does not change.

| Object | Symbols | Why it is safe |
|---|---|---|
| diag | `cfg_gen_next` | atomic |
| fixmode | `sort_pass` | qsort comparator state, CLI fix mode |
| lex | `cls`, `cls_dollar`, `tables_once` | once-init |
| main | `made.3`, `pool.2`, `buf.0`, `fe_opts`, `carry_old`, `carry_at`, `parse_cells`, `dump_symbols`, `verify_symbols`, `rsp.1` | CLI state set before threads |
| srcmgr | `ps`, `ps_once`, `lines_lock` | once-init and a lock |
| thread | `fault_site`, `fault_at`, `fault_once`, `fault_count` | fault injection |
| c/ccall | `ready.3`, `seen.2` | `pure_first_char`'s lazy table under an atomic acquire/release flag (ccall.c:1687-1697) |
| c/csymidx | `serial_counter` | atomic |
| lsp/features | `tok_cache`, `tok_next_id` | server |
| lsp/rpc | `out_fp`, `out_m` | server |
| lsp/server | `S`, `on.0` | server |

The gate:

- A script, `bench/tools/globals.sh`, lists `.data`/`.bss` symbols per
  object.
- It compares them against a checked-in `bench/tools/globals.allow`: one
  line per (object, symbol, reason).
- It fails on any symbol not in the list, and prints stale entries.
- It runs as a `gate.sh` stage.
- RELRO and TLS are out of scope: neither can race.
- The `.N` suffixes of function-local statics vary by compiler, so the
  gate matches the name with the suffix stripped.

### 4.2 A separate index pool (E1)

The index jobs need their own pool for two reasons:

- **Blocking the builder.** `group_wait` makes the waiting thread run any
  queued job. With index jobs on `S.pool`, the builder's `par_run`
  (server.c:271) could pick up a multi-second index job and stall
  diagnostics.
- **Priority.** `S.pool` runs at normal priority; the index pool must run
  low.

Design:

- `S.ipool` is a `ThreadPool` with n = min(max(cores/4, 1), 4) workers
  and `trap_fatal`. The `max(..., 1)` gives D1's formula a floor of one
  worker below 4 cores.
  - It is created with `pool_init(&S.ipool, n + 1)`, because nothing
    helps it: no thread ever calls `group_wait` on it.
- A new field `ThreadPool.nice`, applied by each worker at start with
  `setpriority(PRIO_PROCESS, gettid(), nice)`. Linux nice values are
  per thread.
- Default 10.
- Completion is observed through a new optional `JobGroup.on_done`
  callback.
  - `run_job` calls it after the pending count reaches 0, outside the
    group's lock.
  - It also runs after a trapped `fatal()`, so the scheduler sees
    `failed` and `msg` without `group_wait`, which would re-raise the
    fatal.
  - Each index job has its own group.

### 4.3 Options off the builder thread (E2)

- `config_options_for` grows the shared arena on every call, and
  `add_cereal_files` keeps argument copies there. That is a leak per call
  and is not thread-safe.
- Add `config_options_in(LspConfig *c, const char *path, Arena *a)`:
  - the same layers, with every allocation in `a`;
  - `c` is only read (`cmds`, `root`), and is immutable after
    `config_init` until a reload (7.4).
- It also reports the files it read, for `FILS` (5.2):
  - the `.cereal` layers (`add_cereal_files`, config.c:93);
  - the response files (`entry_expand`, compdb.c:206).
- The existing `config_options_for` becomes a wrapper over it, with a
  per-`Options` arena freed by `config_options_free`. This also fixes
  today's growth of the config arena on the builder.

### 4.4 Server plumbing

- **didSave.** Dispatch it to the index scheduler (8.4).
- **Responses to server requests.** Match them by id so that
  `window/workDoneProgress/create` can wait for its reply before using the
  token (13.1).
- **Thread exit order.** At shutdown the index pool is cancelled and
  joined before units are freed.

## 5. Summary schema and file layout

### 5.1 Two kinds of file (E5)

- **TU manifest:**
  - one per (main file absolute path, flag hash, build id);
  - it lists every file the TU read, with its stat data, content hash and
    record digest;
  - plus status and statistics.
  - The manifest is small, and is what the freshness check (7) reads.
- **File record:**
  - the exports located in one file as one TU saw them;
  - the file is content-addressed by the digest of its own bytes;
  - two TUs that see a header alike produce byte-identical records, and
    so share one file.
- **Records are self-contained.**
  - Every name and path is a string in the record's own table.
  - Identity keys are written out, not decl ids.
  - So identical views are byte-identical. This needs deterministic
    order: records are sorted by (off, len, key bytes).

Directory layout under the cache root (6.1):

```
<cache>/index/<project id>/
    LOCK                         flock target (6.5)
    GEN                          publish counter for readers (6.5)
    meta                         format version, root, compdb path, written once
    tu/<hh>/<tukey>.tu           manifests; tukey = hex64(hash(abs path, flag hash, build id))
    rec/<hh>/<digest>.rec        records; digest = hex64(seeded hash64 of the record bytes, 5.4)
```

- `<hh>` is the first two hex digits. Each level then holds at most a few
  hundred files at 1,000 TUs.
- The project id is `hex64(hash64(root path + "\0" + compdb path))`.

### 5.2 Binary layout (both kinds)

Little-endian throughout, with natural alignment. Each file is:

```
Header (32 bytes):
  u8  magic[8]     "CRLIDX\0\0"
  u32 bom          0x01020304 as written by the host (other byte order -> miss)
  u16 version      INDEX_FORMAT (starts at 1; bumped on any layout change)
  u16 kind         1 manifest, 2 record
  u64 build_id     E14
  u32 nsections
  u32 payload_hash hash64 of everything after the header, low 32 bits
Section directory: nsections x {u32 tag, u32 offset, u32 count, u32 bytes}
Sections, each 8-byte aligned
```

Sections are read by tag, so a reader skips unknown tags. A new
optional section therefore does not need a version bump; a changed
section does.

**Strings section (`STR `):**

- `u32` offsets into a blob of NUL-terminated strings.
- Offset 0 is "".
- Each string is stored once per file.

**Record sections:**

| Tag | Entry | Bytes |
|---|---|---|
| `FILE` | `{u32 path, u32 size, u64 content_hash}`: the file this record describes, with `cindex_hash` of its text (one entry) | 16 |
| `KEYS` | `{u32 name, u8 kind, u8 linkage, u16 kflags, u32 path, u32 off}`: the identity keys used below (5.3). `path` is 0 and `off` is 0 for external-linkage keys. `kflags` is reserved and must be 0. | 16 |
| `DECL` | `{u32 key, u32 off, u16 len, u8 role(DECL/DEF), u8 sk, u32 container, u32 ext_begin, u32 ext_end, u32 flags}`: declarations and definitions in this file. `sk` is the LSP SymbolKind; `ext_*` is the external declaration range (call hierarchy `range`); `flags` holds CIDF_*. | 28 |
| `REFS` | `{u32 key, u32 off, u8 len, u8 flags, u16 pad}`: references (CIX_* flags kept: CALL, ARG, AT_EXPANSION, MACRO_BODY). `len` 255 means "the key's name length", as in `CIdxEvent`. | 12 |
| `CALL` | `{u32 caller, u32 callee, u32 off, u8 len, u8 flags, u16 pad}`: call sites in this file, `xcalls` included (flag AT_EXPANSION) | 16 |
| `MACR` | `{u32 name, u32 off, u16 len, u8 funclike, u8 pad}`: user macro definitions in this file | 12 |

**Manifest sections:**

| Tag | Entry |
|---|---|
| `TU  ` | `{u32 main_path, u64 flag_hash, u32 status, u32 errors, u32 bytes_read, u32 index_ms, u64 started_ns, u64 written_ns}` |
| `FILS` | `{u32 path, u32 size, u64 mtime_ns, u64 content_hash, u64 record_digest, u32 flags}`: every file the TU read. That covers the sources and headers the SrcMgr loaded (virtual ones excluded) and the files read to build its options: response files and `.cereal` layers. Flags: system, user, config. `record_digest` is 0 when the file has no exports (system headers and config files always). |
| `MISS` | `{u32 spelled, u32 from_path, u32 angled}`: `#include`s that were not found (7.2) |

The manifest's `status` takes one of these values:

| Status | Meaning |
|---|---|
| `OK` | indexed |
| `PARTIAL` | indexed, with errors > 0 (a cancelled job writes nothing, 8.4) |
| `OVER_LIMIT` | not indexed: over the hard cap (8.5) |
| `FAILED` | a fatal or a crash, with a backoff count |

### 5.3 What is exported (D3) and the key function (E10)

**Exported decls.** These are the declarations that `c_symbol_event`
selects for document and workspace symbols:

- file-level declarations;
- fields and enumerators at their definition;
- excluding parameters, labels, block-scope names, macro-body events and
  system headers.

Every DECL and DEF event of an exported decl in a user file becomes a
`DECL` entry.

**References.** Every REF event in a user file that names an exported
decl.

- Main-file statics are included, so that references and calls inside
  unopened .c files are complete.
- They are a small share of the total. The spike's estimate for
  exports-only output is 20-40 MB per 1,000 TUs.

**Calls.** Every call site with a resolved caller, any linkage, from
`cindex_calls_in` over each root, plus the `xcalls`.

- All calls are kept, so that a chain of incoming calls can pass through
  a static function in another TU.

**Macros.** User macro definitions (not undefs), as `ws_unit` lists them
today.

**`cindex_key(const CIndex *ix, uint32_t d, CIdxKey *out)`** lives in
`src/c/csymidx.c`. It is the only way any code obtains a key:

| Decl | Key |
|---|---|
| `kind ∈ {FUNC, OBJ}` and `linkage == 2` | (name, kind, external); no path or offset (D8) |
| `kind ∈ {STRUCT, UNION, ENUM, TYPEDEF, FIELD, ENUMCONST}` | (name, kind, path, off) of the *anchor event* |
| everything else exported (internal linkage, statics, inline-in-header) | (name, kind, path, off) of the anchor event |

The anchor event:

- the decl's DEF event if there is one;
- else its first DECL event in (path, off) order;
- macro-body events are skipped;
- an event at an expansion point (`CIX_AT_EXPANSION`) is allowed: its
  offset is the expansion site.

A tag that is never defined anywhere has a TU-local anchor (its first
declaration). Two TUs that forward-declare it in different files get
different keys. This is accepted: there is nothing to navigate to.

D8 lists "tag name" for types. The key also keeps `kind`. It comes free
from the decl, and it lets a reader tell `struct S` from a typedef `S`
without looking up the anchor.

### 5.4 Hashes, versions and the build id (E14)

| What | How |
|---|---|
| Content hash | `cindex_hash` (hash64, seeded), as `CIdxFile.hash` already has it |
| Record digest | `hash64` of the record's bytes after the header, seeded with `build_id ^ INDEX_FORMAT`. The seed matters: a record from another build must not share a name with this build's record, or the idempotent write (6.2) would keep a file the reader rejects, and re-index forever. |
| Flag hash | `Options.fingerprint` mixed with `INDEX_FORMAT` and the target name. Covers `-D`, `-I`, `-std`, `--target`, response files and `.cereal` layers (3) |
| Build id | `hash64` of `/proc/self/exe`'s bytes, computed once on first use (a few ms for a few MB) |

- **Why the build id.** Without it a rebuilt cereal with a checker fix
  would keep serving the old answers. With it a rebuild re-indexes. During
  development that is frequent but correct.
- **Endianness.**
  - `hash64` reads native-endian words (hash.h), so the format is
    host-endian in practice.
  - It is declared little-endian, and the `bom` field makes a
    big-endian host see a miss and rebuild.
  - The cache is a local cache, never shared between hosts.
- **Collisions.** 64-bit digests over at most ~10^5 records give a
  collision probability near 10^-10. Accepted.

### 5.5 Sizes (estimates; measured in B5a)

- **Record bytes per TU** (from the spike):
  - the naive full dump is ~300 MB per 1,000 TUs;
  - the per-file record, keyed by hash, is 60-100 MB;
  - exports only, 20-40 MB.
- **This schema** is exports only, plus macro definitions and all calls.
  Expected 25-50 MB per 1,000 TUs on disk, after sharing headers.
- **Sharing gain.** Header duplication measured 2.5× for Lua and 5.9× for
  libuv; sharing removes it only where TUs see the header alike.
- **B5a measures the real number** on lua54, libuv and cereal's own src.

## 6. The store

### 6.1 Location

Searched in order:

1. The setting `cereal.index.cacheDir` (13.2).
2. `$XDG_CACHE_HOME/cereal`.
3. `$HOME/.cache/cereal`.

If none of these is writable, indexing runs memory-only:

- records are kept in memory and nothing persists;
- the subprocess path (9) writes into a private temporary directory
  (`mkdtemp` under `$TMPDIR`), which the parent reads and removes;
- a `window/logMessage` warning says so.

Directories are created with mode 0700. The cache holds source-derived
names, so other users must not read it.

The project directory sits under `index/<project id>/` (5.1).

`~/.cache/cereal-par` is only par.py's test cache; it is unrelated.

### 6.2 Writes

- **Records first, then the manifest.** Every file is written with
  `file_write_atomic` (common.c:331: temp file in the same directory,
  fsync, rename).
- **Record writes are idempotent.** If the digest's file already exists,
  the write is skipped, because a record's name is its content.
- **Write order makes crashes harmless.** A crash before the manifest
  rename leaves only unreferenced records; GC removes them (6.4).
- **Fsync cost.** One fsync per new file. At 1,000 TUs that is a few
  thousand fsyncs on a first index, spread over minutes. If it measures
  above 10% of indexing time, records batch the directory fsync (B5b
  measures).

### 6.3 Reads and corruption

A reader rejects a file on any of these:

- a short file;
- a bad magic, `bom`, version, kind or build id;
- a section out of bounds;
- a payload checksum mismatch;
- a string offset out of range;
- a key index out of range.

On rejection:

- **A record:** every manifest that references it is treated as stale and
  re-queued, and the file is deleted.
- **A manifest:** deleted and re-queued.

Corruption is logged once per file (`window/logMessage`, level Log).

Records are read with `mmap`, read-only. Validation runs once per mapping,
before any use, so a bad file cannot fault the server:

- the checksum covers the payload;
- bounds are checked against the file size.

Validation reads the whole file once, which pulls it into the page cache,
not the heap.

### 6.4 Eviction and the size cap

- **Within a project:**
  - mark the records referenced by live manifests;
  - sweep the rest, except files modified in the last 10 minutes.
  - The grace period protects a job (in-process or subprocess) that has
    written its records but not yet its manifest.
  - This runs once after the startup scan and then every 100 manifest
    writes, by the writer only (6.5).
- **Across projects:**
  - `cereal.index.cacheMaxMB` (default 2048) caps `<cache>/index`;
  - when over, whole project directories are removed, least recently used
    first;
  - recency is the mtime of `meta`, touched once per server start;
  - a directory whose `LOCK` is held by a live server is never evicted.
    It is probed with a non-blocking `flock`, so the current project is
    covered too.
- **Within the cap, per project:** if one project alone exceeds the cap,
  the oldest manifests by `written_ns` are dropped until it fits, and
  their TUs are not re-queued. This is visible in progress as "index
  capped". Not expected at 1,000 TUs and 50 MB.

### 6.5 Several servers on one project (E13)

- **Writer election.** A server takes `flock(LOCK, LOCK_EX | LOCK_NB)`.
  - The holder indexes and runs GC.
  - The others are readers. They load manifests and records but do not
    index. Their progress says "index maintained by another cereal".
- **Readers pick up changes.**
  - The writer rewrites a small `GEN` file (a counter) after each
    manifest publish.
  - Directory mtimes cannot serve here: a new file under `tu/<hh>/` does
    not change `tu/`'s mtime.
  - A reader checks `GEN` when a query arrives, at most once a second,
    and on a change re-lists the manifests.
  - A reader retries the lock every 30 s, so it takes over when the
    writer exits.
- **Races with GC are safe.** Unlinking a file that a reader has mapped
  keeps the mapping valid on Linux. A reader that fails to open a record
  GC removed treats the TU as stale.
- **Open units are per server.** Every server overlays its own open units
  regardless of role (10.3).
- **Limits.**
  - `flock` over NFS is unreliable. A second writer then only duplicates
    work, because every write is atomic and content-addressed.
  - The cost of that case is CPU, never corruption.

## 7. Invalidation

### 7.1 What makes a TU stale

A TU manifest is fresh when all of these hold:

- its build id and format version match;
- its flag hash equals the flag hash recomputed from the current compile
  DB entry and `.cereal` layers (7.4);
- every `FILS` entry is unchanged (7.2);
- no `MISS` entry now resolves (7.2).

Otherwise the TU is stale and re-queued.

**Staleness never discards old data.** Its records keep answering, mapped
through E4 where possible, until the new manifest replaces them.

### 7.2 Checking files without rebuilding (E6)

**Per-path state.** The scheduler keeps one current state per distinct
path in all `FILS` entries: `{size, mtime_ns, hash}`.

- The startup sweep seeds it. A path whose stat data equals a
  manifest's `FILS` stat data, and is not racy (below), takes that
  manifest's hash. Otherwise the path is read and hashed. Manifests that
  disagree about a path cannot both match its stat data.
- A sweep refreshes it:
  1. `stat`. If size and `mtime_ns` equal the state's, the path is
     unchanged and is not read.
  2. Otherwise read and hash it, and store the new stat data and hash.
- A deleted file gets hash 0, which never matches.

**Staleness.** A manifest is stale when any `FILS` hash differs from its
path's current hash.

- This is checked for every manifest that lists a path whose hash
  changed in the sweep.
- It is also checked for each manifest at publish. A job that read a file
  just before a didSave therefore re-queues as soon as it publishes.
- Changed stat data with an equal hash ("touch") only updates the state.
  It is written into a manifest at that manifest's next write.

**Racy stat data** (git's "racily clean" rule):

- An mtime can stay the same across two writes inside one timestamp
  tick, and a same-size rewrite in that tick would then pass step 1.
- So stat data whose `mtime_ns` is within 2 s of the time it was taken is
  never trusted. That time is the job's `started_ns` for `FILS` entries,
  and the sweep time for the per-path state.
- Step 1 is skipped for such a path and it is hashed.
- After a later sweep the stat data is old enough to trust.

**Stat before read.** The stat data must describe the bytes that were
hashed, or be older than them:

- `srcmgr_load` takes `fstat` of the open descriptor before reading, and
  keeps the time in a new `SrcFile.mtime_ns`.
- A write after the `fstat` gives a newer mtime on disk, which the next
  sweep sees.
- Stat data taken after the read would hide such a write forever.

**Unresolved includes (`MISS`).**

- A sweep resolves each spelled name against the TU's search path again.
  That path is computed lazily, only for TUs with `MISS` entries.
- If a name now resolves, the TU is stale.
- Generated headers that appear after a build are the common case.

**Shadowing is not detected.** A header newly created earlier on the
search path than the one found is missed until the TU is otherwise
re-indexed or the cache is cleared. clangd's background index has the
same gap. This is recorded as a known limit; detecting it would mean
recording every directory probed.

### 7.3 When sweeps run (E7)

Sweeps run:

- at startup, after the manifests load;
- on didSave, for that one path, immediately;
- every `cereal.index.sweepSeconds` (default 30) while the server is idle
  (no pending builds);
- when a query finds a mismatch while resolving positions (10.4).

**Cost.** The unique paths are about the number of TUs plus their user
headers, a few thousand at 1,000 TUs. Each stat is about 1 µs, so a
whole sweep takes milliseconds.

This one mechanism covers edits outside the editor: git checkout, code
generation and build steps. `didChangeWatchedFiles` would only make the
sweep faster to notice changes, so it is parked (18).

### 7.4 Compile DB changes

- **The compile DB's own stat data is part of the sweep.** On a change:
  - `config_reload` builds a new `LspConfig`;
  - it swaps the server's pointer under `S.m`;
  - the old config is freed when its holders release it. The config gets
    a reference count: index jobs and the builder each take one.
- **Then for each entry:**
  - a new entry, or an entry whose flag hash changed, is queued;
  - a removed entry's manifest is deleted, and GC follows.
- **Open units.** Each build already calls `config_options_for(&S.cfg,
  ...)`, so open units pick up the reloaded config at their next build.
  - The reload itself is new in B5b; today `config_init` runs once.
- **Response files and `.cereal` layers** are not part of the compile
  DB, but they are in `FILS` as config files (5.2). A change to one
  makes its TUs stale through 7.2, and re-indexing recomputes the flag
  hash.

### 7.5 Header records versus edits to the header

- A changed header changes its content hash.
- Every manifest that lists it is stale (7.2), and their records for it
  stop matching the new text.
- At query time those records are dropped for that file, not mapped. The
  old text is gone, so E4 has no "from" text (10.3).
- Re-indexing the TUs that include it brings the file back.
- The scheduler orders those TUs first (8.2: recent edits).

## 8. Scheduler

### 8.1 Pool and threads

- **The pool.** `S.ipool` (4.2): min(max(cores/4, 1), 4) threads at nice
  10, with `trap_fatal`. On the 16-core dev machine that is 4 threads.
- **The scheduler thread.** A lightweight thread owns the queue and the
  sweeps. It is the only thread that submits to `S.ipool`. It sleeps on a
  condition variable.
- **Neither the builder nor the protocol thread ever waits on index
  work.** Queries read the committed in-memory index under a
  reader-writer lock (10.1).

### 8.2 The set of TUs, and queue order (E9)

**The set of TUs.** The project's TUs are the compile DB entries that:

- pass `entry_is_c`;
- pass the `paths` setting (13.2);

deduplicated by (absolute file, flag hash), as `compdb_plan`
deduplicates `check --compile-commands` (main.c:569).

A file compiled under two distinct flag sets is two TUs. This differs
from open units, which use the single entry `command_for` picks.

**Priority classes.** Each TU waiting for indexing has a class:

| Class | TUs |
|---|---|
| 0 | the TU of an open document's main file, or a TU whose `FILS` includes an open document |
| 1 | TUs made stale by a didSave or a sweep. Most recent first, by the change time. |
| 2 | all other stale or never-indexed TUs, in compile DB order |

An open header included everywhere puts every TU in class 0. Class 0
then degenerates to class 2's order, which is harmless.

Order within the queue:

- A binary heap orders entries on (class, recency, compile DB index).
- A TU is in the heap at most once; re-queueing updates its class.
- The scheduler submits one job per free pool slot, so the FIFO pool
  never holds more than its thread count.
- When a job completes, its `on_done` callback (4.2) signals the
  scheduler, which submits the next.

### 8.3 Job execution (in-process)

1. **Options.** `config_options_in(cfg, main, job arena)` (E2).
2. **TU.** `tu_init`, with a fresh `pp.cancel` flag the scheduler can
   set.
3. **Byte budget.** Set the per-job budget on the SrcMgr (8.5).
4. **Run.** `frontend_run(tu, main, &(FrontendOpts){.check = true,
   .cidx = &ix})`. `cells_par` is NULL, so the sequential path runs with
   no pool. The checker is the same as the server's check phase.
5. **On success or partial,** build the records and the manifest from
   `ix` and the TU's SrcMgr: every loaded file goes in `FILS` with its
   stat data taken at load (`SrcFile.mtime_ns`, 7.2) and `cindex_hash`
   of the buffer. Write them to the store (6.2), and publish them to the
   in-memory index (10.2).
6. **Free everything.** Run `cindex_free` and free the TU and its arena.

### 8.4 Reading disk only, and the edit events (E3)

**Disk only.**

- A job never takes the server's overlay.
- A file is stat'ed before reading (7.2), and the stat data goes into
  `FILS` together with the hash of the bytes actually read.
- A file rewritten during or after the read has a newer mtime on disk
  than in `FILS`. The next sweep re-hashes it and re-queues if the hash
  changed.

**Edit and lifecycle events** (D1: "cancelled on edits"):

| Event | Effect on the index |
|---|---|
| didChange | Sets `pp.cancel` on every running index job and pauses submission until the builder has been idle for 500 ms (`cereal.index.quietMs`). Cancelled jobs go back to the head of their class. Partial results of a cancelled job are discarded, never written. A running subprocess (9) is not killed: it is already at nice 10 and outside the server's memory. |
| didSave | Sweeps the saved path immediately (7.3). Its dependents go to class 1. A running job that read the old text is not cancelled: its manifest records the old hash, and the check at publish (7.2) re-queues it. |
| didOpen, didClose | Re-classify the TUs that list the path (class 0 versus not). No cancellation. |
| shutdown | Cancels all jobs and joins the pool before units are freed (4.4). |

Why cancel everything on didChange, not just the affected jobs:

- Index jobs read disk, so a didChange never makes their result wrong.
- The reason to cancel is CPU and memory for the builder (D4).
- A per-file rule would not serve that reason better.

### 8.5 One budget mechanism for memory and size (E8)

Every job has a byte budget, `B_job`, checked by the SrcMgr as files load.

**Mechanism.**

- A new `SrcMgr.max_bytes` field is checked in `srcmgr_load` against the
  running total of loaded file bytes.
- When the total exceeds it, the SrcMgr sets `over_limit` and the job's
  `pp.cancel`.
- The job ends quickly and reports "over the in-process budget".

**Thresholds.**

| Name | Default | Setting |
|---|---|---|
| In-process budget | the server's check limit (`CHECK_MAX_BYTES`, 4 MiB, or `CEREAL_LSP_CHECK_MAX`) | `cereal.index.maxInProcessKB` |
| Subprocess hard cap | 64 MiB | `cereal.index.maxFileMB` |
| RSS budget | 1536 MB | `cereal.index.memoryMB` |

**Routing.** All of these routes use the same subprocess path (9):

- **Over the in-process budget:** the TU is re-routed to a subprocess,
  with the hard cap as its budget.
- **Over the hard cap there too:** the manifest is written with
  `OVER_LIMIT`. It is not retried until a listed file changes.
- **Before each in-process submission,** the scheduler reads the
  server's anonymous RSS (`RssAnon` in `/proc/self/status`).
  - It does not use total RSS (`/proc/self/statm`). Total RSS includes
    the mapped record files (10.1), which are reclaimable page cache and
    would otherwise eat the budget.
  - It submits when RSS plus an estimate is under the RSS budget. The
    estimate is 9 MB per thread plus 3× the TU's `bytes_read` from its
    previous manifest, or the in-process budget if there is none.
  - If not, the TU goes to the subprocess path, one subprocess at a
    time.
- **The subprocess path is also the escape from glibc retention**
  (HISTORY Rounds 201-203). RSS that never comes down after in-process
  jobs pushes later jobs to subprocesses, whose memory the OS reclaims on
  exit.
- **A FatalTrap failure in-process** (the partial state leaks,
  LSP.md:132) routes that TU to a subprocess. It never retries in-process
  in this server's lifetime.
- **Open files over the check limit gain from this.** Today an open file
  above the 4 MiB check limit gets no C index. Its TU is still indexed
  through the subprocess path, so project answers (11) cover it, mapped
  like any record.

### 8.6 Errors and partial data

- **Checker errors don't stop indexing.**
  - A TU with errors still yields records.
  - The `PARTIAL` status and the error count are kept, because the index
    of erroneous code is partial by nature (ROADMAP risk).
  - Queries use partial data as is.
  - Rename refuses when any affected TU is `PARTIAL` (11.5).
- **An unopenable main file** is `FAILED`, with no retry until its stat
  data changes.
- **Repeated failures back off.** A subprocess crash or timeout marks
  the TU `FAILED` with a count. It is retried only after a listed file's
  content changes, or after the backoff has passed with no change:
  1 min, then 10 min, then never in this server's lifetime.

## 9. Subprocess fallback

### Command

```
cereal index-summary --root DIR [--compdb PATH] [--cache DIR] [--budget-mb N] FILE
cereal index-summary --dump [--root DIR] [--compdb PATH] FILE     (tests, 14.2)
```

### Child behaviour

- **Options.** The child computes its own options: `config_init`, then
  `config_options_in`. The same code as the server means the same flag
  hash.
  - The parent passes the compile DB path it loaded, so both resolve the
    same DB. `config_init(c, root)` gains an optional compile DB path for
    this.
- **Run and write.** It runs 8.3 steps 2-5 with the same writer, up to
  the store write. Publishing is the parent's.
- **Reporting.** It prints exactly one line on stdout and exits:
  - `ok <tukey>` with exit code 0;
  - or `error <message>` with exit code 1.
- **The child takes no lock.** It writes as the parent's delegate, and
  writes are atomic and content-addressed (6.2).
- **The child reads no stdin.** The parent connects stdin to /dev/null.
- **`--dump`** writes nothing to the cache. It prints the records and
  manifest as text in a stable order (14.2).

### Parent

- **Spawning.** The parent uses `posix_spawn` of `/proc/self/exe`:
  - stdin from /dev/null;
  - stdout and stderr to pipes;
  - no work between fork and exec in a threaded process.
  - Even after the binary on disk is rebuilt, `/proc/self/exe` runs the
    same build, so the build id matches (5.4).
- **Limits.** `--budget-mb N` passes the hard cap (8.5). The child
  applies its limits first thing: `SrcMgr.max_bytes` = N MiB,
  `setrlimit(RLIMIT_AS)` and `setpriority(PRIO_PROCESS, 0, 10)`.
- **Budget.** `RLIMIT_AS` is min(4 × N + 256 MB, 2 GiB).
- **Timeout.** Wall time is limited to `cereal.index.subprocessSeconds`
  (default 120); past it, SIGKILL.
- **Reaping.** A completion thread waits on the child with
  `waitpid(pid, ...)`, never `-1`, so the server's other children are
  unaffected.
- **Result.**
  - On `ok` the parent loads the manifest from the store and publishes
    it.
  - On `error` or a crash the TU is `FAILED` (8.6), and the stderr tail
    goes to `window/logMessage`.
- **Concurrency.** At most one subprocess runs at a time. It counts as
  one of the scheduler's n job slots (8.2), so in-process jobs plus the
  subprocess never exceed n.
- **Readers (6.5) never spawn.**

**Crash isolation.** A crash, OOM kill or timeout in the child affects
only that TU. The parent's state is untouched.

## 10. Query layer

### 10.1 In-memory merged index

The project index (`ProjIndex`, in a new `src/lsp/project.c`) holds:

- **Manifests.** The live manifests, one per TU, as small structs with
  their `FILS` arrays.
- **Records.** One entry per distinct record digest in use: the mapping,
  its validated section pointers, and a reference count of the manifests
  that use it.
- **The key table.** A hash map from key bytes to a `KeyEntry`:
  - name, kind, linkage and anchor;
  - postings: the record ids whose `DECL`, `REFS` or `CALL` sections
    mention the key, as small arrays.
- **The name list for workspace/symbol.** All `DECL` and `MACR` entries
  as (name pointer, record id, entry index), deduplicated by
  (path, off, name).

**What is resident.**

- The key table and the name list are built when a record is published.
- `REFS` and `CALL` sections are touched only by queries, through the
  mapping, so they stay on disk until used.

**Locking.**

- One reader-writer lock guards the index: `pthread_rwlock_t`, wrapped
  in thread.h next to `Mutex`.
- Queries take it shared.
- Publishing a TU takes it exclusive for the swap of one manifest and its
  records. Postings are prepared outside the lock.

**Size at 1,000 TUs** (estimate; stop rule 15):

| Part | Estimate |
|---|---|
| Keys | ≤ 300 k distinct, about 40 B each with postings: ~12 MB |
| Name list | ~200 k entries at 24 B: ~5 MB |
| Manifests | 1,000 × ~60 files × 40 B: ~2.4 MB |
| Mappings | file-backed and reclaimable. They count in `RssFile`, not in the heap, and not in the 8.5 budget. |

The target is under 50 MB of anonymous memory. The stop rule fires at
100 MB.

### 10.2 Publishing

- A TU's new manifest replaces its old one.
- Records no longer referenced are released.
- Postings are rebuilt only for records that are new to the index.
  Records shared with other TUs stay.
- A publish touches only the changed TU's records.

### 10.3 Open units overlay (E3, E4, E11)

**The general rule.** For a file F, the answer is the union of:

- the events about F from every open unit that indexes F, in the
  client's current text of F;
- the events about F from every live project record for F, mapped to the
  client's current text.

The union is deduplicated by (path, off, len, key).

**Mapping a record (E4).** Let T_rec be the text a record was built from
(its content hash) and T_now the text the client sees. T_now is the
overlay if F is open, else disk.

| Case | Mapping |
|---|---|
| hash(T_now) == record hash | Offsets are used directly. |
| F is open, and disk text hashes to the record hash | Map with `cindex_text_edit(disk, T_now)` and `cindex_edit_map`. Events inside the damage are dropped. |
| Otherwise | The old text is gone. Events in F from that record are dropped, and the sweep is nudged (7.3). |

**Why a union and not "open unit replaces summary"** (B4 11 said
"overriding"):

- A header seen by several configurations has record variants the open
  unit cannot see. Overriding would hide them.
- For the file the open unit indexes in its own configuration, the union
  adds nothing new once deduplicated.

**What stays stale.** Unopened TUs that include a dirty open header
still reflect the saved header, until it is saved and they are
re-indexed. This is the documented staleness of cross-TU answers (11.2).

**Keys at the cursor.**

- The cursor's decl in the open unit is converted with `cindex_key`
  (E10).
- A key's anchor offset is in the open unit's current text. It is mapped
  back to disk coordinates with the inverse edit (`cindex_text_edit(T_now,
  disk)`) when F is dirty.
- An anchor inside the damage has no project counterpart, so only
  open-unit results are given.

### 10.4 Positions at answer time

- Records hold byte offsets. LSP needs line and character in the
  negotiated encoding.
- **Open units.** Results use the snapshot's `SrcFile` line tables, as
  `json_location` does today.
- **Other files.** A small cache, `TextCache`, holds 64 files and is
  keyed by path:
  - the file's text and lazily built line starts;
  - its stat data at read time;
  - before each use, the stat data is checked.
- **A stat change** drops the entry and re-reads.
- **A text whose hash no longer matches a record** drops that record's
  events for F (10.3) and nudges the sweep.
- **One position type.** `json_location` is generalized to take a
  position source (text plus line starts) instead of a snapshot. Both
  snapshots and `TextCache` entries provide one.

This avoids storing line tables on disk. The cost is reading result files
at answer time; reads are bounded by `WS_LIMIT` and by the result count.

## 11. Features

### 11.1 workspace/symbol (extends B4c)

Sources, in this order:

1. The open units, as today (`ws_unit`).
2. The project: a scan of the name list (10.1) with `ws_match`.
   - The scan is linear over about 200 k names. At 20-100 ns per name
     that is 5-20 ms, unmeasured.
   - B5b measures it; the stop rule is p95 at 100 ms, and an n-gram
     index is parked.
   - Candidates are mapped by 10.3.

Candidates from both sources go through the same `ws_cmp`.

**Ties and duplicates.**

- Project candidates take `seq` = the number of open units, after every
  open unit.
- `ws_cmp`'s last key is `seq`. So of two candidates with the same
  (path, off, name), the open unit's sorts first, and the existing
  adjacent-duplicate check keeps it.
- Ranking itself is unchanged. Match quality, then C before macros,
  shorter, name, path and offset decide, whatever the source.
- No new comparison key is needed.

**Limits.**

- `WS_LIMIT` (100) applies after deduplication, as today.
- System headers are excluded, since records have no system files.

**Policy.** No wait, as today. Project data is whatever is published.
During the first index, results grow as indexing proceeds.

**`containerName`.** The `DECL.container` string: the struct's, union's or
enum's name for members, "" otherwise. This is today's rule (`x->parent`).

### 11.2 References (cross-file)

**Policy.** Unchanged: CP_WAIT for the open unit. The project part never
waits for indexing.

Steps:

1. Resolve the decl at the cursor in the open unit, as today, then take
   its key (10.3).
2. The open unit's references, as today.
3. Plus every `REFS`, and every `DECL` when `includeDeclaration` is set,
   from the key's postings, mapped (10.3).
4. The union is deduplicated (E11) and sorted by (path, off).

**Not covered.** Keys with no project postings, such as locals, have
only open-unit results, as today.

**Staleness**, stated in LSP.md:

- Files not open answer as of their last indexed saved text.
- Files changed on disk since are omitted until re-indexed.

### 11.3 Call hierarchy across TUs

- **Item data.** The item's `data` keeps today's `{c, name, doc}` and
  gains `"key"`: the serialized key bytes in hex. B4 11 anticipated a
  `usr` field; this is it, under D8's definition.
- **Resolution order.** Resolution stays position first: an item that
  still resolves in an open unit uses the open unit's calls, as today.
- **Incoming calls.**
  - The open unit's calls, as today, plus the `CALL` entries whose
    `callee` is the key.
  - Call sites are deduplicated by (path, off) after mapping, because
    the open unit's own TU usually also has a record (E11).
  - Results are grouped by caller key.
  - Each group gives one `CallHierarchyIncomingCall`.
  - Its `from` is built from the caller key's `DECL` (DEF preferred):
    name, `sk`, the range from `ext_begin` to `ext_end`, and the
    selection from (off, len).
  - Its `fromRanges` are the call sites, mapped.
- **Outgoing calls.**
  - If the item's function is defined in an open unit, as today.
  - Otherwise, from the `CALL` entries whose `caller` is the key, grouped
    by callee key.
- **Statics.** Static callers are keyed by (path, off) (D8), so a chain
  can pass through them.
- **A caller with no `DECL`** (should not happen) is shown with its name
  and the call site's range.
- **Order.** By (path, off), so answers are deterministic.

### 11.4 Definition and declaration across TUs

- **When.** The key has external linkage, and the open unit has nothing
  of the requested role:
  - for textDocument/definition, the unit has no DEF;
  - for textDocument/declaration, it has no DECL.
- **What.** The project's `DECL` entries of that role for the key are
  returned.
- **Other keys.** Types, statics and inline-in-header functions are
  visible to the open unit itself, so its answer stands.
- **Typical cases.**
  - A function declared in a header and defined in another .c;
  - an `extern` object.
- **Policy.** Today's CP_POS policy is kept.
- **Several definitions** (weak and strong, or several configurations)
  are all returned (12.4).
- **Cost.** This is cheap once 11.2 exists: one postings lookup.
  Included in B5c.

### 11.5 Rename across TUs (B5d; D6)

**Plan.**

1. Resolve the key at the cursor (10.3).
2. Run today's per-unit blockers (B2 A3.1-A3.10), minus B2 D3's
   "visible outside this file" refusal. That refusal ("renaming it needs
   the project index", `cindex_rename_plan`, HEAD csymidx.c:1571-1612)
   becomes the checks below.
   - With indexing disabled (`enable` false, or a reader whose writer has
     not indexed yet), D3's refusal stays as it is today.
3. Refuse if any of these holds:

| # | Condition |
|---|---|
| R1 | The key's anchor event position (path, off, len) maps to more than one key across live records (D6, 12.1). |
| R2 | The index is incomplete: some TU of the project is queued, running, stale, `FAILED` or `OVER_LIMIT`; or an affected TU is `PARTIAL`. A TU not yet indexed could reference any external name, and which TUs include a header is known only from indexed ones, so nothing weaker is sound. The message names the TUs; the `paths` setting can exclude ones that never index. (Q6) |
| R3 | A holding file's current text does not match its record and cannot be mapped (10.3 "otherwise"), or an edit position falls in a mapped file's damage. |
| R4 | Any reference to the key is in a macro body (B2's existing rule, now across TUs). |
| R5 | More than `cereal.index.renameMaxTUs` TUs are affected (default 100). |
| R6 | The new name collides, as today, in any affected TU. This is checked by the gate below. |

The refusal message names the cause and the first offending file, as B2
messages do.

**Edits.** The union of the key's `DECL` and `REFS` positions across
records, plus the open units' events, deduplicated by (path, off, len),
mapped to current text.

**Gate: per affected TU, the crename method.**

- `map_off` and `unmap_off` (crename.c:89-105) are generalized from one
  main-file edit list to one edit list per file. `CRename` gains
  `{path, offs, n}` per file instead of `main` only.
- Each affected TU (every TU listing any edited file) is checked twice:
  - as it is;
  - with the edits applied through an overlay;
  - and `differs` compares the two, as today.
- These checks run on `S.ipool`. The checker is thread-safe (D5), so
  they no longer go through the builder.
- The rename job pauses background submissions and uses all index slots.
- Progress is reported with the client's `workDoneToken` when given
  (13.1).

**Consistency (ContentModified).**

- The plan records:
  - the version of every open document that holds edits;
  - the content hash of every unopened file that holds edits.
- Before replying it rechecks both. On any change it replies
  ContentModified (-32801), as `DeferredReq` does today.
- The request is a `DeferredReq` whose work runs on the index pool. The
  reply still goes out through the protocol thread's existing path.

**Atomicity.**

- When the client supports `workspaceEdit.documentChanges` (`S.doc_changes`,
  server.c:1057):
  - the reply is a `documentChanges` list of `TextDocumentEdit`s;
  - open documents carry their version, so the client rejects a stale
    edit;
  - unopened files carry `version: null`.
- Otherwise the reply uses `changes`.
- An LSP `WorkspaceEdit` is applied by the client. Its atomicity depends
  on the client's `failureHandling`, which the server does not control.
  The versioned form is the best the protocol offers.
- After the edit, the client saves, and the sweep re-indexes.

**prepareRename** runs the plan steps only, without the gate. It refuses
early on R1-R5.

## 12. Edge cases

### 12.1 Multi-configuration headers

- **Where variants come from.** The same header seen under different
  macros by different TUs yields different records for the same
  (path, content hash).
- **Provenance.** Each variant is a separate digest; the manifests that
  list it are its provenance.
- **Ambiguity.**
  - An event position (path, off, len) that maps to different keys in
    different variants is ambiguous.
  - Rename refuses it (R1).
  - References and calls give the union.
  - workspace/symbol deduplicates by (path, off, name), so one entry
    shows.
- **Detection is lazy.** It needs the variants of one file, which come
  from `FILS` by path, plus a position lookup, done only at rename time.

The same header in the same configuration but with different inactive
branches across TUs is not an ambiguity: the union simply covers more
branches.

### 12.2 Macros (E12)

**Decision.**

- `MACR` records hold user macro definitions.
- workspace/symbol lists them, as B4c does for open units.
- Definition of a macro name from an open unit already works through the
  unit's own preprocessor.
- Macro references across TUs are parked (18). They need a macro identity
  (name plus definition position), an extension of D8 that the user has
  not decided.
- B2's macro rename stays TU-wide.

**Options considered:**

1. Definitions only (recommended). Pros: cheap, and matches B4c's
   workspace/symbol. Cons: no cross-TU macro references.
2. Definitions plus references keyed by (name, definition path and
   offset). Pros: full macro navigation. Cons: more record bytes (macro
   uses are dense), and a new key kind outside D8.
3. Nothing. Pros: smallest. Cons: workspace/symbol would regress against
   B4c for macros in unopened files.

### 12.3 Statics and same-named statics

- Keyed by anchor (path, off), so two `static int helper` in different
  .c files are different keys.
- workspace/symbol lists both.
- references and rename never mix them.
- A static inline function in a header is one key for all TUs that see
  it alike (D8). Its references span them.
- A .c file compiled by two entries yields two manifests and one shared
  record when both see it alike.

### 12.4 Weak and extern mismatches

- **Same name, kind and external linkage share one key.** This holds for
  `extern int x;` in one TU and `int x[10];` in another, so they are
  navigable together, as a linker would treat them.
- **A function and an object of the same name have different keys.**
  They are not linked together.
- **Weak plus strong definitions** both come back from definition
  requests. There is no ordering by strength: `CIdxDecl` has no weak
  flag. Adding one is cheap and parked until someone asks.
- **No type check across TUs.** Type mismatches between declarations
  are not diagnosed. csum's `type_dig` could do it later (18).

### 12.5 Files outside the compile DB

- **An open file with no entry** keeps today's behaviour (its own unit,
  lenient flags). It is not indexed in the background.
- **A header that no TU includes** has no project data.
- **Non-C entries** (`entry_is_c` false) are skipped.

### 12.6 Paths

- Paths are SrcMgr-normalized (absolute when the TU has a directory), as
  `CIdxFile.path` has them.
- URIs are made from paths the same way as today.
- Symlinked trees can produce two paths for one file. They are treated as
  two files, the same as today in open units.

## 13. Progress and settings

### 13.1 Progress

**When the client advertises `window.workDoneProgress`:**

- The scheduler sends `window/workDoneProgress/create` with a token
  `cereal-index`.
- It waits for the response (4.4).
- It then sends:
  - `$/progress` `begin` ("Indexing", cancellable: false);
  - `report` messages: "n/N TUs" plus a percentage, throttled to 2 per
    second;
  - `end` when the queue empties.
- The token is reused for later bursts, with a new begin and end each.

**Readers (6.5)** show one `begin`/`end`: "index maintained by another
cereal".

**Rename** uses the request's own `workDoneToken` when present (11.5).

**Without the capability**, there is no progress, except a
`window/logMessage` at the start and end of each burst, at Info level.

### 13.2 Settings

Settings are read from `initializationOptions.index`, as `inlayHints` is
today (server.c:1053). Environment variables override them, following the
`CEREAL_LSP_CHECK_MAX` precedent.

| Setting | Default | Meaning |
|---|---|---|
| `enable` | true if a compile DB was found | background indexing on or off; when off, B4c behaviour |
| `threads` | min(max(cores/4,1),4) | index pool size (D1) |
| `memoryMB` | 1536 | RSS budget for in-process jobs (8.5) |
| `maxInProcessKB` | check limit | per-TU in-process byte budget |
| `maxFileMB` | 64 | subprocess hard cap; beyond: not indexed |
| `cacheDir` | XDG (6.1) | cache root; "" means memory-only |
| `cacheMaxMB` | 2048 | cap over all projects (6.4) |
| `sweepSeconds` | 30 | idle stat sweep period (7.3) |
| `quietMs` | 500 | pause after the builder goes idle (8.4) |
| `subprocessSeconds` | 120 | child wall-time limit |
| `renameMaxTUs` | 100 | R5 |
| `paths` | all | include/exclude globs over compile DB entries, such as excluding `third_party/**` |

The environment variable for each setting is `CEREAL_INDEX_<NAME>`, for
example `CEREAL_INDEX_THREADS`.

## 14. Testing

### 14.1 Fixture

**`tests/lsp/proj/`** is a small multi-TU project with a checked-in
`compile_commands.json` that uses relative paths and a directory
variable the test runner fills in. It contains:

- `a.c`, `b.c` and `c.c`;
- a shared header `h.h` with an extern function and a static inline;
- a header `cfg.h` included by two entries with different `-D` values,
  for the multi-config case;
- a static of the same name in two .c files;
- a call chain through a static;
- an `#include "gen.h"` that is missing until the test creates it;
- one TU with an error;
- one TU made over-budget by a low `maxInProcessKB`.

### 14.2 Golden summaries

`cereal index-summary --dump`:

- prints the manifest and records as stable text: sorted, offsets as
  line:col and byte, keys spelled out;
- has goldens under `tests/index/*.expected`, using the existing
  `tests/query` style of runner.

The goldens cover:

- each key kind of D8;
- `xcalls`;
- macro definitions;
- `MISS`;
- `PARTIAL`.

### 14.3 Scripted LSP sessions (tests/lsp, lsp_session.py)

| Test | What it checks |
|---|---|
| `proj_wsym` | workspace/symbol finds symbols in unopened files; the open unit's candidate wins a duplicate; dedupe; limit |
| `proj_refs` | references from an open .c find uses in unopened TUs; a dirty open header maps (E4); a disk change to an unopened file drops its stale events, then re-indexing restores them |
| `proj_calls` | incoming and outgoing calls across TUs, through a static |
| `proj_def` | definition from a declaration to another TU's definition |
| `proj_rename` | accepted across three files with versioned `documentChanges`; refused for R1 (`cfg.h`), R2 (`PARTIAL`), R3, R5; ContentModified when a document changes during the gate |
| `proj_restart` | a second session on the same cache re-validates by `stat` alone. A log counter shows zero TUs re-indexed. |
| `proj_invalidate` | touch without change (stat differs, hash equal: no re-index); a content change (re-index of its dependents only); flags changed in the compile DB (re-index of that entry); `gen.h` created (`MISS` resolves) |
| `proj_cancel` | a didChange during indexing cancels and re-queues; the builder's diagnostics latency is unaffected |
| `proj_fault` | fault injection (`fault_hit`, as in tests/lsp/fault) in an index job, then the subprocess fallback succeeds; a crashing child gives `FAILED` and backoff |
| `proj_two_servers` | two servers on one cache: one writer, one reader; the reader sees the writer's new manifest |

### 14.4 Corruption

A test tool truncates, bit-flips, and changes the version or build id of
cache files between sessions. It asserts that the server re-indexes those
TUs, logs once, and never crashes.

### 14.5 TSan

- The `proj_*` sessions run under the TSan build (`san.sh`), with 4
  index threads plus the builder, edits during indexing, and a cross-TU
  rename.
- 0 reports is the gate, as for Round 204.

### 14.6 Differential against clangd

`bench/tools/clangd_diff.py` gains cross-file modes:

- references, calls and workspace/symbol over a corpus project, with
  both servers' background indexes complete;
- intended differences get new ids, P1 and up, recorded in B5's results
  section:
  - clangd indexes C++;
  - clangd lists macro references across TUs, and B5 does not (E12);
  - clangd's ranking differs;
  - the multi-config union (12.1);
  - system headers.

### 14.7 Memory and time budgets (bench)

Measured on:

- lua54 (about 35 TUs);
- libuv (39 TUs);
- cereal's own `src` (69 .c);
- a generated 1,000-TU project (`bench/tools/genproj.py`: shared headers
  with fan-in, varying `-D`).

Recorded:

- first index wall time;
- per-TU ms;
- cache bytes;
- the `ProjIndex`'s anonymous memory, by `RssAnon` before and after
  loading;
- peak RSS during indexing;
- restart re-validation time;
- workspace/symbol p50 and p95 latency;
- references latency;
- the builder's didChange-to-diagnostics latency with and without
  indexing (`lsp_stress.py`).

Timing follows LONG_RUNS.md: CPU load, Defender state and other sessions
are reported.

## 15. Slices, estimates and stop rules

Past rounds ran 2-4× over their estimates:

- B2 Phase 4: ~734 net against 350;
- B4: estimated at 400 lines in ROADMAP, then re-estimated by its design
  to 1,620-3,110;
- B4b's src landed at the top of its range (B4_DESIGN 19).

The ranges below are lines of src plus tests and tools, and already
include that history. A slice stops for review when it passes its upper
bound.

**B5a: prerequisites and the format.** 1,400-3,000 lines.

- D5: `localtime_r`, the comments (4.1), `globals.sh` and the allowlist
  as a gate stage.
- `config_options_in` (4.3).
- `SrcFile.mtime_ns` (7.2).
- `cindex_key` (5.3).
- The record and manifest writer and validating reader (5.2).
- `cereal index-summary` with `--dump`.
- Goldens (14.2) and a corruption test.
- Measure 5.5 on the corpora.
- **Stop if:**
  - exports-only cache bytes exceed 80 MB per 1,000 TUs (2× the spike's
    upper estimate);
  - or a corpus TU's records are not byte-identical across two runs
    (determinism, which E5 depends on).

**B5b: the store and the scheduler.** 2,200-4,800 lines.

- The store: location, writes, GC, eviction and the lock (6).
- Invalidation and sweeps (7), with compile DB reload.
- The index pool with nice and `on_done`, the scheduler, cancellation,
  `SrcMgr.max_bytes`, and the RSS budget (8).
- The subprocess path (9).
- `ProjIndex` with the name list (10.1-10.2).
- workspace/symbol over the project (11.1).
- Progress and settings (13).
- didSave and server-request responses (4.4).
- Tests: `proj_wsym`, `proj_restart`, `proj_invalidate`, `proj_cancel`,
  `proj_fault`, `proj_two_servers`, TSan.
- **Stop if:**
  - `ProjIndex` exceeds 100 MB of anonymous memory on the 1,000-TU
    project;
  - restart re-validation takes more than 2 s;
  - workspace/symbol p95 exceeds 100 ms;
  - or the builder's p50 latency regresses more than 10% while indexing
    runs.

**B5c: references, calls and definition.** 1,000-2,500 lines.

- The position source and `TextCache` (10.4).
- The mapping and union (10.3).
- Cross-file references (11.2).
- Call hierarchy (11.3).
- Definition and declaration (11.4).
- The clangd differential (14.6).
- **Stop if** references p95 on the 1,000-TU project exceeds 300 ms, or
  the differential shows unexplained misses above 2% of clangd's results.

**B5d: rename across TUs.** 900-2,200 lines.

- Per-file edit lists in crename.
- The plan and refusals R1-R6, the gate on the pool,
  `documentChanges`, and ContentModified.
- `proj_rename`.
- **Stop if** the gate's false refusals on corpus symbols exceed 10% of
  attempts, measured by a script that renames every external function in
  libuv to a fresh name.

**Total:** 5,500-12,500 lines over four rounds or more. B5a is the only
slice whose size is fairly certain.

## 16. Risks

1. **Header variants explode on projects with many `-D` sets.** Records
   multiply and ambiguity spreads. Mitigation: measure on the 1,000-TU
   generator with varied `-D`. The `paths` setting can exclude vendored
   trees.
2. **The build id invalidates the cache on every rebuild.** This hurts
   during cereal development, not for users. Accepted (E14); open
   question Q3.
3. **glibc retention keeps RSS high.** Mitigated by the subprocess route
   (8.5). If most work ends up in subprocesses, D1's in-process benefit
   shrinks to ~20% memory per the spike. That is acceptable, since D1
   already planned for it.
4. **Stale cross-TU answers surprise users** (a dirty header with
   unopened dependents). Documented in LSP.md. Rename refuses (R2, R3)
   rather than editing stale data.
5. **Linear workspace/symbol scans slow down past 1,000 TUs.** Stop rule
   in B5b; an n-gram index is parked.
6. **The FatalTrap leak** while many TUs fail in-process. Bounded: one
   in-process failure per TU per server lifetime (8.5).
7. **Spawning from a threaded server.** `posix_spawn` with no
   pre-exec code (9). The child sets its own limits. The TSan run covers
   the parent side.
8. **Estimates run over again** (2-4× history). The ranges already
   include it; the stop rules bound the damage.
9. **A compile DB reload while units are open** changes their flags
   mid-session. Reference counting (7.4) keeps old options alive; units
   pick up the new config at their next build.

## 17. Open questions (with recommendations)

**Assumption A1.** D2's "one per file keyed by (path, content hash, flag
hash)" is read as "one manifest per TU keyed by (main path, flag hash,
build id), with content hashes per listed file, and content-addressed
per-file records". This is the reading that also satisfies "shared
headers stored once".

**Q1. Macro references across TUs (12.2).**

1. Definitions only. Recommended.
2. Definitions and references with a macro key. Pros: full navigation.
   Cons: bytes, and a new D8 key kind.
3. Nothing. Pros: smallest. Cons: regresses workspace/symbol for macros.

**Q2. Cache location default (6.1).**

1. XDG cache directory, per user. Recommended. Pros: survives `git
   clean`, invisible to the repo. Cons: per-machine only.
2. `<root>/.cache/cereal`, clangd's choice. Pros: discoverable, easy to
   delete. Cons: shows in `git status` unless ignored.

**Q3. Build id (E14).**

1. A hash of the executable. Recommended. Pros: never serves stale
   checker output. Cons: a full re-index after every cereal rebuild.
2. `INDEX_FORMAT` alone. Pros: survives rebuilds. Cons: stale answers
   after checker fixes until the files change.

**Q4. Rename TU cap (R5).** Default 100, with `documentChanges` and
progress. Recommended: keep 100, and measure the gate's time per TU in
B5d before raising it.

**Q5. Settings transport.** `initializationOptions` plus environment
variables (recommended, today's pattern). The other option is
`workspace/didChangeConfiguration`, which needs a new handler; parked
unless asked.

**Q6. Rename requires a complete index (R2).**

1. Refuse until every TU is indexed and none failed, unless excluded by
   `paths`. Recommended. Pros: never misses an edit in an unindexed TU.
   Cons: one failing TU blocks rename of external names project-wide.
2. Allow, and warn which TUs were not covered. Pros: usable during
   indexing and with failures. Cons: can leave a broken build behind,
   against D6's intent.

## 18. Parked

| Item | Why parked |
|---|---|
| `didChangeWatchedFiles` | The stat sweep covers the same events (E7); it would only make detection faster. |
| Macro references across TUs | Q1. |
| Shadowing detection for new headers | Needs every probed directory recorded (7.2). |
| Cross-TU type mismatch diagnostics through csum `type_dig` | Not requested; csum needs `CheckOptions.summaries` (+4% check). |
| A weak flag in `CIdxDecl` to order definitions | Only cosmetic (12.4). |
| An n-gram index for workspace/symbol | Only if the B5b stop rule fires. |
| Indexing unsaved buffers of open files into the store | Open units already overlay. |
| Remote or shared caches between machines | Host-endian keys (5.4). |
| `$/cancelRequest` | Still ignored. The one long new request, a cross-TU rename, is bounded by R5 (at most `renameMaxTUs` gate checks). |
| Type hierarchy | D7. |

## 19. Review log

Adversarial rounds on 2026-10-12, each checking code claims against HEAD
d2aa55b. A round stopped at its first batch of material findings, which
were fixed before the next round started.

| Round | Caught |
|---|---|
| 1 | Line citations taken from the working copy (G1 edit), not HEAD. No completion path for index jobs (`group_wait` re-raises a trapped fatal; added `JobGroup.on_done`). `pool_init(p, n)` spawns n-1 workers. thread.h has no reader-writer lock. A wrong claim that workspace/symbol needed a new tie key (`seq` already orders duplicates). GC racing in-flight jobs (grace period). Eviction of a live project (lock probe). Stat taken after the read. The TU set and its dedupe. `REFS.len` 255. `fork` replaced by `posix_spawn`. |
| 2 | A record digest without a build-id seed (re-index loop). PARTIAL written for cancelled jobs, against 8.4. Readers had no change signal (added `GEN`; directory mtimes do not cover subdirectories). Per-path state and the check at publish. The racy-stat rule. Config files missing from `FILS`. `config_init` needs the compile DB path for the child. Open units picking up a reloaded config. Subprocess slots. `containerName`. Call-site dedupe. 11.4 wording. B5a/B5b item lists. The B4 estimate evidence. Where B2 D3's refusal lives. |
| 3 | Memory-only mode had no place for subprocess output. Cache directory mode 0700. R2 was unsound with a partial index (now requires a complete one; Q6). Open files over the check limit gain from the subprocess path. |
| 4 | Total RSS counted the mapped records (now `RssAnon`). The didClose clause. The rationale for the statics in the globals table. The rationale for `kind` in keys. |
| 5 | An unverified claim that the globals were "already guarded". The qualifier on concurrent checks being safe. E12's wording. |
| 6 | Ambiguous seeding of the per-path state. The child's step 5 went as far as publishing. |
| 7 | Full pass, uninterrupted: nothing material. Two clarifications: a running subprocess is not killed on didChange (8.4), and what `--budget-mb` carries (9). |
| 8 | Fresh-reader pass: section 13's heading did not match the contents list; the parked `$/cancelRequest` rationale named the wrong bound. |
