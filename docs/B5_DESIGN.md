# B5: workspace-wide index over compile_commands

Status: design, 2026-10-12. B5a done 2026-10-13 (record and manifest
format, writer, validating reader, `cereal index-summary`, D5 prerequisites,
goldens, sizes in 5.5); B5b-1 (store, invalidation), B5b-2 (scheduler,
subprocess, progress, settings) and B5b-3 (name list, project-wide
workspace/symbol) done 2026-10-14; B5c (references, calls, definition) done
in Round 229 and B5d (rename across TUs) done 2026-10-15 (15.2, 15.3). ROADMAP B5
points here.

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

### 5.1 Three kinds of file (E5)

- **TU manifest:**
  - one per (main file absolute path, flag hash, build id);
  - it lists every non-system file the TU read, with its stat data, content
    hash and record digest, and the digests of its system chunks (below);
  - plus status and statistics.
  - The manifest is small, and is what the freshness check (7) reads.
- **System chunk** (adopted from the Round 224 spike, 5.5):
  - the stat data of a run of system header files as one TU saw them:
    `{path, size, mtime_ns, content_hash}` per file, sorted by path;
  - content-addressed and immutable like a record, so TUs that read the
    same system headers share the same chunk files;
  - a TU's sorted system files are cut into chunks of about 16 files: a
    chunk ends after a path with `hash64(path) % 16 == 0` (content-defined,
    so one changed header alters one or two chunks, not all). The cut rule
    is not part of the file contract: a reader only follows digests;
  - a chunk has at least one entry. System files have no records.
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
    sys/<hh>/<digest>.sys        system chunks; digest as for records
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
  u16 kind         1 manifest, 2 record, 3 system chunk
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
| `FILS` | `{u32 path, u32 size, u64 mtime_ns, u64 content_hash, u64 record_digest, u32 flags}`: every file the TU read except system headers (those are in the chunks). That covers the sources and user headers the SrcMgr loaded (virtual ones excluded) and the files read to build its options: response files and `.cereal` layers. Flags: user, config, absent. `record_digest` is 0 when the file has no exports (config files always). A `.cereal` file that does not exist in a directory searched is listed with flag absent, size 0, `mtime_ns` 0 and hash 0, which is what a missing file's state is (7.2): creating it later makes the TU stale. |
| `SYSC` | `u64` chunk digests, in path order: the TU's system files, as chunks |
| `MISS` | `{u32 spelled, u32 from_path, u32 first_dir, u32 how}`: `#include`s that were not found (7.2); `first_dir` is where the search began in the unit's directory list, `how` has `angled` and `cur` (the includer's own directory was tried first) |

**Chunk section:**

| Tag | Entry |
|---|---|
| `SYST` | `{u32 path, u32 size, u64 mtime_ns, u64 content_hash}` (24 bytes), sorted by path |

The reader validates a chunk as it does a record (6.3): a chunk with no
entry is invalid.

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

The anchor event (`anchor_rank`, csymidx.c), the least (path, off) among the
events of the best rank:

- for functions, objects, typedefs, fields and enumerators: the DEF event,
  else a DECL event, else any other;
- for `struct`, `union` and `enum` tags the order of DEF and DECL is
  swapped (Round 232, F5): the first declaration, which is the same
  forward declaration in a header for every unit that includes it, so an
  opaque tag defined in one unit and used through its header in another is
  one entity;
- events in a macro body are skipped;
- an event at an expansion point (`CIX_AT_EXPANSION`) is allowed: its
  offset is the expansion site.

