# C design: fix-its, `cereal fix`, LSP quick fixes

Written 2026-10-10 against `cea7869` (v0.14.0). ROADMAP track C. Section 8
covers slices C1 to C4. C5 (a baseline file for CI) and C6 (macro to inline,
enum or const suggestions) are out of scope here and get their own designs
(section 10).

Claims about the code cite `file:line` at that commit. Claims about gcc-13
were probed with gcc 13.4.0 on WSL (section 13). Numbers marked "est." are
estimates. **Status: C1 is implemented (HISTORY Round 212, section 8 "C1
as built"); C2 is implemented (Round 213, "C2 as built" below; its
header-guard and cond-typo items in Round 214); C3 is implemented (Round
214, "C3 as built" below); C4 is not.** Where the text below still describes
C1, C2 or C3 as future work, "C1 as built", "C2 as built", "C3 as built" and
the probe table (section 13, P11 to P19) take precedence.

## 1. Decisions in one place

D1 to D6 are the user's and are not reopened here. The brief said "D1..D7"
but listed six; D7 records the brief's scope rule. D8 onward are this
design's.

| # | Decision | Why |
|---|----------|-----|
| D1 | A `Diagnostic` carries a vector of `FixIt {begin, end, text, applicability SAFE \| MAYBE \| INFO}`. The old `const char *fixit` is deleted. Its three producers, the cell cache and the parallel-PP merge are converted. | (user) |
| D2 | `cereal fix` applies SAFE fixes only by default. `--unsafe` adds MAYBE. INFO is never applied. | (user) |
| D3 | CLI text output uses gcc's `-fdiagnostics-parseable-fixits` lines: `fix-it:"file":{l:c-l:c}:"text"`, byte columns, C-escaped, with gcc-13 as the oracle. JSON gets a gcc-shaped `fixits` array (start, next, string) plus an `applicability` key. `-fdiagnostics-generate-patch` and `cereal fix --diff` share one edit builder. gcc-13 has no fix-it for parentheses around an assignment or for an unused variable, so cereal-only fixes need the re-check gate. | (user) |
| D4 | Order: the C1 to C4 plumbing and the high-value fixes come first; the macro analyses (C6) come later. | (user) |
| D5 | A header edit made from several TUs needs every TU that reports the same (file, range, text) to agree. Otherwise it is skipped and reported. | (user) |
| D6 | The missing-`;` fix runs only behind `--unsafe`, and a pass applies at most one error-fixing edit. | (user) |
| D7 | C5 (CI baseline) and C6 (macro to inline/enum/const) are out of scope; section 10 only places them. | (brief) |
| D8 | Every fix-it enters through one validating function, `diag_fixit` (section 3.2). It rejects locations outside a user file and inside a macro expansion, so a fix-it is always a real source range. Token-level helpers sit on top of it. | One place enforces the "real location" guarantee. gcc does the same in `rich_location`: it refuses fix-its on macro-expanded tokens, including macro arguments (probe P5). |
| D9 | One edit builder, `fixset.c`, does sort, dedupe, overlap and same-point order. It serves `-fdiagnostics-generate-patch`, `cereal fix`, `cereal fix --diff` and the LSP fix-all. | D3 requires generate-patch and `--diff` to share it. With one builder, the LSP cannot drift from the CLI. |
| D10 | `cereal fix` writes only what a re-check accepted. Every TU that read an edited file is re-run with all edits applied through the `SrcMgr.overlay` hook (the `crename.c` method). SAFE must pass the semantic gate (section 6.4). A failure is demoted or dropped, never written. | gcc offers no oracle for cereal-only fixes (D3). The rename verifier already proves the overlay re-check works. |
| D11 | The LSP offers each fix-it as a `quickfix` code action tied to its diagnostic, plus `source.fixAll.cereal` (SAFE, this document only). Fix-all runs the D10 gate on the builder thread. | Same semantics as the CLI (D9, D10). The builder thread is where rename already runs its checks. |
| D12 | Exit status of `cereal fix`: 0 fixed point, 1 something selected was not applied (or `--diff` is non-empty), 2 usage, 3 some TU not checked, 4 a file could not be read back or written. The worst one wins, in the order 2, 4, 3, 1, 0. | Extends the replay statuses of A4 D9 (0/1/2/3) without changing them. A write failure must be told apart from "work left". Open question Q2. |

## 2. What exists (verified at cea7869)

- **The `Diagnostic` struct** (`diag.h`) has a `const char *fixit`
  ("optional suggested replacement text", `diag.h:54`). Nothing prints it.
  The LSP does not read it.
  - Its three producers:
    - `cond-typo`: `cond.c:914`, `d->fixit = best->str`, range on the token.
    - `macro-unparenthesized-param`: `hygiene.c:302`, `"(%s)"`, range on
      the body token.
    - `header-guard`: `include.c:277`, `dg->fixit = g->str`. The location
      is the `#define` keyword (`sk->dirs.data[1].kw_loc`), not the macro
      name.
  - `macro-unparenthesized-body` (`hygiene.c:447-456`) has a note "wrap the
    replacement list in parentheses" but no fix-it.
  - The cell cache copies it: `CellDiag.fixit` (`cell.h:94`), encoded at
    `cell.c:665` and decoded at `cell.c:798`.
  - The parallel-PP merge copies it: `par.c:403-404`.
- **The cell cache is in memory only.** `cell.c` has no `fwrite` and no
  on-disk format. Cells are reused within one process, keyed by
  `CellCache.config`.
  - So the brief's "bump format" needs no version bump. The change is only
    to the in-memory `CellDiag`. This corrects the brief.
- **Printing.**
  - `diag_print` (`diag.c:1002`) prints the include chain, the header line,
    then `print_loc_line` (`diag.c:905`: display columns, tabs expanded to
    8, presumed line), then the notes.
  - `diag_expansion_notes` (`diag.c:1138`) appends "in expansion of macro"
    notes as ordinary notes.
  - JSON: `diag_print_json` (`diag.c:1064`) with `json_loc` (`diag.c:1047`).
- **Header notes.**
  - `diag_header_note_loc` (`cexpr.c:887-904`) puts the first note for each
    known header at `include_loc` (`cexpr.c:852-882`): the line after the
    main file's last `#include`, else line 1. Later notes for the same
    header go at the use.
  - The `hdr_noted` bitmask is in `diag.h:84`.
  - Callers:
    - `header_note` (`cexpr.c:911`), used at 1811, 1876, 1908;
    - `builtin_mismatch` (`cexpr.c:1759`);
    - `implicit_decl`'s "include '%s' or provide a declaration" (`cexpr.c:1796`);
    - `parse.c:1186`.
- **Missing-token errors.** `perr_after_prev` (`parse.c:509-523`) places a
  missing-token error at the end of the previous token, but only when that
  token is plain source (`!exp || exp == t.loc`). Its end is computed with
  `tok_raw_len` (`lex.h:87`).
- **Macro provenance.**
  - `PTok.mloc != 0` marks a token from a macro expansion, including macro
    arguments (`ptok_in_macro`).
  - The checker asks `tfrom_macro` (`check_int.h:620`).
  - Preprocessor tokens carry `TF_ORIGIN_BODY`, `TF_ORIGIN_ARG`,
    `TF_PASTED` and `TF_SYNTH` (`token.h`).
- **SrcMgr.**
  - The `overlay` hook (`srcmgr.c:193-203`) serves a buffer instead of the
    disk.
  - A UTF-8 BOM is stripped on the `read` and mmap paths alike (Round 211;
    before it, a file of 64 KiB or more that started with a BOM gave
    `unknown type name '\U0000feffint'`, P10).
  - The overlay path keeps it.
- **Rename verifier.** `crename.c` checks a unit as it is and again with
  the edits served by an overlay (`run_check`, `crename.c:55`), then
  compares:
  - diagnostics, by level, id and mapped place (`map_off`/`unmap_off`,
    `crename.c:89,105`);
  - declarations and events;
  - inactive groups, through a `skipped` listener (`crename.c:38`).

  `differs` (`crename.c:235`) is the comparison. Section 6.4 reuses this
  method.
- **TU jobs** (`main.c`).
  - `TuFn(Options*, path, out, err)` (`main.c:120`).
  - `TuJob` (`main.c:340`).
  - `run_jobs` (`main.c:408`) buffers each job's output in `open_memstream`
    and commits jobs in input order, so output is identical for any `-j`.
  - `mode_compdb` (`main.c:554`) dedupes entries by (absolute file, options
    fingerprint) and returns `n_err ? 1 : n_not ? 3 : 0` (`main.c:699`).
  - `finish` (`main.c:86`) is the shared end of each TU.
  - Lint analyses are attached as PP listeners in `lint_one` (`main.c:159`).
    `check`/`parse` do not attach them.
  - `-Wfatal-errors` exists (`driver.c:373`). The error limit is
    `max_errors` (`diag.c:364,375`).
- **Command-line flags.**
  - `-fdiagnostics-parseable-fixits` and `-fdiagnostics-generate-patch`
    are rejected today as "unknown option" (exit 2, probed).
  - `cereal fix` prints the usage text (exit 2).
- **LSP.**
  - There is no `codeActionProvider` in `initialize` (`server.c:908-1022`).
    Text sync is incremental.
  - Requests carry a `CPolicy` (`server.c:1037-1045`). `must_wait`
    (`server.c:1091`) and `handle_request` wait up to 1.5 s for the newest
    edit's snapshot.
  - Rename checks run on a worker thread (`project_rename`, `lsp_rename_gate`;
    B5_DESIGN 15.4). Round 217 to 231 ran a unit-only rename on the builder
    (`RenameJob`, `lsp_check_rename`); Round 232 deleted that path.
  - The analyses run on the snapshot's macro-phase TU (`server.c:197`).
  - Published diagnostics (`lsp_publish_diagnostics`,
    `features.c:1867`) are the macro phase's (`s->tu.diag.all`) plus the
    check's. Check diagnostics are deduped by `diag_key` (range and message,
    `features.c:1769`) and kept as `CDiag` offsets (`lsp.h:68-78`).
  - `cdiags_carry` (`features.c:1837`) moves `CDiag` offsets across an edit
    and drops a diagnostic whose range or any note meets the edit.
  - Rename emits a `WorkspaceEdit` with `changes` keyed by URI
    (`c_rename_edits`, `features.c:1013`).
- **`deps.c` writes `.d` files with a plain `fopen("w")`** (`deps.c:288-296`).
  The tree has no atomic-write helper.

## 3. Fix-its in the diagnostic engine (C1)

### 3.1 Data

