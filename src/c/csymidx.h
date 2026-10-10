/* csymidx.h - the C symbol index (docs/B2_DESIGN.md).
 *
 * While a translation unit is checked, hooks in the checker record one
 * event per declaration, definition and use of a C entity (functions,
 * objects, parameters, typedefs, enumerators, fields, tags, labels).  At
 * the end the events are frozen into a CIndex keyed by (file path, byte
 * offset): the only key the language server's two build phases share.
 * A CIndex holds no checker types and outlives the checker.
 *
 * The hooks are off unless CheckOptions.symidx is set (Checker.sx NULL):
 * `cereal check` pays one branch per hook site. */
#ifndef CEREAL_CSYMIDX_H
#define CEREAL_CSYMIDX_H

#include "common.h"
#include "srcmgr.h"

/* CIdxEvent.flags: the role in the low two bits, then where it is spelled. */
enum {
    CIX_DECL = 0,            /* a declaration that does not define */
    CIX_DEF = 1,             /* the defining declaration */
    CIX_REF = 2,             /* a use */
    CIX_ROLE = 3,
    CIX_MACRO_BODY = 4,      /* spelled in a #define body */
    CIX_AT_EXPANSION = 8,    /* pasted or synthesized: at the expansion point */
    CIX_ARG = 16,            /* spelled in source as a macro argument */
    CIX_SYSTEM = 32          /* in a system header */
};

/* CIdxDecl.kind */
typedef enum {
    CIK_FUNC, CIK_OBJ, CIK_PARAM, CIK_TYPEDEF, CIK_ENUMCONST, CIK_FIELD,
    CIK_STRUCT, CIK_UNION, CIK_ENUM, CIK_LABEL
} CIdxKind;

/* CIdxDecl.flags */
enum {
    CIDF_BUILTIN = 1,        /* predeclared, no location */
    CIDF_IMPLICIT = 2,       /* implicitly declared function */
    CIDF_SYSTEM = 4,         /* declared in a system header */
    CIDF_TENTATIVE = 8,      /* defined by a tentative definition */
    CIDF_READONLY = 16,      /* a const object, parameter or field (arrays
                                stripped), every enumerator */
    CIDF_STATIC = 32         /* internal linkage, or static at block scope */
};

typedef struct CIdxFile {    /* the files that have events */
    const char *path;        /* normalized, as srcmgr has it */
    uint32_t size;
    uint64_t hash;           /* cindex_hash of the text the check read */
    bool stale;              /* the language server's text differs */
    /* carried index (B3_DESIGN.md 12): the text was edited since the check;
     * the damage [dmg_begin, dmg_end] (inclusive) is in the current text */
    bool edited;
    uint32_t dmg_begin, dmg_end;
} CIdxFile;

typedef struct CIdxEvent {   /* sorted by (file, off) */
    uint32_t off;            /* byte offset in the file */
    uint32_t decl;           /* index in CIndex.decls */
    uint16_t file;
    uint8_t len;             /* of the name; 255: 255 or longer */
    uint8_t flags;
} CIdxEvent;

typedef struct CIdxDecl {
    uint32_t name;           /* string pool offset */
    uint32_t hover;          /* string pool offset: the --dump-types line, a
                                record's or enum's members (CIX_HOVER_*);
                                equal texts share one offset */
    uint32_t scope;          /* where it can be named: 0 the whole unit, else
                                1 + index in CIndex.scopes (a block-scope
                                name without linkage: its innermost scope;
                                a label: its external declaration) */
    uint32_t type;           /* 1 + the decl of its type (B3_DESIGN.md 3.3), 0 */
    uint32_t parent;         /* 1 + a field's record, an enumerator's enum, 0 */
    uint8_t kind;            /* CIdxKind */
    uint8_t linkage;         /* 0 none, 1 internal, 2 external */
    uint16_t flags;          /* CIDF_* */
} CIdxDecl;

/* [begin, end) of file: the external declarations (roots) and the checker's
 * scopes inside them (blocks, prototypes, function bodies); sorted by (file,
 * begin, end descending), properly nested. */
typedef struct CIdxScope {
    uint32_t begin, end;
    uint32_t file;
    uint32_t parent;         /* 1 + the enclosing scope; 0: a root */
} CIdxScope;

typedef struct CIndex {
    CIdxFile *files;
    uint32_t nfiles;
    CIdxEvent *ev;
    uint32_t nev;
    uint32_t *by_decl, *by_decl_start;  /* CSR: event indices per decl */
    CIdxDecl *decls;
    uint32_t ndecls;
    CIdxScope *scopes;
    uint32_t nscopes;
    char *strings;           /* deduplicated pool; offset 0 is "" */
    size_t nstrings;
    uint32_t unindexed;      /* --verify-symbols: names that got no event */
    uint32_t excused;        /* ... and names left out for a diagnosed line etc. */
    /* Lifetime and identity (B3_DESIGN.md 12.2 K3).  refs: atomic, one on
     * creation; serial: unique per index; core: for a carried index, the
     * index that owns decls and strings (it holds a reference on it). */
    uint32_t refs;
    uint32_t serial;
    struct CIndex *core;
} CIndex;

