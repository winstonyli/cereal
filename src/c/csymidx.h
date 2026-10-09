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
    CIDF_TENTATIVE = 8       /* defined by a tentative definition */
};

typedef struct CIdxFile {    /* the files that have events */
    const char *path;        /* normalized, as srcmgr has it */
    uint32_t size;
    uint64_t hash;           /* cindex_hash of the text the check read */
    bool stale;              /* the language server's text differs */
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
    uint32_t hover;          /* string pool offset (phase 3; 0: none yet) */
    uint32_t scope;          /* scope id (phase 4; 0 for now) */
    uint8_t kind;            /* CIdxKind */
    uint8_t linkage;         /* 0 none, 1 internal, 2 external */
    uint16_t flags;          /* CIDF_* */
} CIdxDecl;

typedef struct CIndex {
    CIdxFile *files;
    uint32_t nfiles;
    CIdxEvent *ev;
    uint32_t nev;
    uint32_t *by_decl, *by_decl_start;  /* CSR: event indices per decl */
    CIdxDecl *decls;
    uint32_t ndecls;
    char *strings;           /* deduplicated pool; offset 0 is "" */
    size_t nstrings;
    uint32_t unindexed;      /* --verify-symbols: names that got no event */
} CIndex;

void cindex_free(CIndex *ix);
uint64_t cindex_hash(const char *text, size_t n);
/* Retained bytes. */
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
/* A field (index in the type table's fields) at tok. */
void csx_field(struct Checker *c, uint32_t field, uint32_t tok, int role);
/* Labels by slot (cstmt.c): a new label in slot, then events. */
void csx_label_new(struct Checker *c, uint32_t slot, uint32_t name, SrcLoc loc);
void csx_label(struct Checker *c, uint32_t slot, uint32_t tok, int role);

#endif