```c
typedef enum { FIX_SAFE, FIX_MAYBE, FIX_INFO } FixApplic;

typedef struct FixIt {
    SrcLoc begin, end;      /* [begin, end) in one user file; begin == end: insertion */
    const char *text;       /* arena; may be "" (deletion) */
    uint16_t owner;         /* 0: the diagnostic itself; k: notes.data[k-1] */
    uint8_t app;            /* FixApplic */
} FixIt;
```

`Diagnostic` gains `fixits` and `nofix`; `DiagNote` gains `nofix` and
`expn`:

- `VEC(FixIt) fixits`, ordered by addition;
- `bool nofix` on `Diagnostic` and on `DiagNote`: that owner is poisoned.
  gcc drops all of an owner's fix-its once one of them is refused. Each
  note is its own rich location there, so a refused note does not affect
  the diagnostic's fix-its. cereal copies this per-owner rule, so the
  differential matches;
- `bool expn` on `DiagNote`, set by `diag_expansion_notes`, so the printer
  can place fix-it lines after an owner's expansion notes (P4).

A diagnostic's fix-its, across the diagnostic and its notes, form one
atomic group in the builder (5.2). gcc treats a note's fix-its as part of
the same edit, for example the `(` and `)` of the logical-not note (P3).

### 3.2 The one entry point (D8)

```c
bool diag_fixit(DiagEngine *d, Diagnostic *dg, int owner,
                SrcLoc b, SrcLoc e, const char *text, FixApplic app);
```

It returns false, poisons the owner (`nofix`) and removes that owner's
fix-its already added, unless all of these hold:

1. `dg` is not NULL and not suppressed.
2. `b` and `e` are in the same `SrcFile` of kind `SF_USER`, not
   `system_header`, with `b <= e <= base + size`. This excludes
   `SF_VIRTUAL` (`-D`, built-ins), `SF_SCRATCH` (pasted and `_Pragma`
   text), `SF_SYSTEM` and missing files.
3. `text` has a newline only as its last character, and only when the fix
   is an insertion at column 1. This is libcpp's `rich_location` rule. No
   gcc-13 front-end diagnostic can produce any other newline (P11), so the
   rule is a guard, not a probed behaviour. The include insertion is the
   only producer that needs a newline.
4. Neither end is strictly inside a token whose spelling differs from its
   bytes. A line splice inside the replaced token is allowed, because the
   bytes are replaced whole; an end that cuts through a splice is refused.

The SrcLoc-level function knows nothing about macros. The guarantee comes
from the helpers, which are the only way producers add fix-its:

- **Parser and checker (`PTok`):**
  - `fix_replace_toks(p, i, j, text)` covers tokens `i..j`, from
    `toks[i].t.loc` to the end of `toks[j]` computed with `tok_raw_len`.
  - `fix_insert_before(i, text)` and `fix_insert_after(i, text)` insert at
    a token's start or end.
  - Each refuses (poisons) when any token touched or anchored has
    `mloc != 0`, or `exp != 0 && exp != t.loc`. This is the same test
    `perr_after_prev` makes (`parse.c:512-514`), and it covers macro
    arguments, as gcc does (P5).
- **Lint (`Tok`):** `fix_tok_replace(a, in, t, text)`. It refuses tokens
  with `TF_ORIGIN_BODY`, `TF_ORIGIN_ARG`, `TF_PASTED` or `TF_SYNTH`. Tokens
  of a `#define` body as written are directive tokens, not expansion
  output, so they are allowed: the hygiene fixes edit the definition.
- **Header notes:** `diag_header_note(d, dg, loc, hdr, fmt, ...)` replaces
  `diag_header_note_loc`.
  - It adds the note at the place gcc uses (first per header at
    `include_loc`, later at the use).
  - When the note is the first for its header, it also adds the fix-it
    `#include <h>\n` at `include_loc`, attached to that note.
  - Callers are converted in C1: `header_note`, `builtin_mismatch`,
    `implicit_decl`'s include note and `parse.c:1186`. The old function and
    `header_note_loc` are deleted.
  - Edge (changed by P12): gcc gives the fix-it to the first note per
    header, for any header, not only the 20 in `known[]`. C1 therefore keeps
    a generic per-engine list of headers already noted (`hdr_seen`) and
    ignores `known[]` for this. `include_loc` is also the line after the
    last `#include` before the use *in the file that holds the use* (P18:
    a header with no includes gets `1:1` in the header), not only the main
    file.

A fix-it on a token that came from a macro expansion is never written into
the macro's definition. The only fixes that edit a definition are the lint
hygiene and guard fixes, and they name definition tokens directly.

### 3.3 Printing (D3)

**Text.** Fix-it lines are printed only under `-fdiagnostics-parseable-fixits`,
as in gcc. Without the flag the 1918 goldens are unchanged.

- Each owner's lines are printed immediately after that owner: after the
  diagnostic and its caret block, or after a note. They follow the owner's
  expansion notes (`expn`, P4).
- Line shape: `fix-it:"F":{L1:C1-L2:C2}:"T"`.
  - `F` is the presumed file name, `L` the presumed line (P8: under `#line`
    gcc prints the `#line` file and line).
  - `C` is the physical 1-based byte column, with `C2` exclusive, from
    `srcmgr_linecol`. Columns are never display columns (P1: tab case).
  - `F` and `T` are escaped by one function, `fixit_escape`: `\\`, `\"`,
    `\n`, and other bytes below 0x20 or of 0x7f and above as three-digit
    octal (P2: `\303\251`). A tab is written `\t`, as gcc's
    `print_escaped_string` does; no gcc-13 diagnostic emits a tab, so P11
    cannot observe it.
- A BOM is not counted (P9: columns match gcc). This is automatic once the
  mmap path strips the BOM too (C1 fixes the 64 KiB bug, section 2).

**JSON.** Diagnostics and notes get `"fixits": [...]`, omitted when empty.
Each entry is
`{"start": LOC, "next": LOC, "string": T, "applicability": "safe"|"maybe"|"info"}`.
`LOC` is cereal's existing `json_loc` shape (`file`, `line`, `col`,
physical byte column), not gcc's `display-column`/`byte-column` triple.

- Same shape as gcc: the key names (`fixits`, `start`, `next`, `string`)
  and their placement on both diagnostics and children (P7).
- Different from gcc: the inner location object, so cereal's JSON stays
  internally consistent. Open question Q6.

**Caret block.** No fix-it line is drawn under the caret block (gcc's
`+++` / suggested-text line). It is parked: cereal's caret block is not
gcc-identical today, so there is nothing to match.

### 3.4 `-fdiagnostics-generate-patch` (D3, D9)

- Accepted in every TU mode. At the end of each TU (`finish`), all of its
  fix-its are fed to the builder (5): every applicability, notes included
  (P6: gcc applies note fix-its and every `;` insertion).
- The builder's unified diff goes to stderr after the TU's diagnostics,
  with gcc's shape (P6): `--- NAME` / `+++ NAME`, 3 context lines,
  `@@ -a,b +c,d @@`, and `SrcFile.name` as written.