/* Drops a reference; the last one frees the index. */
void cindex_free(CIndex *ix);
/* Takes a reference (NULL ok); returns ix. */
CIndex *cindex_ref(CIndex *ix);
uint64_t cindex_hash(const char *text, size_t n);
/* Retained bytes of this index (a carried one: not those it shares). */
size_t cindex_bytes(const CIndex *ix);
/* The file entry for a normalized path, -1 if none. */
int cindex_file(const CIndex *ix, const char *path);
/* The events whose name covers offset off of file fi (off may be just past
 * the name): [*first, *first + return). */
size_t cindex_at(const CIndex *ix, uint32_t fi, uint32_t off, uint32_t *first);
static inline const char *cindex_name(const CIndex *ix, uint32_t decl)
{
    return ix->strings + ix->decls[decl].name;
}
/* The distinct decls named at offset off of the file with this path (none
 * if the index lacks the file or it is stale), at most max into out. */
size_t cindex_decls_at(const CIndex *ix, const char *path, uint32_t off,
                       uint32_t *out, size_t max);
/* The first event at or after offset off of file fi (ix->nev if none). */
uint32_t cindex_lower(const CIndex *ix, uint32_t fi, uint32_t off);
/* The distinct type decls (CIdxDecl.type) of decls[0..nd) into out (room for
 * nd); how many. */
size_t cindex_types(const CIndex *ix, const uint32_t *decls, size_t nd,
                    uint32_t *out);
/* The innermost scope containing offset off of file fi: 1 + its index, 0 if
 * none (file level outside every external declaration). */
uint32_t cindex_scope_at(const CIndex *ix, uint32_t fi, uint32_t off);
/* The root (external declaration) of scope s (1 + index): 1 + its index. */
uint32_t cindex_scope_root(const CIndex *ix, uint32_t s);
/* The decls of the ordinary namespace (functions, objects, parameters,
 * typedefs, enumerators) visible at offset off of path, one per name (the
 * innermost declaration), sorted by name; *out malloc'd.  None if the file is
 * not in the index or stale.  B3_DESIGN.md section 4. */
size_t cindex_visible(const CIndex *ix, const char *path, uint32_t off,
                      uint32_t **out);

/* What cindex_select takes from the events of a set of decls (requests of
 * B2_DESIGN.md section 6). */
typedef enum {
    CIQ_DEF,     /* each decl's DEF events, or its DECL events if none */
    CIQ_DECL,    /* each decl's DECL events, or its DEF events if none */
    CIQ_REFS,    /* every event but those spelled in a #define body (a
                    body's tokens are not uses where they are written) */
    CIQ_USES     /* CIQ_REFS without DECL and DEF (includeDeclaration off) */
} CIdxQuery;
/* The events of decls[0..nd) that q selects, as indices into ix->ev in
 * (file, offset) order, one per location (a DECL or DEF event rather than a
 * REF at the same place); events in stale files are left out, and so are
 * those outside file fi unless fi < 0.  *out is malloc'd (free it). */
size_t cindex_select(const CIndex *ix, const uint32_t *decls, size_t nd,
                     CIdxQuery q, int fi, uint32_t **out);
/* Rename (B2_DESIGN.md, phase 4 addendum A3.2-A3.5): the events to edit to
 * rename the C entity named at offset off of path, one per place in offset
 * order, all in the unit's main file (*ev malloc'd); NULL, or why not (a
 * message in msg).  sm: for the places in messages. */
const char *cindex_rename_plan(const CIndex *ix, SrcMgr *sm, const char *path,
                               uint32_t off, const char *main, uint32_t **ev,
                               size_t *n, char *msg, size_t msgsz);
/* [A-Za-z_][A-Za-z0-9_]* */
bool cindex_is_identifier(const char *s);
/* A byte that can continue an identifier ($ and UTF-8 included). */
static inline bool cindex_ident_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '$' ||
           (unsigned char)ch >= 0x80;
}
/* "FILE:LINE:COL" of offset off of the file with this path, into buf. */
const char *cindex_place(SrcMgr *sm, const char *path, uint32_t off, char *buf,
                         size_t bufsz);
/* Hover text bounds:a string longer than CIX_HOVER_MAX bytes is cut there
 * ("..."), a record or enum lists at most CIX_HOVER_MEMBERS members. */
#define CIX_HOVER_MAX 1024
#define CIX_HOVER_MEMBERS 16
/* The distinct hover texts of decls[0..nd), at most 5 then "and N more",
 * appended to out: as ```c blocks separated by rules (md), else one after
 * another on their own lines. */
void cindex_hover(const CIndex *ix, const uint32_t *decls, size_t nd, bool md,
                  StrBuf *out);
/* Marks the files whose text in sm (by path) differs from what the check
 * read, or which sm lacks, stale. */
void cindex_validate(CIndex *ix, SrcMgr *sm);
/* ---- carrying an index across an edit (B3_DESIGN.md 12) ----------------- */