A tag that is never declared before its definition, and a unit that sees
only the definition (not the header's forward declaration), still anchor at
the definition; two units that see different parts differ. This is
accepted: the tag is then not the same entity to any reader of those units.
`CREC_FORMAT` is 4 since this change (keys changed meaning).

D8 lists "tag name" for types. The key also keeps `kind`. It comes free
from the decl, and it lets a reader tell `struct S` from a typedef `S`
without looking up the anchor.

### 5.4 Hashes, versions and the build id (E14)

| What | How |
|---|---|
| Content hash | `cindex_hash` (hash64, seeded), as `CIdxFile.hash` already has it |
| Record and chunk digest | `hash64` of the file's bytes after the header, seeded with `build_id ^ INDEX_FORMAT`. The seed matters: a record or chunk from another build must not share a name with this build's, or the idempotent write (6.2) would keep a file the reader rejects, and re-index forever. |
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
- **Measured in B5a** (2026-10-13, WSL, `index-summary --cache` once per TU
  into one cache; lua54 34 TUs with `-DLUA_USE_LINUX`, libuv 37 Linux TUs,
  cereal `src` 71 .c; every unit indexed with 0 errors):

  | corpus | TUs | records (shared) | records (one cache per TU) | sharing | manifests | bytes / 1000 TUs | 4 KiB blocks / 1000 TUs |
  |---|---|---|---|---|---|---|---|
  | lua54 | 34 | 1.56 MB, 237 files | 2.20 MB, 448 | 1.4x | 0.25 MB | 53 MB | 89 MB |
  | libuv | 37 | 0.49 MB, 71 files | 3.33 MB, 420 | 6.8x | 0.64 MB | 30 MB | 48 MB |
  | cereal src | 71 | 2.64 MB, 180 files | 7.91 MB, 1065 | 3.0x | 0.64 MB | 46 MB | 64 MB |

  Stop rule (exports-only bytes above 80 MB per 1000 TUs): not hit; the
  largest is 53 MB. Block rounding on the 34-TU corpus puts lua54 at 89 MB,
  so a 1000-TU cache needs the file-count effect re-measured (B5c's
  generated project). Manifests are as large as the shared records for
  libuv (one FILS entry per file read, system headers included): a
  manifest is 7-17 KB per TU. Two runs wrote byte-identical records on all
  three corpora; manifests differ only by their timings.
  Time: 0.09 s to index libuv `core.c` (13 MB RSS) against 0.03 s to
  check it; 34-71 TUs in 0.4-0.7 s at 8 processes.
- **Generated project** (2026-10-13, WSL ext4, `bench/tools/genproj.py
  OUT 1000 1`: 1,000 .c of 120-254 lines, avg 191; 50 shared headers with
  fan-in, 5-15 included per TU (avg 10) and transitive includes; 4 `-D`
  sets; 4-8 system headers per TU directly, about 113 system files read per
  TU). One `index-summary --cache` per TU, 8 processes at `nice -n 10`, one
  cache, 0 errors on all 1,000:

  | what | value |
  |---|---|
  | wall time, cold cache / second run / fresh cache | 10.5 s / 7.6 s / 8.2 s (warm page cache) |
  | files | 2,200: 1,200 records (1,000 .c + 50 headers x 4 `-D` sets) + 1,000 manifests; 507 directories |
  | bytes | 17.25 MB (records 5.85 MB, manifests 11.40 MB) |
  | 4 KiB blocks (`du`) | 23.8 MB (1.38x the bytes; 10.8 KB per file on average, so file-count overhead is mild) |
  | manifests / cache | 66% bytes (11.40 / 17.25 MB), 11.4 KB per TU (8.0-13.7 KB) |
  | FILS entries | 133 per TU (113 system, 20 user); 40 bytes each = 5.33 MB; their path strings 5.93 MB, which is all of the STR sections |
  | system-header FILS entries | 112,685 x 40 B = 4.51 MB + 5.03 MB of path strings = 9.53 MB: 84% of manifest bytes, 55% of the cache |
  | idempotence | second run on the same cache: record bytes and file count identical; manifest sizes identical, contents differ (timings) |
  | determinism | fresh cache: all 1,200 records byte-identical; all 1,000 manifests identical in size, differ in contents (timings) |

  Stop rule (80 MB per 1,000 TUs): not hit (17.25 MB, 23.8 MB in blocks).
  Manifests are 66% of the cache, above the 25% trigger for
  directory-prefix interning, because a generated TU reads about 113 system
  files while owning one user record of its own. For comparison the
  corpora in the table above had manifests of 14% (lua54), 57% (libuv) and
  20% (cereal src) of their shared cache.
- **Directory-prefix interning estimate** (computed over the 1,000
  generated manifests, not implemented): store each manifest's distinct
  directories once (11 per manifest on average, 0.35 MB in all) and for each
  FILS entry the basename plus a 4-byte directory index.
  - FILS path strings 5.93 MB become 2.38 MB (dirs 0.35, basenames 1.50,
    indexes 0.53): saves 3.55 MB, 31% of the manifests and 20.6% of the
    cache. Without the 4-byte index cost (if it packed into existing
    padding) it saves 4.08 MB.
  - After it the manifests are 7.85 MB of a 13.7 MB cache: still 57%. The
    40-byte entries (5.33 MB) and the basenames now dominate, and 84% of
    the entries are system headers.
  - So interning alone does not bring manifests under 25%. What would is
    not writing system FILS entries per TU at all (for example one shared
    system-header table per `-D`/flag hash, with the manifest holding its
    digest). That is a different design change and is not estimated here.
- **System-header sharing spike** (Round 224, 2026-10-13; throwaway
  prototype in a copy of the WSL tree; the estimate above is superseded for
  the cache layout by the recommendation, which B5b-1 implemented as
  chunked system headers).
  - **Method.** The estimates parse the real manifests of the four caches
    (the generated project and three corpora) and model each layout's file
    sizes with the 5.2 rules (header, section directory, 8-byte alignment).
    The model reproduces the current manifest sizes to the byte on all 1,168
    manifests. `index-summary --dump` was not used because it omits
    `mtime_ns`. "4K bytes" is each file rounded up to 4 KiB, directories
    excluded (so it reads about 2 MB under `du`'s 23.8 MB baseline).
    A system entry costs 24 B in shared files: `{path, size, mtime_ns,
    hash}`.
  - **Generated project, 1,000 TUs** (baseline 17,251,600 B, 21.74 MB in
    4K blocks, 2,200 files):

    | option | bytes | saved | 4K bytes saved | files | soundness vs 7.1/7.2 |
    |---|---|---|---|---|---|
    | g: per-TU system entry slimmed to 24 B (no digest/flags/pad) | 15.46 MB | 10.4% | 6.9% | 2,200 | unchanged |
    | f: per-TU entry of 16 B, no content hash | 14.56 MB | 15.6% | 11.2% | 2,200 | weaker (below) |
    | d: directory-prefix interning only | 13.70 MB | 20.6% | 16.3% | 2,200 | unchanged |
    | a: exact-set sharing (manifest holds one 8 B set digest) | 7.97 MB | 53.8% | 41.6% | 2,231 | sound; hit rate is luck |
    | b: per-header stat pool (a file per distinct entry, 8 B digest each) | 8.65 MB | 49.8% | 40.5% | 2,325 | sound |
    | c: chunked sets, cut when hash(path) % 4 = 0 | 7.98 MB | 53.8% | 41.8% | 2,254 | sound |
    | c: chunked, % 8 | 7.86 MB | 54.5% | 42.2% | 2,235 | sound |
    | c: chunked, % 16 (**measured in the prototype: 7.86 MB, 2,242 files**) | 7.83 MB model | 54.6% | 42.2% | 2,236 | sound |
    | c: chunked, % 64 | 7.88 MB | 54.3% | 41.9% | 2,232 | sound |
    | e: one append-only cache-wide table, u32 index per entry | 8.20 MB | 52.5% | 42.8% | 2,201 | needs a mutable shared file |
    | e2: the same with varint-delta indexes | 7.86 MB | 54.4% | 42.8% | 2,201 | same |

    - The generated project has 125 distinct system files in 9
      directories, and no path ever has two stat/hash tuples.
    - Sharing leaves records at 5.85 MB and the manifests at about 2 MB
      (user `FILS` and the `TU` section), so records dominate the cache
      again.
    - (a) hits 96.9% here (31 distinct sets in 1,000 TUs) only because the
      generator draws every TU's headers from 17 names. On the corpora
      below it is 26% (lua), 44% (libuv) and 72% (cereal src). A miss
      stores a whole new set of ~8 KB.
    - (c) at `% 16` and `% 32` coincide in the model (same 36 chunks): its
      boundary hash is crc32, whose low bits on 125 similar paths are
      correlated. The prototype uses `hash64`.
  - **Other corpora** (35 lua54 TUs with `-DLUA_USE_LINUX`; 62 libuv TUs,
    `src/*.c` and `src/unix/*.c` with the include paths and
    `-D_GNU_SOURCE`, some with errors; 71 cereal TUs from `src`, `src/c`,
    `src/lsp` and `src/analysis`; every one wrote its manifest. These TU
    sets differ from the table at the top of this section). Saved share,
    bytes / 4K bytes:

    | corpus | system share of cache | a | b | c (% 16) | e |
    |---|---|---|---|---|---|
    | lua54 | 11% | 4.1 / -1.2 | 9.4 / -13.7 | 6.0 / -4.6 | 10.3 / 6.7 |
    | libuv | 59% | 32.0 / 20.9 | 51.3 / -6.5 | 48.6 / 29.6 | 55.3 / 46.9 |
    | cereal src | 15% | 11.3 / 7.4 | 12.9 / -2.1 | 12.7 / 7.5 | 13.8 / 11.4 |

    A negative 4K figure means more blocks than today: each shared file
    costs a block. Chunk reuse (the share of a manifest's chunk references
    that name an already stored chunk) at `% 16` is 50% for lua54, 82% for
    libuv, 87% for cereal src and 99% for the generated project. How much
    sharing pays depends on the system share: it is large only when TUs own
    little (generated project, libuv). Chunking (c) loses at most 4.6% of
    the blocks, on the smallest corpus (lua54).
  - **Does a system header need the content hash?** Under 7.2 the hash is
    consulted only after the stat data differs (a `touch`, or a real
    change), and for racy stat data. A header whose size and `mtime_ns`
    match is already trusted unchanged without reading it, so dropping the
    hash adds no trust assumption for a header that looks unchanged. What
    dropping it costs:
    1. turn every `touch` or mtime reset (a package reinstall, a docker
       layer, `cp -p` onto a new tree) into "stale", re-indexing every TU
       that includes the header, which for libc is all of them;
    2. lose content identity in a shared file: two TUs that read different
       bytes under equal `size` and `mtime_ns` (the racy case) would share
       a chunk or a pool entry. With the hash their chunks differ.
    - It saves 8 B per entry: 0.9 MB (5.2% of the cache) when entries are
      per TU, and about 1 KB when they are shared. So: keep the hash in
      shared entries; the saving that matters is the sharing.
  - **(e), the mutable table.** Manifests would hold indexes into a shared
    file, which must be appended to by every writer.
    - It does not fit the design: jobs in the subprocess fallback (9) also
      write files, so appends need a cross-process lock, torn-tail
      detection and an fsync before the manifest rename, none of which
      the content-addressed store needs.
    - An index is only valid for one version of the table: the table can
      never shrink without renumbering, and rewriting, every manifest.
      Entries for old package versions would leak until then.
    - A damaged table stales every manifest at once.
    - Against (c) it saves nothing in bytes (e2 is 0.3% larger) and 0.6
      points of blocks. Rejected.
  - **(b) and (a).** (b) is sound but costs a file per distinct entry (125
    here, 276 for libuv, a block each) and the per-entry digest list. (a)
    is sound and the simplest, but its saving is the hit rate: every TU
    with an unusual set stores a full copy, and one new libc include in one
    TU does the same.
  - **Recommendation: (c), chunked sets at about 16 files per chunk.**
    Content-addressed and immutable like records, so it reuses the store's
    write order, idempotent writes, corruption path and GC mark-and-sweep
    unchanged. A change to one header alters the one or two chunks that hold
    it. The chunk cut rule is not part of the file contract: a reader only
    follows digests, so changing the rule later costs sharing and nothing
    else.
  - **Prototype** (throwaway copy `~/cereal-spike`; 120 lines in
    `cidxrec.{c,h}` of which about 35 are the dump merge, plus 63 in
    `indexsum.c` for the test mode `--stale DIR MANIFEST`; about 85 lines
    are the format itself). It adds a file kind `CHUNK` with section `SYST`
    (24 B entries) and a manifest section `SYSC` (u64 digests), splits the
    sorted system rows of each unit into chunks, and `crec_write` writes
    chunks before the manifest into `sys/<hh>/<digest>.sys`.
    - **Size.** 17,251,600 B to 7,864,064 B (-54.4%; records 5,853,576
      unchanged, manifests 11,398,024 to 1,972,344, 42 chunk files of
      38,144 B). `du` 23.81 MB to 14.86 MB (-37.6%). Files 2,200 to 2,242.
      Manifests are now 25% of the cache and average 2.0 KB (0.8-3.2 KB),
      so each is one 4 KiB block; further interning would save bytes but no blocks,
      and is not needed.
    - **Time.** 1,000 TUs at 8 processes, 7.3-7.5 s for a fresh cache,
      against the 8.2 s recorded above for the unmodified tree (not re-run
      side by side; indexing dominates, so a difference is not claimed).
    - **Same content.** `index-summary --dump` of 4 generated TUs and one
      TU per corpus is byte-identical to the unmodified build (the dump
      merges chunk entries back in `FILS` order).
    - **Determinism.** Two fresh caches: all 1,242 non-manifest files (1,200
      records, 42 chunks) are byte-identical; manifest bytes total the same.
    - **Changed system header.** A tiny project with two headers behind
      `-isystem` (mtimes set to 2020). `--stale` checks each `FILS` entry
      and each chunk entry as 7.2 step 1 does, then reads and hashes on a
      stat mismatch:

      | change | result |
      |---|---|
      | none | `fresh` |
      | `touch` of a system header | `touched`, not stale |
      | system header edited, size changed | `stale` (names the path) |
      | system header edited, same size, new mtime | `stale` |
      | reindex after an edit | a new chunk is written, the old one stays until GC, `fresh` |
      | referenced chunk deleted / one byte flipped | `stale chunk ...: cannot read` / `checksum mismatch` |

      A same-size edit that restores the old mtime is not seen, which is
      the existing step-1 limit, not new.
    - A pass over all 1,000 generated manifests with `--stale` (one process
      per manifest, 8 at a time) reported `fresh` for each, in 1.3 s.
  - **Soundness notes for the proposal.**
    - A chunk is named by a digest of its entries, including `mtime_ns`
      and the hash, so a manifest can only see the stat data it recorded.
    - The racy rule (7.2) stays per manifest: a chunk entry is racy for a
      manifest when its `mtime_ns` is within 2 s of that manifest's
      `started_ns`. The manifest keeps `started_ns`; chunks have no job time
      because they are shared.
    - One damaged or deleted chunk re-queues every TU that references it.
      The libc chunk is shared by nearly all of them, so this is a whole
      project's re-index (about 10 s per 1,000 TUs on the generated
      project). Rare, and recoverable, as for a widely shared header record.
    - Chunks are per project, under the project directory, so the whole-
      project eviction of 6.4 stays correct. Sharing across projects would
      need reference counting and is parked.
  - **Design edits this needed** (adopted 2026-10-13 by the user, applied
    to 5.1, 5.2, 5.4, 6.2-6.4, 7.1-7.2, 14.2, 14.4, 15 and 16 at the start
    of B5b):

    | section | edit |
    |---|---|
    | 5.1 | a third file kind, the system chunk, and `sys/<hh>/<digest>.sys` in the directory layout |
    | 5.2 | header `kind` 3 = chunk. New `SYST` (24 B: `{u32 path, u32 size, u64 mtime_ns, u64 content_hash}`, sorted by path) and manifest `SYSC` (`u64` digests). `FILS` no longer lists system files. A chunk needs at least one entry. The reader validates chunks as it does records |
    | 5.4 | the chunk digest is seeded like a record's |
    | 6.2 | write chunks with the records, then the manifest. Idempotent skip as for records |
    | 6.3 | a rejected chunk: every manifest naming it is stale and re-queued, and the file is deleted |
    | 6.4 | the mark phase also marks `SYSC` digests; the sweep covers `sys/`, with the same 10-minute grace |
    | 7.1, 7.2 | "every `FILS` entry" becomes "every `FILS` entry and every entry of its chunks"; a manifest is stale when a chunk it names is unreadable. The per-path state is seeded and compared exactly as before; the loader expands chunks into the same structure, so the sweep needs no other change. The startup scan also reads fewer bytes (about 2 MB of manifests and 38 KB of chunks instead of 11.4 MB) |
    | 14.2, 14.4 | goldens need no change (the dump is identical); the corruption test adds a flipped and a deleted chunk |
    | 15, 16 | the shared-chunk re-index in the risks; slice estimate about 100 lines plus tests |

## 6. The store

### 6.1 Location

Searched in order:

1. The setting `cereal.index.cacheDir` (13.2).
2. `$XDG_CACHE_HOME/cereal`.
3. `$HOME/.cache/cereal`.

If none of these is writable, indexing runs memory-only:

- records are kept in memory and nothing persists;
- the subprocess path (9) writes into a private temporary directory
  (`mkdtemp` under `$TMPDIR`, named `cereal-index-<pid>-XXXXXX`), which the
  parent reads and removes; the next server to need one removes those of
  dead processes (same user, pid gone), so a crash does not leak;
- the same happens when the directory is writable but `USE`, `LOCK` or
  `GEN` cannot be opened;
- a `window/logMessage` warning says so.

Directories are created with mode 0700. The cache holds source-derived
names, so other users must not read it.

The project directory sits under `index/<project id>/` (5.1). The id
hashes the root, the compile DB path and the build id, so two builds never
share (or delete) each other's manifests.

`~/.cache/cereal-par` is only par.py's test cache; it is unrelated.

### 6.2 Writes

- **Records and system chunks first, then the manifest.** Every file is
  written with `file_write_atomic` (temp file in the same directory, rename).
  The index files are not fsync'd: they carry a checksum, and a file a power
  loss leaves damaged is found by the online corruption path (6.3) and
  re-indexed.
- **Record and chunk writes are idempotent.** If the digest's file already
  exists, the write is skipped, because the name is the content.
- **Write order makes crashes harmless.** A crash before the manifest
  rename leaves only unreferenced records; GC removes them (6.4).
- **No fsync cost.** One fsync per new file would be a few thousand on a
  first index; only `cereal fix`'s writes of the user's files sync.

### 6.3 Reads and corruption

A reader rejects a file on any of these:

- a short file;
- a bad magic, `bom`, version, kind or build id;
- a section out of bounds;
- a payload checksum mismatch;
- a string offset out of range;
- a key index out of range.

On rejection:

- **A record or a system chunk:** every manifest that references it is
  treated as stale and re-queued, and the file is deleted (by the writer).
  This is one path (`pstate_reject_rec`) for the startup scan and for every
  later load, including a record that is missing: the name list loads the
  records of each manifest it publishes, so a damaged or vanished file is
  found online, not only at startup. A reader only skips and tries again
  at the next GEN change.
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
  - mark the records and system chunks (`SYSC` digests) referenced by live
    manifests;
  - sweep the rest of `rec/` and `sys/`, except files modified in the last
    10 minutes. Manifests too: a unit that left the compile DB keeps its
    manifest for that long (7.4).
  - The grace period protects a job (in-process or subprocess) that has
    written its records but not yet its manifest.
  - This runs once after the startup scan and then every 100 manifest
    writes, by the writer only (6.5).
- **Across projects:**
  - `cereal.index.cacheMaxMB` (default 2048) caps `<cache>/index`;
  - when over, whole project directories are removed, least recently used
    first;
  - recency is the mtime of `meta`, touched once per server start;
  - every server holds a shared `flock` on `USE` in its project directory;
    the evictor takes it exclusive, without blocking, and keeps it while the
    tree is removed, so a directory in use is never evicted and a server that
    opens a directory being removed waits, finds its `USE` gone and makes the
    directory again (`LOCK` stays the writer election of 6.5);
  - the cap is checked at startup, when taking over as writer, and at most
    every 10 minutes at a GC.
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
- every `FILS` entry and every entry of its system chunks is unchanged
  (7.2); a chunk it names that cannot be read makes it stale;
- no `MISS` entry now resolves (7.2).

Otherwise the TU is stale and re-queued.

**Staleness never discards old data.** Its records keep answering, mapped
through E4 where possible, until the new manifest replaces them.

### 7.2 Checking files without rebuilding (E6)

**Per-path state.** The scheduler keeps one current state per distinct
path in all `FILS` entries and chunk entries: `{size, mtime_ns, hash}`. The
loader expands a manifest's chunks into the same structure as its `FILS`
entries, so everything below applies to both. A chunk entry is racy for a
manifest when its `mtime_ns` is within 2 s of that manifest's `started_ns`
(chunks have no job time because they are shared).

- The startup sweep seeds it. A path whose stat data equals a
  manifest's `FILS` stat data, and is not racy (below), takes that
  manifest's hash. Otherwise the path is read and hashed. Manifests that
  disagree about a path cannot both match its stat data.
- A sweep refreshes it:
  1. `stat`. If size and `mtime_ns` equal the state's, the path is
     unchanged and is not read.
  2. Otherwise read and hash it, and store the new stat data and hash.
- A deleted file gets hash 0, which never matches a real file's hash. An
  absent entry (a `.cereal` that did not exist) records hash 0, so it
  matches until the file appears.

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
- This holds for the configuration files too (`.cereal` layers, response
  files): their stat data and hashes are taken before the options are built
  from them, and the options are built again if the set of files changed in
  between (`index-cfg-race` fault site; `run.sh` 22b).

**Unresolved includes (`MISS`).**

- A sweep resolves each spelled name against the TU's search path again,
  from the directory the original search began at (`first_dir`, and the
  includer's directory if `cur`): so a `#include_next` that missed is not
  searched from the start, where a wrapper header would find itself. A name
  resolves if the loader could open it (a regular file, not over the size
  limit; one predicate, `file_openable`, shared with `srcmgr`).
  That path is computed lazily, only for TUs with `MISS` entries.
- A unit that is still `MISS` right after a job that recorded it is a failure
  (backoff), not a reason to run it again at once.
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
  - a removed entry's manifest is not deleted: the compile DB may be half
    written, and GC takes the manifest after its grace period (6.4);
  - a reload that gives the same commands (a touched file) is dropped
    without swapping; the old config of a real change is released when
    its last holder lets go (`lsp_config_ref`, `lsp_config_unref`).
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
- **The child cannot outlive its server.** It gets `--parent PID` and
  `--timeout-s N`: it sets `PR_SET_PDEATHSIG` (SIGKILL), exits if the parent
  is already gone, and arms `alarm(N + 10)`; the parent also starts no child
  once it is stopping.
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
| R7 | Any reference to the key is at a macro expansion (`AT_EXPANSION`): the name is spelled by a macro invocation whose body uses it. B5 does not list those sites (Q7), so the edit list would silently miss them. R4 and R7 are one mechanism, `cindex_rename_blocker(flags)` (csymidx.h), applied to the open unit's events and to every project record event (`pq_entries` with `PqBlock`). A macro that is defined with the name but never invoked leaves no event, so neither rule sees it; the gate's re-check of the affected TUs does not either (the macro body is not compiled). This limitation is documented, not closed. |
| R5 | More than `cereal.index.renameMaxTUs` TUs are affected (default 100). |
| R6 | The new name collides, as today, in any affected TU. This is checked by the gate below. |
| R8 | An external function or object is renamed to the name of another external function or object of the project (`pq_has` on the records, whatever units read the edited files). Two definitions would clash at link time, and no unit's check sees both. (Round 232, F2) |
| R9 | An external function or object has no definition anywhere in the project (a library's): renaming its uses would break linking. (Round 232, F2) |
| R10 | The key has more than `PQ_MAX_ENTRIES` (20000) uses and declarations: the list must be whole, so a rename of that size is not attempted. (Round 232, F8) |

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
early on R1-R5 and R8-R10 (R8 needs the new name, so it is checked by the rename).

**One path (Round 232).** Every C rename, a local or a parameter as well as an
external or a static, is `project_rename`: `rename_plan` collects the places
(a local's are the unit's own events; a keyed entity's add the records'), the
gate checks the affected units (the open unit alone for a local), and the
edits are written. A name declared in a header or a file another unit
`#include`s is always planned from the records, so a unit that includes the
open unit's main file (a unity build) is an affected unit too (F3). There is
no separate unit-only check on the builder.

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
  line:col and byte, keys spelled out; the system files of the chunks are
  merged back into the file list in path order, so chunking does not show;
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
cache files between sessions (records, manifests and system chunks; a
deleted chunk is included). It asserts that the server re-indexes those
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
- **Done 2026-10-13** (about 1,750 new lines; `src/c/cidxrec.{h,c}`,
  `src/lsp/indexsum.c`, `bench/tools/globals.{sh,allow}`, `tests/index`).
  Neither stop rule fired. Deviations from sections 4-5, for B5b to build on:
  - `CRecDecl.sk` is dropped (derivable from the kind); a pad byte is left.
    `CRecTu` fields are reordered; the target name is not mixed into the
    flag hash (it is in the options already).
  - A record has no KEYS section when it has no keys. `.cereal` files that
    do not exist are not tracked in `FILS` (7.1 still needs them: B5b).
  - A callee is recorded when it is a function or an exported declaration.
  - Missing includes are collected by a `PPListener` include callback
    (`INC_NOT_FOUND`), installed after `tu_init`.
  - `config_init` takes the compile DB path (needed by the child, 9).
    `config_options_for` is now a wrapper over `config_options_in` using
    `o->rsp` as the arena; this also stopped the config arena growing per
    call. `Options` gained `rsp_files` (response files read).
  - `file_write_atomic` now creates a missing file (mode 0600).
  - `cindex_symbol_event` moved from `features.c` into `csymidx.c`.
  - `index-summary --check FILE...` validates files (used by the
    corruption test); the cache layout is `<cache>/index/<projid>/{rec,tu}`
    as in 6.1.
  - Records are byte-identical across runs; manifests carry timings and
    are not.
  - The globals gate lists 31 symbols, each justified in
    `bench/tools/globals.allow`; it runs as stage `0globals` of `gate.sh`.

**B5b-1, B5b-2 and B5b-3 done 2026-10-14** (about 2,970, 1,900 and 790 lines,
5,660 in all; `cidxstore`, `pstate`, `indexjob`, `project`, `projidx`). Deviations from sections 5-13,
for B5b-3 and later:

- Store and invalidation (B5b-1):
  - The per-project size cap (6.4) is deferred: only `cacheMaxMB` eviction.
  - Files with a BOM are always re-hashed at startup (the racy-stat rule
    cannot cover them).
  - A retry of an OVER_LIMIT unit is keyed to a listed or overflow file.
  - The child gets `--cache` and recomputes the project directory with
    `store_project_dir`.
  - `include_miss` is a new preprocessor listener hook (a `__has_include`
    miss is a dependency too); `pp_find_include` wraps the static finder.
  - `index-crec` is the only fault site (no `index-job`): the scheduler
    strips `CEREAL_FAULT` from a child's environment and passes
    `CEREAL_FAULT_CHILD` as the child's `CEREAL_FAULT`.
- Measured (B5b-2, WSL, 300-TU generated project, 100 edits 0.5 s apart on
  one open file, `quietMs` 100 so jobs overlap the edits, 3 runs each):
  builder edit-to-refresh p50 29.1/31.1/30.8 ms with indexing off and
  29.1/30.9/30.9 ms on (0%; p90 34-40 vs 35-38 ms). The 10% stop rule did not
  fire. Not measured: a heavier single-file project, indexing on all cores.
- TSan (B5b-2): all 29 LSP sessions, plain and `-fparallel`, with
  `CEREAL_INDEX_ENABLE=1`: 58 runs, 0 reports; `lsp_stress.py --symbols
  --calls --hints` 200 edits (236 sent) on a 40-TU project with the file in
  its compile database and indexing active: 0 reports; `proj_test.py`: 0
  reports. The first run found one race (the scheduler read `P.stop` unlocked
  at startup); fixed.
- Scheduler (B5b-2):
  1. Retired `LspConfig`s are kept until exit; `S.cfg` is an atomically
     swapped pointer (`cfg_now()`), so a compile DB reload never frees a
     config a build still reads.
  2. Memory-only mode (`cacheDir` ""): a private `mkdtemp` cache root.
  3. The subprocess is an ipool job: `posix_spawn`, a poll loop with the
     `subprocessSeconds` deadline, `waitpid`. No completion thread.
  4. A reader polls `GEN` once a second from the scheduler thread, and
     retries `store_lock` every `lockRetrySeconds` (default 30; taking over as
     writer and running GC).
  5. Failure state (`fails`, `retry_ns`) is in memory only. Backoff 60 s,
     600 s, then never; any content change (a sweep or save that changed a
     hash) resets all of it. This is coarser than 7.1's per-file idea,
     because a failed unit has no file list.
  6. `index-summary` gained `--entry N` and `--budget-mb M`. The child
     sets nice 10, `max_bytes`, and `RLIMIT_AS`. The limit is 4 GiB + 257 MiB
     + 4 x budget, not 4 x budget: every `SrcMgr` reserves 4 GiB + 1 MiB of
     address space, so a smaller limit cannot start (found by the first
     `memory` run). Skipped under sanitizers.
  7. An over-limit unit gets a stub OVER_LIMIT manifest (final), except in
     process: the in-process job runs with `retry_over` (no stub is written),
     so a crash cannot leave a false final stub; the unit is rerouted to a
     child with the hard cap (`maxFileMB`).
  8. Query-time mapping of project candidates (B5b-3) verifies the file
     hash and drops on mismatch; for open documents it maps through
     `cindex_text_edit`/`cindex_edit_map`.
  9. For ext-linkage keys a DECL candidate is dropped when a DEF with the
     same name and kind exists.
  10. Tests are a Python driver, `tests/proj_test.py`, which generates its
      fixture with `bench/tools/genproj.py` (12 units) instead of a static
      `tests/lsp/proj/`, and uses test-only requests `cereal/indexStats`,
      `cereal/indexWait` (a barrier) and `cereal/indexHold`. Existing LSP
      tests run with `CEREAL_INDEX_ENABLE=0`. B5b-3 may add a static
      fixture for `proj_wsym`.
  11. `cacheMaxMB` is parsed as a double.
  12. `paths` is a list of fnmatch globs (flags 0) on the root-relative
      path; a leading `!` excludes. Env form: comma separated.
  13. Selection is a linear min-scan over the units (key: class, recency,
      entry), not a heap: the set is a few thousand and the scan runs only
      when a slot is free. `PTu` embeds a `PSched` (class, pending, recent,
      sticky_sub, fails, retry_ns) instead of `void *sched`.
  14. The one job path (`index_unit`) serves both executors; a trapped
      fatal in a pool thread (`trap_fatal`) sets `sticky_sub` for the unit
      and the unit reruns in a child. A flag change through `.cereal` (the
      job's tukey differs from the unit's) triggers one `pstate_set_config`.
  15. Settings are read from `initializationOptions.index` and overridden by
      `CEREAL_INDEX_<KEY>` (13.2).
  16. (B5b-3) `ProjIndex` (`src/lsp/projidx.[ch]`) holds, per distinct record
      digest, a `PiRec`: the mapped record plus an array of 24-byte `PiSym`
      (name, container, offset, length, kind, flags) built when the record
      loads: per key its definition if any, else its first declaration;
      parameters and labels left out; macros added. Units and records are
      ref-counted (`pi_set_tu`, `pi_prune`), so a header shared by 1,000
      units is loaded once. One `RwLock`: the scheduler takes the write lock
      only to swap pointers (records load outside it); a query holds the read
      lock for the whole of one answer, because names point into the record
      mappings. Lock order: pi lock, then `S.m` (the doc-text callback);
      the scheduler never takes `S.m` or `P.m` while holding the write lock.
      It is refreshed wherever a manifest is reloaded: after a job
      (`finish`), GEN follow, takeover, startup and compile DB reload.
  17. (B5b-3) One matcher: `lsp_workspace_symbols` appends the project's
      names to the open units' candidates and sorts them together
      (`ws_match`, `ws_cmp`, `WS_LIMIT`, trigram filter for records of
      unopened files). A project candidate sorts after the open units'
      (`seq` = number of units). The dedupe by (path, offset, name) is a keyed
      set of what has been sent, keyed on the offset as placed (round 228;
      it compared the unplaced offset with the last candidate before).
  18. (B5b-3) Mapping at query time (decision 8) uses the hash of the file
      text: the client's text if the document is open, else the disk's (BOM
      stripped). Equal to the record's hash: offsets as recorded. Open
      document, disk hash equal to the record's: `cindex_text_edit(disk,
      text)` and `cindex_edit_map`, dropped if the edit overlaps the name.
      Anything else: dropped, and the scheduler is asked for a sweep
      (`EV_SWEEP`, at most one in 5 s) so the next query is right. Positions
      are made from offsets with a per-file line table (`line_table_build`,
      `line_table_pos` in pos.c, built once per file per query; the first
      `off_to_pos` scanned from the start of the file for every result,
      which was the whole cost of the slow queries, round 228).
  19. (B5b-3) Decision 9 is applied to the candidates after the matcher: an
      external-linkage function or object declaration is dropped when a
      definition with the same name and kind is among them.
  20. (B5b-3) `lockRetrySeconds` (env `CEREAL_INDEX_LOCKRETRYSECONDS`,
      default 30) replaces the constant; `proj_test.py` has `wsym` and
      `takeover` scenarios (no static `tests/lsp/proj_wsym`: the generated
      project is enough). `cereal/indexStats` gained `nameListBytes`,
      `names`, `records`.
- Measured (B5b-3, WSL, 1,001-TU generated project from `genproj.py`, 1,201
  records, 23,495 names; host 70% busy with other sessions' jobs):
  - indexing from scratch 8.5 s (4 threads); `ProjIndex` heap 1.2 MB;
    server RssAnon 38 MB at the end of indexing and 5.8 MB after a restart
    (2.3 MB with indexing off), 7.4 MB after 300 queries. Stop rule 1 (100
    MB) did not fire. Caveat: genproj headers are few and shared, so the
    name count is small; a real project at 1M names would hold about 24 MB
    of `PiSym` plus the file-backed mappings.
  - restart re-validation (spawn to all 1,001 units fresh, no job): 0.08-0.09
    s (limit 2 s).
  - workspace/symbol, 20 queries (3 to 100 results each) x 15: p50 1.7-2.1
    ms, p95 3.5-4.0 ms, max 6.5 ms (limit 100 ms). The scan is linear, about
    80 ns a name; extrapolated, p95 reaches 100 ms at roughly 1M names.
  - builder edit-to-refresh, 100 edits 0.15 s apart on `stress.c`,
    `quietMs` 50, indexing on a fresh cache and confirmed running during
    the edits (`cereal/indexStats` before and after: published 90-276 to
    480-1,001, 4 jobs running): quiet runs p50 31.4-35.3 ms on against
    31.7-32.4 ms off (0-5%). The host was busy, so single runs ranged 31-93
    ms in both modes (medians of 9 runs: 36.1 off, 44.7 on); best of N
    differs by -1%. The 10% stop rule did not fire on the quiet runs; it
    cannot be judged tighter on a shared host.
- Re-measured (Round 227, WSL, quiet(er) host, real project; measurement only,
  no src change; `./cereal` as built at 22:55 from the Round 226 tree). Host
  load: Windows `\Processor(_Total)` 17.6% before the first run (limit 20%),
  then 10-35% during runs (that includes the server and its 4 index threads;
  other sessions were intermittently active, so it was quieter than the 70% of
  the 1,001-TU run but not idle), WSL loadavg 0.3-1.7 per run; Defender
  RealTimeProtectionEnabled False. Project: a compile DB over 602 real units
  (libuv src 34 + libuv test 196, Lua 5.1-5.5 165, LuaJIT 2.0/2.1 130, zstd
  amalgamation 1, cereal's own `src` 76) with shared system headers, each unit
  also under two more flag sets (`-DCFG_A=1`; `-DNDEBUG -DCFG_B=2`), except the
  open file `libuv/src/unix/core.c`: 1,804 TUs, 2,117 distinct records, 213,903
  names (distinct name strings not counted; `index-summary --dump` takes a
  unit, not a record file). Harness: an ad hoc script (deleted with its
  scratch; it speaks `cereal/indexStats`/`indexWait` like `proj_test.py`),
  not in the repo.
  - Indexing from scratch (4 threads, default priority, `quietMs` 500 default):
    23 s (about 13 ms per TU wall); cache 32.7 MB; `ProjIndex` 6.1 MB
    (`nameListBytes`; 28.7 bytes a name); server RssAnon 68 MB at the end of
    indexing, 13 MB after a restart, 20.7 MB after 330 queries, 3.5 MB with
    indexing off. Stop rule 1 (100 MB on the 1,000-TU project): not fired
    (68 MB total is the server, of which the index is 6 MB; the builder runs in
    the latency runs below put the whole server at 83-92 MB with an open file
    being edited).
  - Restart re-validation (spawn to `indexWait` returns, 1,804 units all fresh,
    0 jobs): 0.15, 0.15, 0.18 s (limit 2 s). Not fired.
  - workspace/symbol, 22 queries (0 to 100 results) x 15, mixed prefixes,
    exact names, one-letter and no-match queries: p50 18.9 ms, p95 111.9 ms,
    max 171 ms over 330 queries (the first mix, run in the same session as the
    index build: p50 22.3, p95 145.8). Per query p50 (ms): 8.5 to 34 for 20
    of the 22 (`uv_` 17.7, `a` 26.1, `buf` 34.4, `qqq` 13.7), but `ZSTD_`
    101.5 and `ZSTD_compress` 147.8, repeatably (min over 15 runs 99.4 and
    143.5). So the scan is not the cost for these: the linear scan is about 60
    ns a name (13.7 ms for no match over 213,903 names), and the two slow
    queries are the ones with thousands of candidates (zstd.c declares about
    1,360 `ZSTD_*` names, each present in 3 configurations); cost per candidate
    after the matcher (ranking, mapping, dedupe, `ws_cmp`) looks like 20-25 us,
    not profiled. Stop rule 3 (p95 100 ms): **fires on this mix** (111.9 ms,
    two of 22 queries); without those two queries p95 is about 35 ms. The
    "p95 reaches 100 ms at about 1M names" extrapolation (linear scan) holds
    for ordinary queries (about 60-100 ns a name, 213,903 names: 14-22 ms) and
    understates candidate-heavy queries. The mix is not a user distribution;
    the zstd amalgamation is a heavy case (one file, 1,360 related names).
  - Builder edit-to-second-publication latency (the C check's
    publishDiagnostics; this client sends no semantic-token request, so no
    refresh is issued; first publication = the builder phase), 100 edits on
    `core.c` (unsaved overlay), interleaved off/on pairs, 9 pairs, fresh cache
    per on-run, `quietMs` 50, indexing confirmed active by
    `cereal/indexStats` before and after (published 0 -> 127-999 of 1,804, 4
    jobs running at the end, 221-307 jobs cancelled by edits):
    - edits 0.15 s apart: median of 9 run-p50s 65.1 ms off, 64.0 on (-1.7%);
      median p95 74.5 off, 77.0 on (+3.5%); paired per-pair p50 change: median
      +2.4%, mean +10.4%, range -19% to +57% (one pair, off 58.7 vs on 92.0,
      with the loadavg highest, drives the mean); best of N 46.5 off vs 57.3
      on (the best off was a single 46.5 ms outlier run and is not a fair
      statistic). First publication p50: 27.2 vs 27.5 ms.
    - edits 0.5 s apart (indexing runs between edits; it finishes at 25-35 s
      of the 50 s run: published 0 -> 1,804, 71-104 cancelled): median run-p50
      64.5 off, 62.2 on (-3.6%); median p95 79.4 off, 86.0 on (+8.2%); paired
      p50: median -3.1%, mean -1.7%, range -16% to +13%; paired p95 median
      -9.5%.
    - Stop rule 4 (10% p50 regression): not fired in either cadence on the
      median or the paired median; individual runs vary 55-92 ms on both arms
      (host intermittently busy), so a single-digit-percent effect cannot be
      excluded or confirmed; the data say the regression is below about 5%.
  - Sweep timer spin (checked on the reviewer's report, `project.c` about
    1039-1077): confirmed. Per-thread CPU from `/proc/PID/task/*/stat` during
    a 1-thread indexing run: one thread at 0.5-1.0 cores (the job) up to 31 s;
    from 33 s, with the default `sweepSeconds` 30 and jobs still pending, a
    second thread (the scheduler) at 0.88-1.02 cores until the work was done
    (43 s); with `sweepSeconds` 3600 that thread stays at 0.01. The 4-thread
    run finished in 20 s, before the timer, so it was not hit. Effect on
    numbers: the 0.5 s-cadence latency runs ran over 30 s of indexing, in
    arm `on` (default sweep) and a third arm `onS` (`sweepSeconds` 3600):
    median run-p50 62.2 (on) vs 69.9 (onS) vs 64.5 (off), median p95 86.0 /
    91.7 / 79.4: no visible inflation from the spin (the differences are noise,
    onS is not lower), RssAnon 88-92 MB in both arms. It costs a core of CPU
    (on a 16-core host not visible in latency; on a 4-core machine it would
    be) for the duration of any indexing longer than `sweepSeconds`.
  - Scratch: `~/rp` (project, scripts, caches) removed; `~/.cache/cereal`
    unchanged (21 entries, none from these runs: all runs used an explicit
    `cacheDir`); no leftover cereal/python/valgrind/clangd processes.
- TSan (B5b-3): 58 LSP session runs (plain and `-fparallel`, indexing on), 0
  reports; `lsp_stress.py --symbols --calls --hints` 200 edits (236 sent) on
  a 40-TU project with indexing active (workspace/symbol queries against
  the project index while jobs publish): 0 reports; all 10 `proj_test.py`
  scenarios: 0 reports.
- Gates (B5b-3): run.sh 2142 passed; san 2142, 628 files, 0 findings; callgrind
  3.8848 G; globals 32 allowed (no new global); symcov unchanged; clangd_diff
  7 files, 2186 differences (2264 before; the project names now answer some
  queries clangd answers), 0 unexpected.

**B5b: the store and the scheduler.** 2,200-4,800 lines (system chunks, 5.5
spike: about 100 more, with their tests).

- System chunks (5.1, 5.2, 6.2-6.4, 7.1-7.2), and the missing `.cereal`
  files in `FILS`.
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

**B5c: references, calls and definition.** 1,000-2,500 lines. Done in Round
229 (15.2); about 730 net new lines of `src` (`projq` 532, `features.c` 3,300
to 3,383 after deleting the B5b copies, `projidx` about 100) and about 430 of
tests and tools.

- The position source and `TextCache` (10.4).
- The mapping and union (10.3).
- Cross-file references (11.2).
- Call hierarchy (11.3).
- Definition and declaration (11.4).
- The clangd differential (14.6).
- **Stop if** references p95 on the 1,000-TU project exceeds 300 ms, or
  the differential shows unexplained misses above 2% of clangd's results.

**B5d: rename across TUs.** 900-2,200 lines. Done in Round 230 (15.3); about
1,430 lines.

- Per-file edit lists in crename.
- The plan and refusals R1-R6, the gate on the pool,
  `documentChanges`, and ContentModified.
- `proj_rename`.
- **Stop if** the gate's false refusals on corpus symbols exceed 10% of
  attempts, measured by a script that renames every external function in
  libuv to a fresh name.

**Total:** 5,500-12,500 lines over four rounds or more. B5a is the only
slice whose size is fairly certain.

### 15.1 Review fixes (Round 228)

All 15 findings of the B5b review were real and are fixed; each real defect
has a check in `tests/proj_test.py` (scenarios `spin`, `corrupt`, `config`,
`evict`, `private`, `wpos`, extended `readers`) or `tests/run.sh` 22b.

| # | Finding | Fix |
|---|---|---|
| 1 | Scheduler busy-spin once `sweepSeconds` expired mid-run | One `PSched` "route + next attempt" record per unit; the sweep deadline advances when the sweep is taken, not when quiescent. `spin` asserts the scheduler thread's CPU time stays near zero. |
| 2 | `#include_next` / `__has_include_next` misses | The MISS entry records the spelling, the including file, the start directory and how it was spelled; `miss_resolves` and the loader share one predicate (`file_loadable`). A PS_MISS straight after a successful job is a failure and backs off. |
| 3 | Corrupt or missing record not detected online | Single path: `pi_set_tu` reports, `pstate_reject_rec` runs, the unit is LOST and re-indexed (`corrupt`). `pstate_check_records` deleted. |
| 4 | Config files hashed after being read | `options_snapshot` stats the files before the options are built and retries (`index-cfg-race` fault, 22b). |
| 5 | `gc()` + `store_evict` after every burst | GC after the startup scan and every 100 manifest writes; eviction at startup and every 10 minutes (`evict`). |
| 6 | Reader reloaded every manifest per GEN change | `pstate_refresh` compares ino and mtime of each manifest; `readers` asserts `manReloads == changed`. |
| 7 | Retired `LspConfig`s unbounded; mtime-only compdb change swapped; partial compdb unlinked manifests | `LspConfig` is ref-counted (`cfg_m` leaf mutex); Options deep-copy their entry; `config_same` drops equal reloads; units that leave the config keep their manifest until GC (`config`). |
| 8 | Orphan children; private `/tmp` roots leaked; spawn after stop | `--parent PID --timeout-s N`: `PR_SET_PDEATHSIG`, `getppid` check, `alarm(N+10)`; private roots are `cereal-index-<pid>-XXXXXX`, dead pids swept (`private`); `run_child` checks `P.stop`. |
| 9 | Eviction races | Every opener holds `USE` shared; the evictor takes `USE` exclusive non-blocking and holds it while removing; `store_open` re-verifies the inode and retries. |
| 10 | `ws_drop_decls` pointers into compacting vector | A (name, kind) table. |
| 11 | `crec_parse` key kind/role | Validated. |
| 12 | workspace/symbol dedupe before placement | One keyed set `WsSent` (path, placed offset, name) after placement. |
| 13 | `finish()` keyed by tukey | Keyed by job identity. |
| 14 | `indexWait` hang with a blocked reader or writer | The barrier is "quiescent for the role" (`idle = !writer || quiescent`). |
| 15 | `store_open` failure; build id; fsync; `report_rejected` | LOCK/GEN failure falls back to memory-only; the project id includes the build id; index writes do not fsync (fix mode still does); rejects are reported at every refresh. |

Deleted: `lsp_config()`, `S.retired`, `pstate_drop_manifest` (unexported then
removed), `store_bytes` and `store_remove_tree` (now static, over one
`walk(dir, cb)` with `sweep_sub`), `is_file`, `pstate_check_records`,
`ws_read`, `stat_row`, `off_to_pos`, the three copies of `u64_cmp` /
`sort_unique_u64` (now in common.c), the `retry_over` field, `pi_all`, and
the duplicate BOM readers (one `read_file_nobom`).

Not applied, and why:
- "Edits first" is not collapsed to cancel plus one quiet timer: a timer
  cannot keep "no index job starts while the builder is mid-build longer than
  `quietMs`". `lsp_builder_idle` and `last_edit` stay.
- `join()` (malloc, cidxstore) and `path_join` (arena) are not duplicates: one
  is for code without an arena. Kept.
- Deviations: the over-limit path stat is still taken after the run; a header
  appearing during a job (PS_MISS) backs the unit off 60 s; eviction sizes are
  walked, not cached.

**Rule 3 (workspace/symbol p95), root cause.** `off_to_pos` scanned the file from
its start for every result position, so queries with thousands of matches in
big files cost O(results x file size) (perf: about 60% of samples). Fix: a
per-file `LineTable` (line starts, binary search, then a column scan of one
line), built lazily per file per query. Repro harness `bench/tools/wsym_bench.py`
(1,770 TUs, 219,087 names, 22 queries, index 9.3 s): before p50 -, p95 85.5 ms,
max 125.2 ms (`ZSTD_` 84.5, `ZSTD_compress` 121.9); after p50 13.1 ms, p95
17.2 ms, max 20.9 ms (`ZSTD_` 12.4, `ZSTD_compress` 8.3). The stop rule (100 ms)
no longer fires. Top-k, a placement cache and cross-config pre-collapse were
not needed.

Gates (Round 228): run.sh 2153 passed; san 2153, 628 files, 0 findings;
verify unchanged (gcc.dg 3908/3910, c-c++-common 637/638, cpp 289/289);
callgrind 3.8863 G (+0.04% against 3.8848 G, from the extra `include_miss`
parameters in `pp.c`); globals 32 allowed (no change to `globals.allow`);
symcov unchanged (4718 files, 1,146,276 events, unindexed 0); clangd_diff 7
files, 1988 differences, 0 unexpected; TSan (58 session runs, 236-edit stress with indexing, all 16 proj_test scenarios): 0 reports.

### 15.2 B5c: references, calls and definition (Round 229)

**What was built.** One query module, `src/lsp/projq.[ch]`, and the features
rewritten over it; no parallel code.

- **Position source (10.4).** `PqFiles` is one position source for every
  feature that places a result: per query, one `PqFile` per path with its text
  and a lazily built `LineTable` (B5b's). A file's text comes from the open
  unit's snapshot `SrcFile`, else the open document (`ws_doc_text`), else the
  disk. `workspace/symbol`'s private `WsFiles` is gone: it uses `PqFiles` too.
- **Mapping and union (10.3).** `pq_place` is B5b's query-time mapping (record
  hash equal: direct; open document whose disk text hashes to the record:
  `cindex_text_edit` plus `cindex_edit_map`; otherwise dropped, with a sweep
  hint at `pq_free`). Results are `PqLoc`s (file, offset, length, source rank),
  sorted by (path, offset, length, rank) and deduplicated, so a place the open
  unit and a record both give is written once, the open unit's.
- **Keys.** `pq_key` converts the open unit's decl with `cindex_key` and maps
  the anchor of a non-external key to disk coordinates; an anchor inside the
  damage has no project counterpart. `projidx` gained a key map (`pi_find`:
  key to (record, key index) hits, under the read lock) maintained where
  records are added and released, so a query is a hash lookup, not a scan.
- **References (11.2).** The open unit's events plus the project's REFS (and
  DECL and DEF with `includeDeclaration`) under the key. `documentHighlight`,
  `typeDefinition` and the macro branch use the same collector.
- **Call hierarchy (11.3).** `c_calls` unions the open unit's calls with the
  project's CALL entries under the key, grouped by the other end's key. The
  other end's item is the open unit's if it has the decl, else the project's
  definition or declaration (`pq_sym`). Item data gained `key` (hex); an item
  the open unit cannot resolve is answered from the key alone, and a resolved
  item is accepted only if it has the item's key (two statics of one name are
  two keys).
- **Definition and declaration (11.4).** For an external function or object
  the open unit lacks the role of, the project's DEF (DECL) entries replace the
  unit's fallback to the other role.

**Deleted** (B5b copies and their helpers): `WsFile`, `WsFiles`, `ws_file`,
`ws_place`, `ws_files_free`, `json_pos_at`, `c_events`, `uses`, `c_call_item`.

**Deviations from the design.**
1. No persistent 64-entry `TextCache` with stat checks. Files are read at most
   once per query (the snapshot's text when the unit has the file) and the
   operating system caches the rest. Records are validated by content hash, as
   in 10.3, which makes the stat check redundant. Cost: a references query
   over files that are not in the snapshot re-reads them each time; measured
   p95 is 3 ms on the 1,770-TU project, so nothing is gained by a cache.
2. The key map in `projidx` replaces the "postings" of the text (one
   mechanism for 11.2, 11.3 and 11.4, and the name list stays as it was).
3. The open unit's own results are now sorted by (path, offset), like the
   project's, instead of by index event order. `tests/lsp/csym.expected` was
   regenerated: only the order of reference results changed (api.h before
   main.c).
4. Call items are ordered by key (name, kind, linkage, path, offset), not by
   (path, offset): the groups are keyed by the other end, whose place may be
   in a file the call sites are not in. The call sites inside a group are in
   (path, offset) order.
5. Open-unit results for a name defined in another unit now name the
   project's definition for `definition` where the unit used to answer with
   its declaration (the 11.4 replacement), so `definition` of `h01_f2` from a
   caller in another unit jumps to the defining .c, not the header (`xref`).

**Stop rules (neither fired).**

| Rule | Measured |
|---|---|
| references p95 on a 1,000-TU project over 300 ms | 3.1 ms on the 1,770-TU project of `wsym_bench.py` (libuv, Lua 5.1-5.5, LuaJIT, zstd, cereal; three flag sets each; 24 functions of libuv's core.c, 1 to 438 results each). p50 0.3 ms. `definition` p95 0.3 ms; `incomingCalls` p95 4.6 ms (up to 354 callers). workspace/symbol p95 17.6 ms (limit 100). |
| clangd differential: unexplained misses above 2% of clangd's results | 0 of 3,425 (lua54: 2,652 results; libuv's Linux units: 773), see below. |

**The differential (14.6).** `clangd_diff.py` has a project mode: the units of
lua54 and libuv's Linux build get a `compile_commands.json`; cereal is asked
after `cereal/indexWait`; clangd after its background index has written a
shard for every unit and its progress has ended. clangd's index is not
deterministic (a run can lack whole units' references and keeps lacking them),
so its answers are the union of three runs on fresh copies. Asked of each of
four open files of lua54 and three of libuv: references (with declarations)
of every top-level name clangd lists, and definition and declaration at up to 80 distinct calls.
Intended differences (P1 is B4's):

- P2: clangd lists the invocation of a macro whose body uses the name
  (`lua_call(...)` for `lua_callk`); cereal lists no place for it (a `#define`
  body token is no use, LSP.md, and the records follow the open unit).
  459 results, 13% of clangd's. This is the one large difference; it is
  intended (Q7, resolved 2026-10-15: keep, parked in 18). Rename refuses
  instead of missing these sites (R7, 11.5).
- P3: where a definition or declaration request answers when both answer
  (clangd: the header declaration, or the declaration at a definition; cereal:
  the definition). 120 requests.
- P4: cereal lists a use spelled in a macro argument that clangd omits. 0 in
  the final runs.
- P5: the request is in an inactive preprocessor region (`cc -E` line markers
  give the active lines): cereal analyses the unit as compiled and has nothing
  there. 29 requests, not counted in the total.
A miss is a clangd result in none of these classes, or a clangd definition or
declaration answer where cereal has none. The single-file part is unchanged in
kind (7 files, 0 unexpected); its difference count moves between runs with
clangd's own nondeterminism (2078 and 2230 on two runs in one session; the
old binary and this one give identical outgoingCalls for the sampled item).

**Gates (Round 229).** run.sh 2157 passed (new `xref` scenario in
`tests/proj_test.py`, run by 22c); san 2157, 628 files, 0 findings; verify
unchanged (gcc.dg 3908/3910, c-c++-common 637/638, cpp 289/289); callgrind
3.8886 G (+0.06% against 3.8863 G); globals 32 allowed (no change to
`globals.allow`); symcov unchanged (4718 files, 1,146,276 events, unindexed 0);
TSan: 58 session runs (plain and `-fparallel`) with indexing on,
`lsp_stress.py --symbols --calls --hints --refs` 200 edits (239 sent) on a
40-TU project with indexing active, all 17 `proj_test` scenarios: 0 reports.
`wsym_bench.py` (now also timing references, definition and incomingCalls):
workspace/symbol p95 17.6 ms, index 9.8 s for 1,770 units.

### 15.3 B5d: rename across TUs (Round 230)

**What was built.** About 1,180 lines added to `src` (1,030 net after
deleting 147), 115 of tests and 132 of `bench/tools/rename_libuv.py`.

- **Per-file edit lists in crename.** `CRename` carries `CRenFile {path, offs,
  n}` per file; `map_off`/`unmap_off` take one list; a unit is checked twice
  over an overlay of every edited file it reads. `files` is given (project)
  or planned from the unit (`plan_unit`, the old single-file path), so
  `cereal query rename=` and the LSP use one `c_rename`. The renamed decl is
  the one at the asked place; any other decl with an event at an edit (a
  macro argument that is also a declaration) is refused as before ("also
  names").
- **One refusal mechanism for macros (R4 and R7).** `cindex_rename_blocker
  (flags)` returns the reason for `MACRO_BODY` or `AT_EXPANSION`; the unit's
  events (`cindex_rename_plan`) and every project record event (`pq_entries`
  with `PqBlock`) use it. Q7 keeps macro invocation sites unlisted, so this
  is what stops a silent miss. R1 is `pq_ambiguous` (the same place under
  another key in a record variant of the same text), R3 the `PqBlock` count
  of entries that could not be placed.
- **Scope and gate.** R2 and R5 are `project_scope` on the scheduler thread
  (every unit fresh, indexed, not running, not `FAILED`/`OVER_LIMIT`; an
  affected unit not `PARTIAL`; at most `renameMaxTUs`). R6 and the per-unit
  blockers are `c_rename` per affected unit on `P.pool` via `project_gate`,
  with `project_pause` holding the scheduler's submissions. Without a pool
  it runs serially.
- **Request path.** A project rename is a worker-thread request
  (`need_worker`, `Server.workers`, joined at shutdown), so the builder
  stays free (since Round 232 every C rename, a local too). The reply is
  `documentChanges` (version for an open document, `null` otherwise) or
  `changes`. `pq_current` rechecks every edited file's version or hash
  against the text the edits were made on; a mismatch is ContentModified.
- **Tests.** `proj_test.py` scenario `rename`: edits equal the references;
  versions in `documentChanges`; the `changes` form; R6 (collision with an
  existing name); R4/R7 for prepareRename and rename; R2 (an unindexed unit,
  jobs held); R5 (`renameMaxTUs` 1); ContentModified (a file changes after
  the checks: `cereal/holdChecks` with `gate`; before Round 232 a fault hook
  appended to the file). `lsp_stress.py --rename`.

**Stop rule.** `bench/tools/rename_libuv.py` renames each of 441 external
functions of the libuv Linux units (38 units that compile; `posix-poll.c`
does not on Linux and is left out, as `paths` would) to a fresh name:

| Outcome | Count |
|---|---|
| edits returned | 382 |
| refused, a use in an inactive `#if` group (A3.9) | 54 |
| refused, a use in a `#define` body (R4/R7) | 5 |
| other refusals | 0 |

False refusals: 0 of 441 (0%) with A3.9 and the macro refusals counted as
correct refusals, 12.2% if the A3.9 ones are counted as false. They are
correct (user decision, Round 231: keep refusing renames that touch inactive
`#if` groups, so the design reading, 0%, is the stop-rule measure): each is a
use in another platform's branch (`uv_barrier_*`, the
`uv_sem_*` of the macOS select path, `uv__iou_fs_*`), which a rename would
leave stale. The first run, with `posix-poll.c` in the project, refused
413 of 441 under R2 (an erroneous unit is `PARTIAL`, and every external name
is declared in `uv.h`, which it reads): by design, but it shows that one
non-compiling entry blocks all renames until `paths` excludes it (kept, user
decision; since Round 231 the R2 message names the blocking units with their
error counts and points at `cereal.index.paths`; the manifest stores the count,
not the first error's text, so that is not shown). Duration
about 4 min (0.57 s a rename on 4 cores; 0.54 s after Round 232's R1 rewrite).

**Deviations and limits.**

- prepareRename waits 300 ms for the scheduler's scope check and skips R2/R5
  if it does not answer.
- Macro renames still reply with `changes`.
- A macro that names the function but is never invoked leaves no record
  event, so R4/R7 cannot see it (a test had to invoke it).
- A decl whose events at the edits include a second decl is refused (before,
  the plan listed only the asked decl's events).

**Gates.** run.sh 2158 passed; san 2158, 628 files, 0 findings; callgrind
3.8845 G; globals 32 allowed; symcov 4,718 files, 1,146,276 events;
wsym_bench workspace/symbol p95 21.9 ms (limit 100), references p95 3.3 ms;
clangd_diff project mode 3,400 clangd results, 0 unexplained misses (the
single-file part reported 6 and then 15 `cereal only` lines in lua54 that
clangd's nondeterministic index omits; they are real references, and
clangd varies between runs). TSan: 58 session runs, `lsp_stress.py
--symbols --calls --refs --rename` 200 edits on a 40-TU project with
indexing active (two files), all 18 `proj_test` scenarios: 0 reports.

### 15.4 Review of B5c and B5d (Round 232)

A Sonnet review of `git diff 9327eb6..ab72ecc` gave nine findings and six
unifications; each was checked in the code before it was fixed. Eight of the
nine findings were real; F7 was not reachable and is discarded (below). The
fixes follow the end-state rule: one rename path first (U1), and what it
subsumed is deleted.

| # | Finding | Status |
|---|---|---|
| F1 | Call hierarchy `fromRanges` doubled: `pqcall_cmp` ended with `site.seq`, and the loop used it as the duplicate test | Real. The loop dedupes on (key, place) with `pqcall_same`; the sort is `pq_key_cmp` then `pq_loc_order`. `proj_test` `xref` asserts unique ranges and an exact site count, with the unit open and saved. |
| F2 | A rename of an external to the name of another external of a unit that does not read the edited files was not seen | Real. R8 (`pq_has` on both kinds, whole project) and R9 (no definition in the project). Tests: a clash and a no-definition refusal, and a fresh name that goes through. |
| F3 | A unit-only rename ignored a unit that `#include`s the main file (unity build) | Real. U1: a name with a project key is always planned from the records (`rename_plan`), so every unit that reads an edited file is checked. Test: `unity.c` including `uni_a.c`; three edits in two files. |
| F4 | `prepareRename` ran `pq_ambiguous` (R1) on the protocol thread, O(entries x hits) under the read lock | Real. Per record: a bsearch of (path, text hash) in the sorted own places first, the own-key loop only for records that pass it, the file-less scan under the lock only; the one place found is mapped after the lock. `rename_libuv.py`: 0.54 s a rename (was 0.57 s). No path-to-records index in projidx was needed. |
| F5 | An opaque `struct`/`union`/`enum` tag got a different key per unit | Real. The anchor of a tag prefers its first declaration (5.3); `CREC_FORMAT` 4. Test: a forward declaration in a header, the definition in one unit, a use in another: one entity in the references. Not closed: a unit that sees only the definition anchors there. |
| F6 | The outgoing call item pointed at the caller's local prototype | Real. `sym_define` asks the project (`pq_sym`) when the open unit's symbol is a declaration and not a definition. `xref` checks the callee item is at the definition. |
| F7 | R7 not applied to DECL/DEF entries from records (`CRecDecl` has no `AT_EXPANSION`) | Discarded: not reachable. A declaration formed by a macro never gets a record entry (`cindex_symbol_event` rejects it), so there is nothing to flag; the rename of such a name is refused by the gate's re-check of that unit (the unit's declarations would change). The gap is that such a declaration is absent from the project index (18). A test (`zm.c`) pins the refusal. |
| F8 | Unbounded work: no caps, a linear `pq_file`, file reads and `S.m` under `pi_rdlock` | Real. Caps: `PQ_MAX_ENTRIES` 20000 entries a query, a cut answer is logged (`window/logMessage`), a rename of that size is refused (R10). `pq_file` is a hash table. Entries, declarations and calls are copied out under the lock and placed after it. Parked: a stat-validated hash cache, releasing texts early (18). |
| F9 | Smalls | See below. |

**F9.**

- The R2 snapshot versus `project_pause` gap: the pause now starts before the
  plan (`project_rename`) and ends after the edits are written.
- The `rename-modify` fault hook, which appended to a user file inside
  production code, is deleted. The ContentModified test uses the test-only
  `cereal/holdChecks` with `gate` (the rename waits after its checks, the test
  edits a file, then releases).
- `gate_job` leaked the `Run` structs of `c_rename` on a fatal: `run_check`
  now traps the fatal itself, so `run_free` frees them and the message
  reaches the refusal.
- `pq_where` counted bytes and scanned from the start: it uses the client's
  line table and encoding now (`pos_of`, shared with `pq_pos`).
- The ignored `pq_key` return in `c_sym` and `item_is`: not a bug (the key is
  filled either way; false only means the project has nothing under an anchor
  inside an edit). Made explicit with a comment and `(void)`.
- `c_rename` in project mode had no asked place, so the renamed decl was
  the first event at an edit: `project_rename` passes the cursor
  (`CRename.path`, `off`).
- The shared compile database case (several Lua versions in one database)
  is documented in LSP.md.

**Unifications.**

- **U1, one rename path.** `project_rename` serves every C rename; a local or
  a parameter is planned from the unit's events and checked as the open unit
  alone. Deleted: the `wide` parameter of `cindex_rename_plan` and its "needs
  the project index" refusal for the project case (`project` flag instead),
  `lsp_check_rename`, `RenameJob`, `rename_run`, the rename use of
  `need_builder`, and the unit-only write path of `c_rename_edits`.
  `plan_unit` stays for `cereal query rename=`.
- **U2, staleness.** `pq_current` answers every "the text changed" question for
  a rename (the request's document included, now that the plan uses the
  snapshot's text); `doc_unchanged` is no longer asked for a rename, only for
  code actions. `lsp_doc_state` hashes without the BOM, as the checks' text.
  R3's `ndropped` stays: it is the plan-time check on the records, a
  different question (can this entry be placed) from the end-time one.
- **U3, places.** `loc_cmp`, `pqcall_cmp` and the call dedupe are one pair,
  `pq_loc_cmp` (place) and `pq_loc_order` (place, then source). `mine_cmp`
  and `pq_key_cmp` stay: they order records' coordinates and keys, not places.
- **U4, `DocSet` and `WsDocText`: kept.** `DocSet` is a consistent copy of all
  open documents for the pool threads of the gate, which borrow buffers;
  `WsDocText` copies one document on demand for a query. Merging them would copy every
  open document for each references request.
- **U5, scheduler events.** `EV_SYNC`, `EV_CREATED` and `EV_SCOPE` are one
  `EV_CALL` (a function and argument run on the scheduler thread); `Event.req`
  is gone. `EV_SAVE`/`OPEN`/`CLOSE`/`SWEEP` stay: they carry a path and have no
  caller-specific function.
- **U6, messages to the client.** `rpc_begin`/`rpc_end`/`rpc_log`/
  `rpc_progress` (rpc.c) replace four copies of the JSON-RPC envelope
  (`begin_notification`, `begin_msg`/`end_msg`/`send_log`/`send_progress`,
  `phase_failed`'s and `rename_progress`'s hand-written ones). `pq_where` and
  `pq_pos` share one position function; `cindex_place` (the checker's
  messages, from a `SrcMgr`) stays: it has no `PqFiles`.

**Deviations from the review.**

- F5: the tag anchor is "the first declaration (DECL before DEF)", not "first
  event in (path, off) order whatever its role": the second would make the key
  depend on the sort order of the two paths.
- F4: no path-to-records index in `projidx`; the bsearch over the sorted own
  places is enough, and the lock is held for memory scans only.
- F8: the cap is a count, 20000, not a byte or time bound.

**Gates.** `tests/run.sh` 2158 passed; `gate.sh` globals 32, san 2158 passed with 0 findings, callgrind 3.8830 G (baseline 3.8840 G), verify unchanged (gcc.dg 3908/3910, c-c++-common 637/638, cpp 289/289); `symcov` 4718 files, 1,146,276 events, 0 unindexed, 0 param misses; `clangd_diff` project mode 3424 results, 0 unexplained; `wsym_bench` workspace/symbol p95 21.2 ms; `rename_libuv.py` 441 renames in 3m59s, 0 false refusals (0.0%, design reading); TSan (Round 192 recipe): no reports in the sessions, the stress run (`--symbols --calls --refs --rename`, indexing on, 6 project renames answered with edits; the stress now roots at the compile database and waits for the index) or `proj_test.py`. Caveats: `csym_rename.json` differs under TSan only because the recipe sets `CEREAL_INDEX_ENABLE=1` (R9 message instead of the index-off one); one `proj_test` rename failure in the first TSan pass did not reproduce in 11 later runs (alone, full, 4 in parallel).
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

10. **One damaged or deleted system chunk re-indexes a whole project.** The
    libc chunk is shared by nearly every TU (about 10 s per 1,000 TUs on the
    generated project). Rare and recoverable, as for a widely shared header
    record. Chunks are per project (under the project directory), so the
    whole-project eviction of 6.4 stays correct; sharing across projects would
    need reference counting and is parked.

## 17. Open questions (with recommendations)

**Resolved 2026-10-13: the user accepted every recommendation below (A1,
Q1-Q6).** The options stay as the record of what was weighed.

**Assumption A1 (accepted).** D2's "one per file keyed by (path, content hash, flag
hash)" is read as "one manifest per TU keyed by (main path, flag hash,
build id), with content hashes per listed file, and content-addressed
per-file records". This is the reading that also satisfies "shared
headers stored once".

**Q1. Macro references across TUs (12.2). Resolved: recommendation.**

1. Definitions only. Recommended.
2. Definitions and references with a macro key. Pros: full navigation.
   Cons: bytes, and a new D8 key kind.
3. Nothing. Pros: smallest. Cons: regresses workspace/symbol for macros.

**Q2. Cache location default (6.1). Resolved: recommendation.**

1. XDG cache directory, per user. Recommended. Pros: survives `git
   clean`, invisible to the repo. Cons: per-machine only.
2. `<root>/.cache/cereal`, clangd's choice. Pros: discoverable, easy to
   delete. Cons: shows in `git status` unless ignored.

**Q3. Build id (E14). Resolved: recommendation.**

1. A hash of the executable. Recommended. Pros: never serves stale
   checker output. Cons: a full re-index after every cereal rebuild.
2. `INDEX_FORMAT` alone. Pros: survives rebuilds. Cons: stale answers
   after checker fixes until the files change.

**Q4. Rename TU cap (R5). Resolved: recommendation.** Default 100, with `documentChanges` and
progress. Recommended: keep 100, and measure the gate's time per TU in
B5d before raising it.

**Q5. Settings transport. Resolved: recommendation.** `initializationOptions` plus environment
variables (recommended, today's pattern). The other option is
`workspace/didChangeConfiguration`, which needs a new handler; parked
unless asked.

**Q6. Rename requires a complete index (R2). Resolved: recommendation.**

1. Refuse until every TU is indexed and none failed, unless excluded by
   `paths`. Recommended. Pros: never misses an edit in an unindexed TU.
   Cons: one failing TU blocks rename of external names project-wide.
2. Allow, and warn which TUs were not covered. Pros: usable during
   indexing and with failures. Cons: can leave a broken build behind,
   against D6's intent.

**Q7. Function references through macro expansions (found by the 14.6
differential, Round 229). Resolved 2026-10-15: keep as is.** `lua_call(L, 0, 0)`
expands to `lua_callk(...)`. For `lua_callk` clangd lists each invocation (the
spelling `lua_call`); cereal lists none of them and not the body token either
(in the open unit as in records). Of clangd's 3,425 results on lua54 and libuv,
459 (13%) are such sites. Decision: cereal does not list the invocation sites
of macros whose body uses a function. This is an intended difference from
clangd (P2 in `clangd_diff.py`). Listing them is parked (18): it needs the
invocation's spelling per event in the record (a format bump) and a rename
policy for macro sites. Rename must therefore refuse, not miss, when a record
shows the name used in a macro body (R7, 11.5).

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
| Listing the invocation sites of macros whose body uses a function (Q7) | Needs a record format bump (the invocation's spelling per event) and a rename policy for macro sites. Rename refuses instead (R7). ROADMAP Track D, "macro-aware project index and refactoring". |
| A macro that names a function but is never invoked (R4/R7 gap) | No record event exists for it; needs the macro-body identifier spellings in the record (the same format bump as Q7). Documented limitation. |
| Renaming through inactive `#if` groups (A3.9) | Rename refuses a use in a group the unit does not compile (user decision, Round 231). Editing it needs per-branch preprocessing or a textual mode verified under the other branches. |
| A macro-aware rename policy | What to edit at a macro site (argument, body, or neither) is undecided; rename refuses rather than guesses. |
| Declarations formed by a macro in the project index | `cindex_symbol_event` rejects a pasted or synthesised declaration (`PROTO(zm)` making `int zm_fn(void);`), so no record has it: references and call hierarchy do not list it and R7 cannot see it. A rename is still safe, because the gate's re-check of that unit changes its declarations and refuses (`tests/proj_test.py` rename, `zm.c`). Needs the declaration kept in the index with its expansion-site spelling. (Round 232 F7: the R7 bit on declarations was not reachable; discarded.) |
| A stat-validated hash cache for `PqFiles` | A query reads and hashes each file it places entries in; `PQ_MAX_ENTRIES` bounds the files, the page cache makes a repeat cheap. A cache would need the record's own stat. Not needed at the measured 3.3 ms references p95. (Round 232 F8) |
| Releasing a file's text after its last edit is written | The peak is the sum of the files a result lies in, bounded by the entry cap. (Round 232 F8) |

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