- **Changed in C1 (P13).** The patch is not dropped on overlap: the C1
  producers never make two fix-its cover the same bytes (P13: duplicates and
  same-point insertions are all applied, in order, and the patch is
  identical to gcc's). The builder still skips an overlapping later group
  whole, as in section 5. What invalidates the patch (no patch printed) is:
  - a refused fix-it anywhere in the TU (`nofix`: a fix-it that gcc's
    `rich_location` would refuse, such as one in a macro expansion), or
  - a fix-it whose presumed file or line differs from the physical one
    (`#line`), matching P8.

### 3.5 Cells and the parallel PP

- `CellDiag` replaces `const char *fixit` with `CellFix *fixes;
  uint32_t nfixes`, where `CellFix = {uint16_t owner; uint8_t app;
  CLoc at; uint32_t len; const char *text}`.
  - The end is stored as begin plus length: a fix-it never crosses a file,
    and a relative `CLoc` keeps it reusable when the cell moves.
  - `cenc_diag` and `cdec_diag` (`cell.c:646-675`, `782-811`) encode and
    decode it.
  - There is no on-disk format, so there is nothing to version (section 2).
- `par.c` `diag_copy` deep-copies `fixits` and `nofix`.
- The run.sh parallel-identity sections (5 and 7) gain one run with
  `-fdiagnostics-parseable-fixits`, so fix-its are compared too.

### 3.6 What C1 converts

The three existing producers move to the helpers. `header-guard` changes
its fix range: from the `#define` keyword (which the old code implied) to
the macro *name* token of the `#define`, the first token of
`sk->dirs.data[1].toks`. Their applicabilities come from section 4.

## 4. Fix-its offered (C1, C2)

There are 1438 fix-it lines in 201 files when gcc-13 is run with
`-Wall -Wextra -std=gnu17` over gcc.dg and c-c++-common (survey P14).

### 4.1 Classes gcc emits (oracle: gcc-13; differential in section 8, C1)

| Class (survey lines / files) | Producer in cereal | Applic. | Slice |
|---|---|---|---|
| include insertion on header notes (80 / 61) | `diag_header_note` | SAFE | C1 |
| `.` to `->` on a pointer (in did-you-mean family) | `ccall.c:2259` (not the pointer-to-pointer message at 2255) | SAFE | C1 |
| did-you-mean, any kind (97 / 41): undeclared, implicit function, unknown type name, member, `#elsif` to `elif` | `cexpr.c:1805,1871,1903`, `parse.c:1186-1190`, member lookup, `pp.c:2640` | MAYBE | C1 |
| expected `;` (336 / 30) | `parse.c:3323` via `perr_after_prev` | MAYBE (D6) | C1 |
| expected `)` `]` `:` `(` `,` (265 / 50) | `parse.c:662` `"expected %s%s"` | INFO | C1 |
| missing braces (452 / 23) | `cinit.c:1639` | SAFE | C2b (done, Round 213) |
| logical-not parentheses note (126 / 10) | `cwarn_expr.c:1324` | MAYBE | C2b (done, Round 213) |
| xor used as pow (26 / 2) | `cexpr.c:6243` | MAYBE | C2b (done, Round 213) |
| format specifiers (41 / 10) | format checker | MAYBE | C2b (done, Round 213) |
| "use 'struct' keyword" (~9) | parse unknown-type path | MAYBE | C2b (done, Round 213) |

Notes on the table:

- **`.` to `->`.** For a pointer to pointer gcc gives no fix-it for the
  `.` form (P15), but for `->` on a pointer to pointer it does give two
  MAYBE insertions, `(*` before the operand and `)` after it (P19: gcc.dg
  pr91134, spellcheck-fields-3). C1 produces both.
- **Same-line rule.** A fix-it whose two ends are on different lines is
  refused, and so is any text with a newline except a column-1 insertion
  ending in `\n`. A hint that begins where the previous hint of the same
  owner ends is merged into it (touching merge), unless the previous text
  ends in a newline. This replaces the earlier "splice" wording.
- **did-you-mean is MAYBE, not SAFE.** It is an edit-distance guess. The
  closest name may be wrong even when the code then compiles.
- **missing braces is SAFE.** Braces that follow the existing aggregate
  structure do not change the initialised values. This is true by
  construction (6.4), and `fixgate.sh` checks it.
- **Expected-token insertions other than `;` are INFO.** They are printed
  and offered in the LSP, never applied by `fix`. After a syntax error the
  insertion point is a guess about intent (Q8).
- **Logical-not is MAYBE.** The parentheses change the meaning to the
  likely intent, which is not the current meaning.

### 4.2 Cereal-only fixes (no gcc oracle; gated)

| Fix | Edit | Applic. | Reason |
|---|---|---|---|
| `macro-unparenthesized-param` | param token to `(param)` in the `#define` body | SAFE | Gate: every expansion's AST modulo parens is unchanged. If any use would parse differently, it is demoted to MAYBE: the fix then changes meaning, though for the better. |
| `macro-unparenthesized-body` | insert `(` and `)` around the replacement list | SAFE | Same gate. Offered only for a body that is one expression: not for statements, an empty body, or a body that uses `#` or `##`. |
| `header-guard` | `#define` name to the `#ifndef` name | SAFE | Gate: inactive groups and diagnostics unchanged in every TU reading the header. Demoted to MAYBE when the misspelt name is tested anywhere else in the TU (another `#if` reads it). |
| `cond-typo` | identifier in `#if` to the close name | INFO | The "never defined" name may come from `-D` in another configuration. One TU cannot know that. Q1. |
| parentheses around assignment in a condition (`cstmt.c:956`) | insert `(` `)` around the assignment | MAYBE | It keeps the meaning, but it silences a warning that is often a real `=`/`==` bug. Applying it unasked would hide bugs. `==` is not offered. |
| `-Wstrict-prototypes` `()` to `(void)` (`cdecl.c:348,3421-3423`) | insert `void` | SAFE on a definition; MAYBE on a non-defining declaration | A definition's `()` already takes no arguments. A declaration's `()` may have callers, in other TUs, that pass arguments; then `(void)` makes those calls errors. |

**Parked**, with reasons in 12.3:

- macro trailing semicolon;
- multi-statement macro / dangling else (the do-while wrap);
- missing header guard;
- unused variable (gcc has none; deleting a declaration is not local).

### 4.3 What each slice changes in the diagnostic text

- **No message changes.** Fix-its are an attachment.
- **No new notes.**
- **One location change:** the `header-guard` fix range (3.6). The
  diagnostic's own location stays at the keyword.

## 5. The edit builder (C1; D9)

### 5.1 Input

One `FixSet`, filled with each diagnostic's fix-its as a group. Each group
carries:

- the selected applicabilities;
- a sequence key: `(job index, diagnostic index in TU order, fix-it
  index)`.

Positions are `(FileKey, offset)`.

- `FileKey` is the realpath plus `st_dev`/`st_ino`. Different spellings of
  one file and symlinks become one file.
- `offset` is into the text cereal read: BOM stripped, line splices and
  CRLF left as they are.

### 5.2 Rules

1. **Sort** by `(file, begin, end, kind, seq)`. At equal `begin`, an
   insertion sorts before a replacement, so text inserted at a point lands
   before the replacement that starts there.
2. **Exact duplicates collapse.** Same file, range and text, from any TU or
   group. The survivor keeps the lowest `seq`, and every group that
   contained a copy refers to it. The same include insertion from two TUs
   of one header becomes one edit.
3. **Several different insertions at one point** are kept, in `seq` order.
   This matches P3: `size_t` and `uint8_t` inserted at 1:1 come out in
   diagnostic order.
4. **Touching is not overlap.** An insertion at a replacement's begin or
   end is not a conflict. Neither are two adjacent replacements
   (`[a,b)` and `[b,c)`).
5. **Overlap** (non-empty intersection, or an insertion strictly inside a
   replaced range) is a conflict.
   - The group with the lower `seq` wins, and the whole later group is
     skipped. A group is atomic: the `(` of a pair is never applied without
     its `)`.
   - A skipped group is reported once, and is offered again on the next
     pass (6.3), when its diagnostic is recomputed against the new text.
6. **Output:** per file, the edits in order. The result is the edited text
   (for the overlay and the writer), a unified diff, or LSP `TextEdit`s
   (C4).

The builder knows nothing about TUs or applicability. Section 6 decides
which groups go in.

### 5.3 Check

The tree has no C unit-test harness (the Makefile builds only `cereal`),
and real diagnostics rarely produce overlapping fix-its. So the builder
gets a testing input, in the spirit of `index --replay`:
`cereal fix --edits=FILE --diff`. FILE lists groups, one edit per line
(`PATH BEGIN END "TEXT"`, with a blank line between groups), and they
replace the collected ones.

This combination of options lands in C1, as the first piece of the `fix`
mode. The rest of `fix` came in C3 (Round 214); `--edits=FILE` stays as
the builder's testing input.

Cases in `tests/fix/` cover sort, dedupe, same-point order, touching
versus overlap, and group atomicity. This is the one small runnable check
for this logic. Real-diagnostic coverage comes from the generate-patch
goldens (8, C1).

## 6. `cereal fix` (C3)

### 6.1 Interface

```
cereal fix [options] FILE...
cereal fix --compile-commands DB [options] [FILE-OR-DIR...]
  --unsafe          also apply MAYBE fixes (D2)
  --diff            print the unified diff of what would change; write nothing
  --max-passes=N    default 8
  --only=ID[,ID]    apply only fix-its of these diagnostic ids
  --root=DIR        write only files under DIR (default: the current directory)
  -j N, every check option (-I, -D, -std=, -W...)
```

- Which fixes exist follows the warning options. A diagnostic disabled by
  `-Wno-x` has no fix-its.
- `fix` runs the check-mode diagnostics and the lint analyses in one TU:
  the analysis listener is attached to `tu.pp` before `frontend_run`, as
  crename adds its listeners.
  - The PP runs sequentially (`-fparallel` is ignored). Parallelism comes
    from `-j` over TUs.
  - `-Wfatal-errors` is forced off.

### 6.2 One pass

1. **Collect** (parallel, `-j`).
   - Each TU job runs the check plus analyses with its own options. With a
     compile database this is the entry's options, deduped as in
     `mode_compdb`, in database order.
   - `TuJob` gains `void *res`. The new TU function fills a `FixResult`:
     - every fix-it group, with its applicability, owner's level and id,
       as `(FileKey, offset)`. Selection happens in the merge;
     - the TU's error count and its error-diagnostic ids;
     - the list of files it read, with a hash of each one's text.
   - Nothing is printed per TU, except option problems, as `mode_compdb`
     does.
2. **Merge** (main thread, in job order, so the result is the same for any
   `-j`).
   - Group selection:
     - SAFE groups, or SAFE and MAYBE under `--unsafe`;
     - in a TU with errors, only groups belonging to error diagnostics;
       warning fixes wait for an error-free pass (Q3). So in a TU whose
       errors have no fix, no warning fix is applied, and the exit status
       is 1;
     - at most one error group per pass (D6), whatever its
       applicability. It is the lowest `seq` among the selected error
       groups.
   - Why D6 covers every error group, not only `;`: the error fix-its
     after the first error come from the parser's recovery state, which a
     first fix changes. With SAFE fixes the cost is small. The SAFE error
     fix is the include insertion for an unknown type name, and only the
     first note per header carries it, so one header is one group. This
     reading of D6 is open question Q10.
   - **Same text everywhere.** All TUs read the file from the same disk,
     so their hashes of it agree. If they do not, the file changed during
     collection, and all of its edits are skipped ("changed during the
     run").
   - **D5 agreement.** For an edit in a file that several TUs read, see
     whether every TU that read the file reported the same
     `(file, range, text)` edit.
     - If all of them did, the edit stands.
     - If any did not (for example a header fix that only one
       configuration reports), the group is skipped and reported "not
       reported by N of M translation units that include FILE".
     - A TU whose options disable that diagnostic does not count as
       disagreeing. It is left out of M, so `-Wno-x` in one entry does not
       block a fix the others want. `FixResult` keeps a reference to the
       TU's options for this question; they live until the merge ends.
   - Feed the builder. Conflicts are handled as in 5.2.
3. **Gate** (6.4), parallel over the affected TUs.
4. **Write** (6.5) the edits that passed.
5. **Next pass**, while something was written and `pass < max-passes`.
   Each pass re-reads from disk, so the offsets of pass k+1 refer to the
   text of pass k.

**Idempotence.** At the fixed point a second run applies nothing, prints
nothing and exits 0. Tests run every `fix` case twice and require this.

### 6.3 Output

- **stderr, sorted by (path, line, column)** so it is deterministic for any
  `-j`:
  - one line per skipped group, with its reason: conflict with an earlier
    fix, D5 disagreement, failed gate, error-limit pass, unwritable or
    changed file;
  - one summary line: `cereal fix: N edits in M files in P passes; K
    skipped`. Under `--compile-commands`, the `mode_compdb` entry summary
    comes first.
- **`--diff`** prints, to stdout, the builder's diff of the first pass's
  gated edits, then stops. Files are sorted by path, with paths relative to
  the current directory, so it works with `patch -p0`.
  - It runs one pass only. Later passes depend on written text; simulating
    them through overlays is possible but parked.
  - With `--diff`, exit 1 means "would change".
- Fix-it lines in gcc format are not printed by `fix`. For that, use
  `cereal check -fdiagnostics-parseable-fixits`.

### 6.4 The gate (D10)

- **Which TUs.** Every TU whose read set contains an edited file is re-run.
  - A TU of the same database that did not read the file needs no re-run.
  - A TU *outside* the run (another configuration, a file not in the
    database) cannot be checked. This is a stated limitation (12.1).
- **How.** The re-run uses `SrcMgr.overlay` to serve each edited file's new
  text (BOM-stripped, LF/CRLF as in the file). It collects:
  - the multiset of diagnostics `(level, id, message, place)`, where the
    place maps through the edit as `crename.c`'s `map_off` does;
  - the inactive `#if` ranges (the `skipped` listener, mapped);
  - for SAFE paren groups, a new `ast_digest`: a hash of the unit's tree in
    pre-order, of each node's tag, child count and token spelling. It
    skips `N_PAREN` nodes (their child takes their place) and ignores
    locations. With child counts in the hash, `a-(b-c)` and `a-b-c` differ,
    because their trees have different shapes.
- **SAFE passes** when all of these hold:
  1. every diagnostic that owned an applied fix-it is gone;
  2. no other diagnostic was added, removed or changed;
  3. the inactive ranges are unchanged;
  4. for paren groups, `ast_digest` is unchanged.
- **Missing braces** are SAFE by construction: the braces follow the
  aggregate structure the initialiser walker already computed. They get
  tests 1 to 3, and `fixgate.sh`'s object-code identity (8, C3) checks
  this claim. No initialiser comparison is built.
- **The overlay serves a file to each TU under that TU's own spelling.**
  The overlay hook is called with the normalized path. Each `FixResult`
  records the `(FileKey, path)` of every file it read, so a re-run serves
  the edited text for exactly those paths. The place mapping generalizes
  `crename.c`'s main-file-only `map_off` to every edited file.
- **MAYBE passes** when (1) holds and the TU's error count drops by at
  least the number of fixed errors. MAYBE may add warnings: a
  did-you-mean can surface an unused-variable warning.
- **Failure.** A failing SAFE group that passes the MAYBE test is demoted
  to MAYBE. It is applied only under `--unsafe`, otherwise skipped with
  "fails the semantic check".
  - If the pass fails, the failing groups are found by deterministic
    bisection over the pass's groups, in `seq` order. Then the TU is
    re-gated with the rest. The cost is about log2(groups) extra checks per
    failing group.
  - A group must pass in every TU that re-ran. The minimum applicability
    across those TUs is used.
- **Paren groups are digested separately.** The `ast_digest` comparison
  needs a run in which only paren groups changed the file. So paren groups
  are gated in their own re-run when a pass mixes them with other groups:
  at most two re-runs per TU per pass, plus bisection.

### 6.5 Writing (and files on disk)

For each edited file, in path order:

1. **Re-read and compare.** Read the file again, strip a BOM as `SrcMgr`
   does, and compare the result with the hash the collect phase took. If
   it differs (an editor saved it meanwhile), skip all its edits: "changed
   during the run".
2. **Map to bytes on disk.**
   - With a BOM on disk, offsets shift by 3 and the BOM is kept.
   - Fix text `\n` becomes the file's line ending, taken from its first
     line ending (CRLF if that is `\r\n`, else LF). Text in a CRLF file
     stays CRLF. gcc's patch keeps `^M` (P9).
   - Encodings: cereal reads bytes, and the fix-it texts are ASCII, plus
     identifiers copied from the same file. So no transcoding happens. A
     UTF-16 file is not C that cereal reads, and never gets here.
3. **Skip what cannot be safely written.** Skip with a report when:
   - `access(path, W_OK)` fails;
   - the file is not regular;
   - it has `st_nlink > 1`: a hard link would be split by rename;
   - its real path is outside the root: `--root=DIR`, default the current
     directory. A header reached through `-I` from a vendored or installed
     tree is otherwise a user file (`SF_USER`) and would be edited. Its
     groups are skipped with "outside the root", and they do not raise the
     exit status to 1.
   - Symlinks are resolved, and the target is written.
4. **Write atomically.**
   - `mkstemp` in the same directory, write the whole text, `fsync`,
     `fchmod` to the original mode (and `fchown` if possible, ignoring
     failure), then `rename` over the original.
   - On any failure: unlink the temporary file, report, set exit status 4,
     and continue with other files.
   - A new helper `file_write_atomic` in `common.c` does this. `deps.c` is
     not changed: replacing a `.d` file needs no atomicity.

Every group this design offers edits one file. The include insertion's
owner may be in a header, but its edit is in the main file. A future group
that spans files stays atomic: skipping one of its files skips the whole
group.

### 6.6 Exit status (D12)

| Code | When |
|---|---|
| 0 | Fixed point: no selected group remains to be applied, every TU was checked, every write succeeded |
| 1 | A selected group was skipped (conflict left at max passes, D5, failed gate, D6 limit reached at max passes), or `--diff` printed something |
| 2 | Usage error |
| 3 | Some TU or database entry was not checked |
| 4 | A file could not be re-read or written |

The worst code wins, in the order 2 > 4 > 3 > 1 > 0. Diagnostics left in
the code do not affect the status: `fix` is not a checker (Q2).

### 6.7 Parallelism and determinism

- **Collect and gate run in parallel. Merge and write do not.**
  - Collect and gate are TU jobs through `run_jobs`, with results committed
    in job order.
  - Merge and write run on the main thread, in `seq` and path order.
- Checker per-check state is `__thread`, and each job runs one check at a
  time on its thread. So the gate's re-runs are separate jobs, never nested
  inside a collect job.
- Tests:
  - `-j1` and `-j12` give byte-identical stdout, stderr and files;
  - run.sh section 7 adds a `fix --diff` case.

## 7. LSP code actions (C4; D11)

### 7.1 Capabilities

`initialize` advertises
`"codeActionProvider": {"codeActionKinds": ["quickfix", "source.fixAll.cereal"], "resolveProvider": false}`,
but only when the client sends
`textDocument.codeAction.codeActionLiteralSupport`. Without it, clients
expect `Command`s, and cereal has no commands, so the provider is left out.

The client's `workspace.workspaceEdit.documentChanges` decides the edit
shape (7.4).

### 7.2 Where the fix-its live

There are two sources:

- **The macro phase**, `s->tu.diag.all`. Fix-its come directly from each
  `Diagnostic` (sm0 locations). They are rebuilt with every snapshot, so
  they never need carrying.
- **The check**, `s->cdiags`.
  - `CDiag` gains `CFix *fixes; size_t nfixes`, where
    `CFix = {char *path; uint32_t off_b, off_e; char *text; uint8_t app}`.
  - `cdiag_make` fills them, keeping a fix only if its file passes
    `file_same` (its offsets are valid in the snapshot), exactly as
    `carry` is computed for notes.
  - `cdiag_dup` and `cdiag_free` handle the new field.

**Carrying** (`cdiags_carry`): each `CFix` goes through `carry_range`
like the diagnostic's range.

- If any fix of the diagnostic meets the edit, all of the diagnostic's
  fixes are dropped. The group is atomic.
- The diagnostic itself is kept, if its range and notes survive.
- So a carried diagnostic never offers a fix that was computed for text
  that has since changed.

### 7.3 `textDocument/codeAction`

- **Policy.** A new policy `CP_SNAP`: wait (the same 1.5 s deadline) only
  for `snap->gen == u->want`, not for the check.
  - If the deadline passes with the snapshot still older than the
    document, answer `[]`, because positions would not match.
  - Carried fixes are valid, so waiting for the check buys nothing.
  - Lightbulb requests are frequent and must not stall.
- **Selection.** Pick diagnostics (both sources) whose range intersects
  the request range and that have fixes.
  - With `context.only`, honour it: only `quickfix`, or only
    `source.fixAll`.
  - The diagnostic object put in the action is the client's own from
    `context.diagnostics` when one has the same `diag_key` (range and
    message). Otherwise cereal renders its own, as `put_diag` does.
- **One action per group.**
  - `kind: "quickfix"`.
  - `diagnostics: [d]`.
  - `isPreferred: true` iff SAFE.
  - `title` is generated from the edit:
    - `Insert 'X'` for a single insertion;
    - `Replace with 'X'` for a single replacement;
    - `Remove 'X'` for a deletion;
    - `Add #include <h>` for the include note's group;
    - `Apply fix-it` otherwise.
  - INFO and MAYBE fixes are offered too. Here the user, not cereal,
    decides.
- **`source.fixAll.cereal`** is offered when the request names that kind,
  or when the request has no `only` and the document has at least one
  SAFE group.
  - Its edit is computed on the builder thread. (As planned: `RenameJob`
    generalized into `BuilderJob {fn, ctx, result}`. As built: the first
    fix-all request is deferred to the builder and answered from there,
    "C4 follow-ups"; the `BuilderJob` was deleted in Round 217 when rename
    went the same way.)
  - The job runs the D10 gate on the unit with the snapshot's overlay plus
    the edits, restricted to this document's file. The edits come from the
    builder (5.2) over the SAFE groups of the document, selected as one CLI
    pass selects them (6.2: errors first, D6), without D5: the editor has
    one unit.
  - Edits that fail the gate are left out.
  - If the gate run is fatal (`FatalTrap`) or the document changed
    meanwhile (the version check below), it answers `[]`.

### 7.4 Edits and versions

- With `documentChanges` support, the edit is
  `{"documentChanges": [{"textDocument": {"uri", "version"}, "edits": [...]}]}`.
  - The version is the snapshot overlay's version for that path
    (`lsp_overlay_version`).
  - A client applying it to a newer text rejects it. That is the safe
    outcome.
- Without `documentChanges` support, the edit is `{"changes": {uri:
  [...]}}`, as rename emits.
- An include fix-it in the main file that comes from a header's
  diagnostic is a cross-file edit. It is shown only on the file the
  diagnostic is published for, and its edit names the main file's URI.
- Ranges use `json_range` with the negotiated position encoding. Insertion
  text is sent as is: LSP clients keep the document's line endings, and
  cereal's `\n` is what the editor inserts.

### 7.5 Incremental edits

No new carrying code is needed. Text changes already produce a new
snapshot whose macro-phase diagnostics (and their fix-its) are fresh, and
`cdiags_carry` now carries or drops the check's fixes (7.2).

## 8. Staging, tests, gates, estimates

### C1: plumbing, output, gcc-covered fix-its (est. 6 days, realistic 8 to 12)

**Contents**

- 3.1 to 3.5.
- (The BOM fix on the mmap path landed separately in Round 211.)
- The edit builder (5), with its testing input `fix --edits=FILE --diff`
  (5.3).
- `-fdiagnostics-generate-patch`.
- Converting the three existing producers and the header notes.
- The gcc-covered rows of 4.1 marked C1.
- `diag_fixit` and the helpers.

Est. 900 to 1300 lines.

**Tests**

- `tests/check/fixit-*.c` goldens with
  `// flags: -fdiagnostics-parseable-fixits`, each verified against gcc-13
  first: tab columns, octal escapes, `#line`, BOM above and below 64 KiB,
  CRLF, macro argument refusal, `perr_after_prev` across lines, include
  insertion with and without prior `#include`s, two headers at one point.
- JSON goldens (`-fdiagnostics-format=json`) with fixits on notes.
- Generate-patch goldens (patch on stderr).
- Builder cases through `fix --edits` (5.3).
- run.sh sections 5 and 7 compare with fix-it lines on.

**Differential:** `bench/tools/fixits.sh`, modeled on `deps.sh`.

- With no argument, it runs the `tests/check/fixit-*.c` goldens' sources
  against gcc-13, which keeps the goldens honest. With `DIR`, it runs
  every C file under DIR.
- It runs gcc-13 and cereal with `-fsyntax-only
  -fdiagnostics-parseable-fixits $FIXIT_FLAGS`, keeps the `fix-it:` lines
  with the header line that precedes each, and compares.
- Output: `N cases, M differ`.
- Also: `par.py` gains `CEREAL_FIXITS=1`, which adds the flag on both
  sides and compares fix-it lines (owner header plus line). `verify.sh`
  then prints "gcc fix-its identical in F of G files".
  - This needs the cache key to include the flag. It already does, since
    flags are part of `base`.
  - Gate number for C1: every survey file whose fix-its come from C1 rows
    matches. The C1 classes are about 780 lines in at most 180 files:
    include 80, did-you-mean 97, `;` 336, other expected tokens 265.
    Others are reported, not failed.

**C1 as built (HISTORY Round 212, 2026-10-10)**

Done: everything under Contents and Tests above. Size: src +1284/-128 lines
(new: `fixset.c/h` 462, `fixmode.c` 199, `fixtok.c/h` 28, `c/fixptok.c/h` 60),
tests +369, bench +135.

- **Model.** `Diagnostic.fixits` is a `VEC(FixIt)` with `owner` (0 the
  diagnostic, k the note k-1) and a per-owner poison flag `nofix`; the old
  `fixit` string is gone. Cells carry `CellFix` plus `CN_NOCOL|CN_NOFIX|
  CN_EXPN` note flags; `par.c diag_copy` deep-copies.
- **Where the guard lives.** `diag_fixit(d, dg, owner, b, e, text, app)`
  holds all of 3.2. Passing `b = 0` forces a poison, which the token helpers
  (`fixtok.c` for lint `Tok`, `c/fixptok.c` for parser `PTok`, wrapped for
  the checker by `cfix_tok`/`cfix_ins` in `check_int.h` and `fix_missing` in
  `parse.c`) use for macro-origin tokens.
- **Producers.** Header notes (`cheader_note` in `check.c`: builtin
  mismatch, implicit declaration, "defined in header", unknown type name);
  did-you-mean for identifiers, implicit functions, members (`ccall.c`,
  `cinit.c`), unknown type names and `#elsif`; `.` to `->` and the
  pointer-to-pointer `(*`/`)` pair; expected `) ] ; , :` (after the
  previous token) and `(` (before the token found); lint: cond-typo (INFO),
  hygiene parameter parentheses (SAFE), header-guard (SAFE).
- **Output.** `-fdiagnostics-parseable-fixits`, the JSON `fixits` array
  (diagnostics and notes), `-fdiagnostics-generate-patch` (printed to the
  diagnostic stream after the TU's diagnostics; JSON mode prints it after
  the array). `fix --edits=FILE --diff` (`fixmode.c`): `PATH BEGIN END
  "TEXT"` per line, a blank line between groups; the diff goes to stdout and
  `group N skipped: overlaps group M` to stderr. Any other `fix` use is a
  usage error, exit 2. The builder's diff handles single-line edits only
  (everything the producers make); a multi-line edit prints an error and
  exit 1.
- **Tests.** `tests/check/fixit-{typo,tabs,include,include-bare,line,macro,
  json,patch}.c`, `tests/fix/*.edits` (sort, dedupe, same-point order,
  touching, overlap, group atomicity; run.sh section 19), run.sh section 20
  (BOM x 64 KiB x CRLF against gcc-13), fix-its on in the run.sh section 5
  and 7 parallel/`-j` comparisons.
- **Differential.** `bench/tools/fixits.sh` statuses: `same`; `diag` (a
  covered diagnostic is missing on one side: a difference of diagnostics,
  other parity work, not failed); `differ` (fails). Over all 7042 C files
  of gcc.dg and c-c++-common (3.5 minutes at 8 jobs): 0 differ, 31 diag.
  With `FIXIT_PATCH=1` (patches compared where every gcc fix-it is of a
  covered class): 0 differ over the 198-file survey. `par.py
  CEREAL_FIXITS=1` through `verify.sh`: identical in 102 of 103 files; the
  one is spellcheck-inttypes (`PRIx8` is not in `std_names`).
- **Design changes forced by the probes** (also marked in the text above):
  1. 3.4: the patch is not invalidated by an overlap, only by a refused
     fix-it or a `#line` presumed-file/line difference (P13, P8).
  2. 3.2: the splice rule is replaced by a same-line rule; touching hints
     of one owner merge unless the earlier text ends in a newline.
  3. 3.2: the once-per-header rule is generic (`hdr_seen`), not tied to
     `known[]` (P12); `include_loc` is per file of the use (P18).
  4. 4.1: `->` on a pointer to pointer does get fix-its (P19); a missing
     `(` is inserted before the next token, not after the previous one.
  5. `fixset_diff` supports single-line edits only.
  6. `#line N` without a file name: cereal marks the patch invalid, gcc
     prints a garbage name; not matched (not worth a rule).
- **Known gaps (not fixed, reported).** Of the 31 diag files (Round 213
  closed two: the `case` colon message and `PRI*`/`SCN*` macros): the
  `expected ')'`/`']'` fix-its inside attribute and asm operand parsing
  (attr-assume-2, c2x-attr-*, declspec-18, asmgoto-2); implicit declaration
  did-you-mean for functions (20041213-1, redecl-*, pr59963-1,
  Wimplicit-function-declaration-c89*); `__FILE_`, `__LINE_` and
  `__builtin_` did-you-mean; the `include <h> or provide a declaration`
  note in C89 mode (c90-impl-decl-1, Werror-implicit-function-declaration).
  (The "'f' is declared in header" notes were converted in Round 213: four
  goldens changed, see C2 as built.)

### C2 as built (Round 213)

Implemented: the gcc-covered C2b producers and the cereal-only C2 fixes of
the brief, through `diag_fixit` and the C1 helpers; nothing in C3 or C4.

- **Header notes** (`ccall.c`, builtin mismatch) use `cheader_note`: first
  note per header at `include_loc` with the `#include` fix-it. Goldens
  changed (checked against gcc-13 note positions): `attr_fmt_noproto`,
  `builtin_decl_mismatch`, `misc_122`, `misc_123`.
- **Missing braces** (`cinit.c`, SAFE). Replicates gcc's model: `{` is
  recorded when an implicit level is pushed (before its first element),
  `}` when it is popped (before the following comma, or before the
  enclosing `}` at the end of a list, so a trailing comma puts it at the
  `}`); designator-pushed levels add a `}` only; hints keep insertion
  order and touching ones merge; every explicit pop with the flag set warns
  with the cumulative hints; range designators warn without fix-its; in a
  compound literal the warning is at the `(`. Two root-cause fixes came
  with it: `zeroinit` is now updated after conversion (`{0.5}` for an int
  is zero) and float truncation in `post_zero`.
- **Logical-not note**, **xor as pow**, **"use 'struct' keyword"**, **format
  specifiers** (MAYBE). The format fix-it replaces the whole spec keeping
  flags, width and precision; the written conversion character is tried
  first with lengths none, hh, h, l, ll, L, z, t, j, then the first
  character of each table row; signedness is ignored and an identical
  replacement is still emitted (gcc does). The logical-not note is not
  emitted when the operand folds to a constant (gcc does not either).
- **Cereal-only**: body parens in hygiene (`nowrap` rule), assignment
  parens (MAYBE), strict-prototypes `()` to `(void)` (SAFE on a
  definition, MAYBE on a non-defining declaration).
- `diag_fixit` refuses fix-its with a column above 4096 (gcc's
  LINE_MAP_MAX_COLUMN_NUMBER).
- **Tests**: nine `tests/check/fixit-*.c` goldens (assign, braces, case,
  format, lognot, pri, proto, struct, xor), `tests/lint/hygiene_fixit.c`;
  `verify.py` learns `// expect-fixit: L:C-L:C "text" APP` (when any are
  present the set must match exactly). `fixits.sh`: `FIXIT_COVER` extended
  to the new classes, `FIXIT_SKIP` removed.
- **Differential**: 7042 files, before 0 differ and 31 diag; after 2 differ
  and 32 diag (the covered set is wider).
- **Remaining / deviations**:
  - 2 differ: the zero-size-array missing-braces files
    (Wmissing-braces-fixits.c, init-excess-1.c), where gcc pushes and pops
    repeatedly.
  - Format fix-its with `%1$d` positional arguments are not produced.
  - Header-guard and cond-typo INFO items of 4.2 were not in this round.
  - Diag files added by the wider coverage, all pre-existing gaps:
    gnu2x-attrs-2 (cereal reports an error on a call through a
    format-attribute typedef), pr65066 (no scanf signedness warning with a
    null format target), store_merging_28 (cereal rejects the dg-option
    `-fno-tree-vectorize`). Closed: missing-symbol-2, spellcheck-inttypes.

### C2: cereal-only fixes and C2b (est. 5 days, realistic 7 to 12)

Status: done in Round 213 except the header-guard and cond-typo items (see
"C2 as built"), which were done in Round 214 ("C3 as built").

**Contents**

- 4.2: hygiene param and body, header-guard, cond-typo INFO, parentheses
  around an assignment, strict-prototypes.
- The C2b rows of 4.1: missing braces, logical-not, xor, format, `struct`
  keyword.

Est. 500 to 900 lines (format fix-its are the largest unknown).

**Tests**

- Goldens: `tests/check` for the checker fixes, and `tests/lint` with
  `// expect:` lines for the lint fixes. `verify.py` learns a
  `// expect-fixit: L:C-L:C "text" APP` form.
- The gcc-covered C2b rows join the differential.

### C3: `cereal fix` (est. 8 days, realistic 11 to 17)

**Contents:** section 6, `file_write_atomic`, `ast_digest`, the gate, and
compdb integration. Est. 1000 to 1500 lines.

**Tests:** `tests/fix/NAME.cmd` cases run by a new `tests/fix_case.sh`. It
copies the case's directory to scratch, as `deps_case.sh` does, runs the
command twice, and prints stdout, stderr, exit status, and `diff -r`
against the original tree after each run, so idempotence is visible.

Cases:

- each SAFE fix applied;
- each MAYBE fix skipped without, and applied with, `--unsafe`;
- D6 one-error-per-pass;
- a conflict;
- a D5 disagreement through a compdb with `-DA` in one entry;
- a header fixed once from two TUs;
- a gate demotion (a hygiene fix that changes a use);
- CRLF, BOM, read-only file, symlink, hard link, file changed mid-run (a
  hook env var makes the writer pause, test only);
- `--diff`;
- max passes;
- `-j1` against `-j12` identity.

**Semantic gate check:** `bench/tools/fixgate.sh DIR` runs `cereal fix`
on a scratch copy of a corpus with gcc-13. For every file changed, it
compares `gcc-13 -fsyntax-only` diagnostics before and after (SAFE: the
fixed warnings gone, nothing new) and the object code of `gcc-13 -O0 -c`
before and after, built without `-g`. For SAFE, the objects must be
identical. Reported per id.

A fix that adds lines (the include insertion) moves every later
`__LINE__`, for example in `assert`. For those fixes the objects are
compared only when the file uses neither `__LINE__` nor `assert`. Other
files are checked by diagnostics alone, and the report says so. Every
other SAFE fix keeps line numbers: parens, braces, `void`, the guard
name.

### C3 as built (Round 214)

Implemented: `cereal fix` as scoped above (6.1 to 6.7), nothing of C4. The
C2 leftovers (header-guard, cond-typo) landed in Round 214 part 1:
`cond-typo` is INFO; `header-guard` is SAFE, or MAYBE when the misspelt name
is tested anywhere else in the unit (`tested_anywhere`, `include.c`; Q9 as
proposed).

Where the code is:

- `src/fixtu.c` (`fix_tu`, a `TuFn`): one TU's check plus lint analyses, the
  `skipped` listener, and `collect` into a `FixResult` (`fixres.h`: files by
  real path and text hash, diagnostics, edit groups with their applicability,
  inactive ranges, the enabled-option map, the AST digest). An overlay set in
  `FixResult.ov` makes it a gate re-run.
- `src/fixmode.c` (merge, gate, write): the global file table, candidate
  selection, `merge_pass`, `fix_run` (the pass loop), the old `--edits`
  path. `src/fixmode.h` holds `FixOpts`, `fix_flag`, `fix_run`.
- `src/jobs.h`: `TuFn`/`TuJob` (now with `res`), `run_jobs`, `CdbPlan`;
  `mode_compdb` was split so `fix` reuses its plan (`compdb_plan`).
- `src/c/cdigest.c`: `checker_digest_unit`, the AST digest, mixed into
  `*CheckOptions.digest` after the visit loop. `DiagNote.hdr` records which
  header a "declared in header" note suggests (see the correlation rule).
- `common.c`: `file_write_atomic`. `diag.c`: `diag_enabled_map`.

How a pass runs (6.2 in practice):

1. Collect in parallel (`run_jobs`, results in job order). Groups with
   `nofix` owners, edits outside user files, and no-op edits (gcc offers
   `%u` for `%u`) are dropped. A note's fix-it that overlaps one of its own
   diagnostic's (xor: `0x2` against `1 <<`) is dropped, it being the other
   way to the same end.
2. The global table merges files by real path. Candidates: SAFE groups
   (class 0 strict, class 1 paren), MAYBE groups with `--unsafe` and error
   groups (class 2 lenient), gate-demoted groups. Candidates are deduped,
   D5-checked (a group each TU that read its file reports), cut by `--only`,
   `--root`, read-only, hard link, and non-regular tests, and put through
   the builder. D6: one error group per pass, the lowest `seq`; with errors
   in a TU, its warning groups wait (Q3).
3. The gate (6.4) runs three phases, each with bisection of failures: 1a
   strict on the non-paren SAFE set, 1b the paren groups alone (digest),
   2 lenient groups on top of the accepted set. A TU that read an edited
   file is re-run with the overlay; the others are not. Strict means every
   diagnostic that owned an applied fix-it is gone, none added or changed
   (places mapped through the edits), inactive ranges and, for paren groups,
   the digest unchanged. Lenient means no new errors, and the fixed errors
   gone. A strict failure demotes the group to class 2; it is skipped
   without `--unsafe` with "fails the semantic check: why".
4. Write: re-read and hash-compare each file (`CEREAL_FIX_HOOK`, a shell
   command run just before, lets a test play editor), keep BOM and line
   ends, `file_write_atomic`, report. Then the next pass, up to
   `--max-passes` (default 8); after the last a probe pass reports whether
   fixes remain ("fixes remain after N passes", exit 1).
5. Report: skip lines come from the final pass only
   (`cereal fix: FILE:L:C: skipped [ID]: REASON`, sorted); the summary
   `cereal fix: N edits in M files in P passes; K skipped` (distinct files,
   passes that wrote; "would apply " for `--diff` and `--dry-run`) prints
   when N or K is not 0. Exit 4 > 3 > 1 > 0, 2 for usage; outside-root skips
   do not raise it.

Deviations from the text above, and additions:

- `--dry-run` (summary only, no diff) exists beside `--diff`; `--apply` is
  the default and is an error together with `--diff`/`--dry-run`. The design
  names only `--diff`. `--edits=FILE` is kept for the builder goldens.
- `fix` is a mode word like `lint`, not a pre-parse dispatch; it rejects
  `-M`/`-o` and sets `fatal_missing_include` (an input that cannot be read or
  an include that cannot be found is exit 3).
- The file key is the real path only (not a `(FileKey, path)` pair); the
  overlay is served per TU under that TU's own spelling of the path
  (`FFile.path`).
- Lenient test for error groups: the fixed error must disappear and no error
  may appear; warnings may change. Several unknown-type fixes need one pass
  each (D6), which is why `p.x + q.x` takes two passes.
- Correlation rule: a diagnostic whose "declared in header" note matches the
  header note of an applied include insertion counts as fixed (inserting
  `<stdio.h>` also removes the builtin-declaration-mismatch warning).
- `-Wno-x` handling relies on the enabled map: a diagnostic disabled in one
  TU but not another is compared option by option, not by presence.
- D6 held-back error groups are reported as skipped ("held back: one error
  fix per pass") when a pass can make no more progress; at the pass limit
  they are the "fixes remain" case.
- After phase 2 no combined re-verification of the three phases is run: each
  phase was gated on top of the accepted set, so the result is the last
  gated state, but the final file text itself is only checked by the next
  pass (and the probe). Parked: a final whole-set re-run if a case shows a
  combination the last pass misses.
- `fixgate.sh` compares the `.text` section (not the whole object) for SAFE
  runs and skips the comparison when the fix inserted an `#include`: a header
  can redirect a call (`scanf` to `__isoc99_scanf`, gcc.dg/format/attr-6),
  and `assert` strings change when a stringized macro argument gains
  parentheses (tree-ssa/ldist-strlen-1). The design's `__LINE__`/`assert`
  rule is therefore not needed. For `--unsafe` runs it counts only new
  errors (more warnings may appear when broken code starts to parse, or when
  a printf call's second bad conversion is no longer hidden).

Tests: `tests/fix/NAME.sh` cases through `tests/fix_case.sh PROG NAME JOBS`
and `tests/fix/lib.sh`, each with `NAME.expected`, run by `run.sh` section
21 at `-j1` and `-j12` (which must agree): `safe`, `maybe` (each MAYBE
skipped and applied), `gate` (demotion), `passes` (D6, `--max-passes`),
`crlf`, `bom`, `files` (read-only, hard link, symlink, outside `--root`),
`unwritable` (exit 4, skipped as root), `changed` (file saved mid-run),
`diff` (`--diff`, `--dry-run`, `--apply`), `only`, `compdb` (a shared
header fixed once; a D5 disagreement from `-DA` in one entry), `corpus`
(every `tests/check/fixit-*.c` and fix-it input of `tests/lint` with
`--unsafe`, then again: a no-op). No synthetic conflict case exists; real
fix-its from one diagnostic rarely overlap, and `overlap.edits` covers the
builder. `bench/tools/fixgate.sh [DIR]` as in 8, C3: 24 cases of the
tests, 620 files of `tests/check`, a 1469-file sample of gcc.dg (every
third file): 0 failed. Under TSan the fix mode ran at `-j2/4/8/12` on 150
gcc.dg files and the compdb, maybe, safe and files cases: 0 reports.

Remaining: the final combined re-verification above; the
`CEREAL_FIX_HOOK` is a test hook, not an interface. Two nits of this slice
were closed in Round 215: an unwritable file's error is printed once per
file per run (`Totals.failed`), and `--only` with an id no diagnostic can
carry a fix-it for is a usage error (exit 2, `--only: 'ID' is not the id of
a diagnostic with a fix-it`; the ids are the static table `FIXABLE_IDS` in
`fixmode.c`, and `tests/fix/only.sh` runs each of them, so a new producer
that is missing from the table fails there).

### C4: LSP code actions (est. 4 days, realistic 5 to 9)

**Contents:** section 7. Est. 450 to 700 lines.

**Tests:** `tests/lsp/fixit-*.json` sessions with `.expected` (run plain
and with forced parallel flags, as section 9 does):

- a quick fix on a check diagnostic;
- one on a lint diagnostic;
- a carried diagnostic after an edit far away (fix kept);
- one after an edit inside the fix's range (fix dropped, diagnostic
  kept);
- a stale request answered `[]` under a held check;
- fix-all with one demoted fix;
- a client without `codeActionLiteralSupport` (no provider).

### C4 as built (Round 215)

Implemented: section 7 as scoped. Where the code is:

- `src/diag.c`: `diag_fix_group`, the validated, atomic fix group of one
  diagnostic (`diag_fixit`'s refusals, files outside the user files, no-op
  edits, a note's fix-it overlapping the diagnostic's own). `fix_tu`'s
  `collect` (so `FGroup.change` is gone), the LSP check and the macro-phase
  actions all call it: one definition of "the fix of a diagnostic".
- `src/fixmode.c`: `fix_unit_edits(o, main, file, base, ctx, out)` runs
  `fix_tu` on the unit `main` with the editor buffers as the overlay
  (`FixResult.base`, served by `fixtu.c` after the per-run `ov` files), builds
  a one-unit `Pass` (`Pass.base`: `gload` copies the buffer, no BOM strip,
  no file checks; `Pass.only_path`: groups that touch another file are not
  candidates) and runs `select_cands`, `gate_all` and `sub_build`, exactly
  the CLI's code. Gate re-runs layer their per-trial files over `base` and
  run serially with their diagnostics discarded, because stdout belongs to
  the protocol.
- `src/lsp/features.c`: `CFix`/`CDiag.fixes`, `cdiag_set_fixes` (only if
  `file_same` holds for every file the group edits), `cdiags_carry`,
  `lsp_code_action`, `put_fix_edit` (`documentChanges` or `changes`).
- `src/lsp/server.c`: capability, `CP_SNAP`, `BuilderJob` (rename now one
  user), `lsp_fix_all` (cache in `Snapshot.fixall`, freed with the
  snapshot), the `holdChecks` `builds` hook.

Deviations and choices:

- Fix-all was one pass of the merge in Round 215, not the CLI's loop of up
  to 8 passes with writes; it loops until stable since Round 216 (below).
- The fix-all run uses `fatal_missing_include=false` (the check's D2 setting)
  where `cereal fix` sets it; an include that cannot be found is a
  diagnostic, not an abort.
- The fix-all edit is computed once per snapshot and document and cached
  (lightbulb requests arrive on every cursor move). The first request after
  an edit needs the builder (Q4: up to one build or check latency); since
  Round 216 it does not hold up the protocol thread (below). The cache entry
  is stored only if the live document still equals the snapshot's buffer.
- No `only` and no SAFE group: no fix-all offered (as 7.3 says). With
  `only: source.fixAll` but no SAFE edit, nothing is offered. A fix-all is
  offered also when the cursor is on a line without diagnostics.
- The test for a stale request needs a hook the design did not name: a held
  check does not make the snapshot stale (the builder waits in the hold
  loop, an edit ends it). `cereal/holdChecks` gained `"builds": true`, which
  holds the builder before each build.
- A diagnostic of an unopened header shows on the `#include` line as in the
  publication; `diag_place` is asked with "no header is open", so for an
  open header whose diagnostic is not shown there the action is offered
  anyway (harmless; its edit is in the header).
- `cdiag_make` does not fill the fixes (7.2 says it does): the file test
  needs the `SameText` state of the publication, so `cdiag_set_fixes` runs
  right after `carry` is computed.
- The carry test edits above the fixes (they move) and at the closing
  parenthesis of the parentheses fix (the group drops, the diagnostic stays),
  not "far away" versus "inside the range" literally.

Tests: `tests/lsp/fixit_actions`, `fixit_carry`, `fixit_stale`,
`fixit_nolit` (+ `.expected`, workspace `tests/lsp/fixit/`), each also with
the parallel flags forced by `run.sh`; the `fixall` step of
`tests/lsp_session.py` applies the fix-all and compares with `cereal fix` on
a copy of the text. See LSP.md, Tests, for what each covers.

### C4 follow-ups (Round 216)

- **Fix-all loops until stable.** `fix_unit_edits` runs the one-unit pass
  (`unit_pass`, the former body) up to `FIX_MAX_PASSES` (8, also the CLI's
  `--max-passes` default) times, each over the buffers with the document's
  text replaced by the previous pass's result (`Layer`, over `base`): the
  CLI's loop, minus the writes. Other files keep the editor's text, so an
  edit of a header the fix-all does not return is not seen by the next pass.
  After one pass the edits are returned as they are. After more, each pass's
  edits are relative to the text before it, so they are replaced by one edit
  from the first changed byte to the last (common prefix and suffix of the
  buffer and the final text trimmed); applying it gives what `cereal fix`
  gives. The coarser edit is the price of not composing the passes' edits;
  `map_off` could do it if an editor shows the difference. The pass limit is
  not reported (the CLI's "fixes remain" message has no place in a code
  action); the next request, after the edit, continues.
- **The first fix-all does not block the protocol thread.** Rename, which
  design Q4 compared this to, was not asynchronous then: it waited on the
  protocol thread (`run_on_builder`; Round 217 below moved it to this
  mechanism). So this was a new mechanism: `handle_request` runs the code action as before; if
  `lsp_fix_all` has no cached edit and the request is not on the builder
  (`Req.on_builder`), it only sets `Req.need_builder`. The request is then
  handed to the builder as a `DeferredReq` (it takes over the message body and
  its arena, the request's arena and the snapshot and index references; the
  main loop does not free them: `handle_request` returns true), and the
  protocol thread reads the next message. The builder takes deferred
  requests before the next build (before Round 217: after a `BuilderJob`), runs the code
  action again with `on_builder` (this time `lsp_fix_all` computes and
  caches) and writes the response, `rpc_write` being locked. The action is
  recomputed rather than resumed (the quick fixes are cheap); if the
  document changed since the request the answer is `[]` (`stale`). Answers
  can then arrive out of order, as LSP allows. A deferred request still
  waiting when the client exits is dropped. Test hook: `cereal/holdChecks`
  now also holds a fix-all at its start (`tests/lsp/fixall_async`: a hover
  and a quick fix are answered while a fix-all is held; the fix-all arrives
  after the release, and a second one is answered at once from the cache).
  The `lsp_session.py` steps `defer`, `await` and `unanswered` express it.
- **Rename used it too (Round 217; replaced in Round 232 by the worker path,
  B5_DESIGN 15.4).** `lsp_rename`'s C path
  (`c_rename_edits`) sets `Req.need_builder` and writes nothing when it is
  not on the builder, after the existing "index not ready; retry" check (a
  stale or unchecked snapshot is still refused at once, RequestFailed
  -32803). `handle_request` defers it as the fix-all (`DeferredReq.kind`
  tells `deferred_run` which request it is). (Superseded in Round 232: renames run on a worker, B5_DESIGN 15.4.) On the builder, `lsp_rename`
  runs `lsp_check_rename` (now a plain call of `rename_run`) and the reply
  is the edit of before. New: the protocol thread no longer holds edits
  back while the checks run, so the document can change meanwhile; then the
  reply is a ContentModified (-32801) error, not edits for the old text (a
  fix-all answers `[]`; a rename error is what LSP clients expect for a
  stale document). The hold of `cereal/holdChecks` moved from `lsp_fix_all`
  to the start of `deferred_run`, so it holds both. `run_on_builder`,
  `BuilderJob` and `Server.bjob` are deleted: nothing else used them.
  prepareRename never needed the builder (`cindex_rename_plan` reads the
  index) and still answers on the protocol thread. Test:
  `tests/lsp/csym_rename_async` (a rename held while a hover and a
  prepareRename are answered; the edit after the release; a second one held
  across an edit gives -32801; then a rename on the new snapshot).

### Totals

| Slice | est. | realistic | lines est. |
|---|---|---|---|
| C1 | 6 d | 8 to 12 d | 900 to 1300 |
| C2 | 5 d | 7 to 12 d | 500 to 900 |
| C3 | 8 d | 11 to 17 d | 1000 to 1500 |
| C4 | 4 d | 5 to 9 d | 450 to 700 |
| Total | 23 d | 31 to 50 d (6 to 10 weeks) | 2850 to 4400 |

ROADMAP's 3 to 5 weeks for all of track C (with C5 and C6) is too low.
This design alone is 6 to 10 weeks. The gate and its bisection (C3) and the
format fix-its (C2b) carry most of the uncertainty.

### Docs to update when each slice lands

- **ROADMAP Track C:** C-slice status.
- **STATUS:**
  - "What exists": `cereal fix`, fix-its;
  - "How to check it": `fixits.sh`, `fixgate.sh`;
  - the numbers section: the fix-it parity count.
- **LSP.md:** code actions, capabilities.
- **HISTORY:** a round entry per slice.
- **This file:** mark slices done.
- **`cereal` usage text** (`main.c:19-84`): `fix` mode, the two `-f` flags.

## 9. Verification against gcc-13 (detail)

- **What gcc covers.** The 4.1 rows are compared line for line by
  `fixits.sh` and `par.py CEREAL_FIXITS=1` (section 8, C1), which covers the owner header, file, range and text.
  Applicability is cereal's own and has no oracle.
- **What gcc does not cover.** 4.2 rows rely on the gate (6.4) plus
  `fixgate.sh`'s object-code identity for SAFE.
- **Known gcc behaviours copied** (all probed, section 13):
  - byte columns;
  - presumed file and line under `#line`;
  - octal escapes;
  - fix-it lines after expansion notes;
  - only the first include note per header carries a fix-it;
  - no fix-it on macro-expanded tokens, including arguments;
  - none for a pointer to pointer;
  - the patch includes note fix-its.
- **Where `#line` matters:** gcc prints no patch for a TU with a `#line`d
  fix-it (P8). `-fdiagnostics-generate-patch` copies this (3.4).
  `cereal fix` and the LSP work on physical file offsets, so `#line` does
  not affect them.

## 10. Out of scope: C5 and C6 (D7)

- **C5, a baseline file for CI:** record the current diagnostics so CI
  fails only on new ones.
  - Needs a stable diagnostic identity across edits: id, file, enclosing
    function or macro, message without line numbers. It also needs a file
    format and an `--update-baseline` workflow.
  - It touches `mode_compdb` and the summary, not fix-its.
  - Its own design. It depends only on A4's replay.
- **C6, macro to inline function, enum or const suggestions:** analyses
  over macro definitions and their uses (the type of each use, side
  effects, `#`/`##`, uses in `#if`, uses as lvalues).
  - Their edits span the definition and possibly every use, across TUs.
  - They can use this design's builder and gate, with group atomicity and
    D5 agreement. The gate would need a whole-database run, not just
    "TUs that read the file".
  - Its own design (D4: later).

## 11. Open questions for the user

- **Q1. `cond-typo`: INFO or MAYBE?**
  1. INFO (recommended): a name never defined in this configuration may be
     defined in another; only the user knows. Con: `fix --unsafe` will not
     touch it.
  2. MAYBE: applied with `--unsafe`. Con: it can break another
     configuration silently, and the gate cannot see that.
- **Q2. Exit codes of `fix`.** The proposal is 0/1/2/3 plus a new 4 for
  read or write failure, and remaining diagnostics are ignored.
  1. As proposed (recommended).
  2. Fold 4 into 1. Simpler, but CI cannot tell "work left" from "could
     not write".
  3. Also return 1 when errors remain after fixing. Con: it duplicates
     `check`.
- **Q3. Warning fixes in a TU with errors:** defer them to an error-free
  pass (proposed), or apply them anyway?
  1. Defer (recommended): diagnostics after an error are less reliable,
     and the gate's "no new diagnostics" is weaker there.
  2. Apply: more progress per run on broken code.
- **Q4. LSP fix-all:** gate it on the builder (proposed), or apply the
  SAFE edits without a gate?
  1. Gate (recommended): the same guarantee as the CLI. Con: up to one
     check's latency.
  2. No gate: instant. Con: SAFE then means less in the editor than in
     the CLI.
- **Q5. Max passes default 8, and `--diff` limited to one pass.**
  Recommended as is: cycles are bugs, and 8 bounds them.
- **Q6. JSON location shape.**
  1. Keep cereal's `json_loc` (recommended): consistent with the rest of
     cereal's JSON.
  2. gcc's `display-column`/`byte-column`/`column` triple: closer to gcc.
     Con: inconsistent inside one document.
- **Q7. The BOM-over-64-KiB fix.** Bundle it in C1 (recommended: fix-it
  columns depend on it, and the fix is three lines), or fix it right away
  as its own round?
- **Q8. Expected `)` and the other non-`;` insertions as INFO.** The
  proposal is INFO. The alternative is MAYBE behind `--unsafe` with the D6
  limit. Recommended: INFO until the C1 differential shows how often
  gcc's guess compiles.
- **Q9. Header-guard demotion rule.** "MAYBE if the misspelt name is
  tested anywhere else." Is that right, or should header-guard always be
  MAYBE?
- **Q10. D6's reach.** Does the one-error-fix-per-pass limit cover every
  error group (proposed, 6.2), or only the missing `;`?
  1. Every error group (recommended): later error fix-its are computed
     from recovery state.
  2. Only `;`: faster on files with many independent errors, such as
     several unknown-type headers.

## 12. Risks and limits

### 12.1 Limits stated, not solved

- **The gate only sees the TUs in the run.** A header edit can break a TU
  that is not in the database, or one in another configuration. D5 helps
  only within the run.
- **`-Wno-x` in an entry removes it from D5's count** (6.2). A TU that
  disables the diagnostic is not checked for agreement, but it is still
  re-gated.
- **The LSP sees one configuration.** Its fix-all gate is per unit.

### 12.2 Risks

- **Attaching analyses to a check TU is new** (6.1). Today they run only
  in lint mode and on the LSP's macro-phase TU.
  - If it breaks an invariant, the fallback is two runs per TU, lint and
    check, which roughly doubles the collect cost.
  - Probe it first in C3.
- **Bisection cost** on a large database with many failing groups. It is
  bounded by groups × log2(groups) re-runs, and the max-passes cap limits
  it further.
- **`ast_digest` blind spots.** Some meaning is not in the tree: an
  implicit conversion decided by the type of a parenthesized
  subexpression, for example. The digest therefore hashes each
  expression node's type too.
  - `fixgate.sh`'s object-code identity is the backstop check.
- **gcc rules recalled, now probed in C1** (section 13, P11 to P13, P18,
  P19): the newline and tab rules are not observable in gcc-13's front end;
  the other three matched or changed the design as recorded in 3.2 and 3.4.
- **Format fix-its (C2b)** depend on the format checker's spelling of
  conversions. Their size is uncertain.

### 12.3 Parked

Moved to ../PARKED.md (fix-its).

## 13. Probes (gcc 13.4.0, WSL, 2026-10-10)

| # | Probe | Result |
|---|---|---|
| P1 | did-you-mean on tab-indented `countr` | `{12:2-12:8}:"counter"`: byte columns, exclusive end. The header uses display columns |
| P2 | `é` in text; file named `e "x".c` | `\303\251`; `"e \"x\".c"` |
| P3 | `printf` without `<stdio.h>`; `size_t` and `uint8_t` unknown | Fix-it on the note `include '<stdio.h>' or provide a declaration`: `{2:1-2:1}:"#include <stdio.h>\n"`. Only the first note per header has it. Two insertions at 1:1 in diagnostic order. Logical-not note: separate `(` and `)` insertions |
| P4 | pr89520-1.c | Fix-it lines come after the owner's "in expansion of macro" notes |
| P5 | `ID(p.m)`, `ID(countr)`, `return ONE }`, `return ID(2) }`, `ID(p).m` | No fix-it on any macro-expanded token, arguments included. `ID(p).m` gets `->` (the `.` is outside) |
| P6 | `-fdiagnostics-generate-patch` | Unified diff on stderr after the TU's diagnostics. `--- a.c`/`+++ a.c`, 3 context lines. All fix-its applied, notes and every `;` included |
| P7 | `-fdiagnostics-format=json` | `fixits` on diagnostics and children. Entries `{start, next, string}`; locations with `file`, `line`, `display-column`, `byte-column`, `column` |
| P8 | `#line 100 "other.c"` before a typo | `fix-it:"other.c":{100:39-100:45}:"counter"`. Generate-patch printed nothing for the whole TU |
| P9 | BOM file; CRLF file | Columns do not count the BOM. CRLF columns unaffected. The patch keeps `^M` |
| P10 | BOM file of 64 KiB or more | gcc rc 0. cereal: `unknown type name '\U0000feffint'` (bug, section 2) |
| P14 | Survey: gcc-13 `-Wall -Wextra -std=gnu17` over gcc.dg + c-c++-common | 1438 fix-it lines in 201 files; classes in 4.1 |
| P15 | `pp.m` with `struct s **pp`; `int a[2][2] = {1,2,3,4}` | No fix-it for a pointer to pointer. Missing braces: four insertions, `{` at 3:17, `}` at 3:21, `{` at 3:23, `}` at 3:28 |
| P16 | cereal today with both `-f` flags and `fix` | "unknown option", exit 2; `fix` prints usage, exit 2 |
| P17 | `.` to `->`, `expected ';'` across lines, `expected ')'`, `#elsif`, union member did-you-mean | All give fix-its, as in 4.1. No fix-it for: assignment parens, strict prototypes, unused variable, `expected '}' before numeric constant`, `expected ',' or ';'` |

Probed in C1 (2026-10-10, gcc 13.4.0 on WSL; `gcc-13` is the oracle, plain
`gcc` is 15.2):

| # | Probe | Result |
|---|---|---|
| P11 | The newline rule and the `\t` escape | No gcc-13 front-end diagnostic emits a tab or a newline except the include insertion (`"#include <h>\n"`), so neither can be observed. cereal follows `print_escaped_string` (`\\ \" \t \n`, octal for other non-printables) and keeps the newline rule as a guard |
| P12 | `isalpha`, `isdigit`, `toupper` used without `<ctype.h>` (not in `known[]`) | Only the first note carries the fix-it; the other two notes are at their use. Same for any header, so the rule is generic (3.2) |
| P13 | `printf`, `strlen`, `isalpha` twice, after a prior `#include`, with `-fdiagnostics-generate-patch` | Three insertions at one point, in diagnostic order; the patch is byte-identical to cereal's. The C1 producers cannot make two fix-its overlap, so the invalidate-on-overlap rule is not observable and was dropped from 3.4 |
| P18 | `printf` used in a header with no `#include`, itself included from a `.c` (gcc.dg missing-header-fixit-2) | The note and fix-it are at `1:1` of the header, not in the main file. `include_loc` uses the file of the use |
| P19 | Differential over 7042 gcc.dg and c-c++-common files (`fixits.sh`) | `->` on a pointer to pointer does get fix-its: `(*` before the operand and `)` after (MAYBE). A missing `(` is offered before the token found (`asm const (`, `if foo ()`), while `)` `]` `;` `,` `:` go after the previous token. Touching hints of one diagnostic are merged into one |

## 14. Review log

There were five adversarial passes over the draft. Each one re-checked
claims against the code at `cea7869` and against the probe output.

**Round 1: claims against the code.** Every `file:line` was re-read.

- Corrected line numbers:
  - `mode_compdb`'s status line is `main.c:699` (not 700);
  - `lint_one` is at `main.c:159`;
  - `cdec_diag` is at `cell.c:782`.
- `ccall.c:2255` is the *pointer-to-pointer* message. gcc gives no fix-it
  there (P15, probed for this round), so the table now names only `2259`.
- There is no `util.c` (the helper goes in `common.c`). There is no C
  unit-test harness: the Makefile builds only `cereal`. The builder's
  "unit test" became a testing input, `fix --edits` (5.3).
- Confirmed:
  - `crename.c` compares diagnostics pairwise by level, id and place, and
    maps only main-file offsets. The gate's generalization to every
    edited file is now stated (6.4).
  - `par.py`'s gcc cache key already includes the flags.
  - The LSP's macro-phase TU runs the analyses (`server.c:197`), so lint
    fix-its reach the LSP without the check.
  - Both `-f` flags are "unknown option" today.

**Round 2: internal consistency.**

- Fixed stale cross-references: the builder is section 5, not 6.2, and the
  nonexistent "9.2".
- The missing-braces row claimed a gate on folded initialiser values that
  6.4 did not define. Missing braces are now SAFE by construction, with
  `fixgate.sh` as the check.
- The `ast_digest` description, a flat tag sequence, contradicted the
  risk it was meant to answer. It now hashes tree shape (child counts) and
  types.
- The 3.1 field list was miscounted.
- Whole-diagnostic poisoning would have broken the gcc differential for a
  note refused while its diagnostic's fix-it is valid. Poisoning is now
  per owner, as gcc's separate rich locations are.

**Round 3: the brief, item by item.** Every listed topic has a section:

- data and producers: 3, 4;
- real-location guarantee: 3.2;
- builder semantics: 5.2;
- LSP kind, association, fix-all, capabilities, builder thread, carrying,
  incremental edits: 7;
- replay, `-j`, determinism: 6.2, 6.7;
- idempotence and gate: 6.2, 6.4;
- CRLF, encoding, BOM, unwritable, atomic: 6.5;
- exit codes: 6.6;
- C2 list with the assignment-parens decision: 4.2;
- tests per slice, golden layout, gcc differential, semantic gate: 8;
- sizes: 8.

Findings:

- `fix --edits` was placed in C1 while `fix` itself is C3. It is now
  explicitly the first piece of the mode.
- The brief's "bump format" is moot (no on-disk cell format, section 2).
- The brief labels the user's decisions "D1..D7" but lists six. D7 records
  its scope rule.
- D6's wording could be read as applying to `;` only. The broader reading
  is now Q10.

**Round 4: edge cases.**

- Headers reached through `-I` into vendored or installed trees are
  `SF_USER` and would have been rewritten. Added `--root` (6.5).
- A file read under different spellings by different TUs needs the
  overlay keyed per TU's path (6.4).
- The D5 "same text hash" condition was redundant within one disk read.
  It was reworded as a changed-during-run check.
- Warning fixes deferred in a TU whose errors have no fix leave exit
  status 1. Now stated.
- The LSP fix-all selection was undefined in a TU with errors. It now
  uses one CLI pass's selection, without D5.
- The cross-file wording in 6.5 was garbled and has been rewritten.

**Round 5: a full pass.** One material finding: `fixgate.sh`'s object
identity would fail on every include insertion in a file that uses
`__LINE__`/`assert`, because later lines move. Its comparison now
separates line-moving fixes (8, C3). A further pass found nothing
material.

**Probed in C1** (section 13, P11 to P13, P18, P19): the newline and tab
rules (not observable), include notes for headers outside `known[]`
(generic once-per-header), the patch on overlapping fix-its (not
producible; dropped), adjacent fix-its (touching hints merge).

The gate's cost on a large database is estimated, not measured.