typedef struct CIdxEdit {    /* old [pre, old_end) became new [pre, new_end) */
    uint32_t pre, old_end, new_end;
    bool same;               /* identical texts */
} CIdxEdit;
/* The edit from text a to text b: their common prefix and suffix, widened to
 * whole identifiers. */
void cindex_text_edit(const char *a, size_t na, const char *b, size_t nb,
                      CIdxEdit *out);
/* The map of an old position (a point between bytes) through the edit:
 * monotone; a position inside the span maps to its start. */
uint32_t cindex_edit_map(const CIdxEdit *e, uint32_t x);
/* An index in the coordinates of sm_new's texts, made from `from` (whose
 * offsets are in sm_old's): the per-text arrays copied, offsets shifted past
 * each file's edit, events overlapping it dropped; decls and strings shared
 * with from's core.  Files `from` marks stale, or sm_new lacks, stay or
 * become stale.  With no change, from itself with one more reference. */
CIndex *cindex_carry(CIndex *from, SrcMgr *sm_old, SrcMgr *sm_new);
/* Offset off of file fi lies in the file's damage (inclusive at both ends). */
bool cindex_damaged(const CIndex *ix, int fi, uint32_t off);
/* A carried index has fewer DECL/DEF events for decl d than its core: the
 * declared name itself was edited. */
bool cindex_touched(const CIndex *ix, uint32_t d);
/* --verify-carry: c, ix1 carried to the text in sm2, against fresh (the index
 * of sm2's text): the structure of c, then the events and scopes outside the
 * damage compared; prints the problems and "carry: kept K of N events,
 * damage L:C-L:C, D differences".  Returns the structural problems
 * (differences are reported, not counted). */
size_t cindex_verify_carry(const CIndex *ix1, const CIndex *c,
                           const CIndex *fresh, SrcMgr *sm2, FILE *out);

/* sm's user or system file with exactly this (normalized) path, or NULL. */
SrcFile *cindex_srcfile(SrcMgr *sm, const char *path);
/* "DECL", "DEF", "REF"; "func", "obj", ...; " body expansion arg system" */
const char *cindex_role_name(unsigned flags);
const char *cindex_kind_name(unsigned kind);
void cindex_print_flags(FILE *out, unsigned flags);
/* --dump-symbols: every event (file:line:col ROLE kind name -> #decl), then
 * the decls.  sm: the check's, for line numbers. */
void cindex_dump(const CIndex *ix, SrcMgr *sm, FILE *out);
/* --verify-symbols: the structural checks (sorted events, CSR, a DECL or
 * DEF event per decl, file hashes) and the unit-end coverage count;
 * prints the problems and a summary, returns how many there were. */
size_t cindex_verify(const CIndex *ix, SrcMgr *sm, FILE *out);

/* ---- the builder (the checker's side; src/c only) ---------------------- */

struct Checker;
struct CSym;
typedef struct SymIdxB SymIdxB;

/* verify: report names the hooks missed to it, unit by unit (NULL: off). */
SymIdxB *csx_new(FILE *verify);
void csx_free(SymIdxB *b);
void csx_unit_begin(struct Checker *c);
void csx_unit_end(struct Checker *c);
/* A checker scope opens at token tok (open), or closes at it. */
void csx_scope(struct Checker *c, uint32_t tok, bool open);
/* The frozen index; the builder is emptied.  NULL without one. */
CIndex *csx_finish(struct Checker *c);

/* The hooks.  tok: the name token, an index into the unit's tokens
 * (CSX_NOTOK: none, no event). */
#define CSX_NOTOK 0xFFFFFFFFu
/* x was declared at tok (x: the declaration's own symbol, before any merge;
 * file: at file scope); ref: the symbol the name now denotes. */
void csx_decl(struct Checker *c, uint32_t ref, const struct CSym *x, bool file,
              uint32_t tok);
/* An ordinary symbol at tok in role CIX_DEF / CIX_REF ... */
void csx_sym(struct Checker *c, uint32_t ref, uint32_t tok, int role);
/* A function definition's parameter: its declaration defines it. */
void csx_param_def(struct Checker *c, uint32_t ref);
/* A tag (TypeId t) at tok; redecl: the name at tok now declares t instead
 * of what an earlier hook recorded there (`struct S;` shadowing). */
void csx_tag(struct Checker *c, uint32_t t, uint32_t tok, int role);
void csx_tag_redecl(struct Checker *c, uint32_t t, uint32_t tok);
/* An enum (TypeId t) was completed with the enumerators ecs[0..n) (symbol
 * refs): its hover text. */
void csx_enum(struct Checker *c, uint32_t t, const uint32_t *ecs, uint32_t n);
/* A field (index in the type table's fields) at tok. */
void csx_field(struct Checker *c, uint32_t field, uint32_t tok, int role);
/* Labels by slot (cstmt.c): a new label in slot, then events. */
void csx_label_new(struct Checker *c, uint32_t slot, uint32_t name, SrcLoc loc);
void csx_label(struct Checker *c, uint32_t slot, uint32_t tok, int role);
/* A name that is no entity (verify: not missed), e.g. an identifier list in a
 * declaration that is not a definition. */
void csx_skip(struct Checker *c, uint32_t tok);

#endif
