/* csum.h - per-unit summaries and read sets (docs/TYPES.md, "Summaries").
 *
 * A unit's SUMMARY is what it declares or defines at file scope: for each
 * entity its name, kind, linkage, storage, type digest, definition-present
 * flag, enumerator value, record layout digest, inline/attribute bits, and
 * a digest of the lot.  Its READ SET is the file-scope names it looked up
 * while it was checked (ordinary identifiers, tags, external linkage
 * names; misses included) with the digest of what each denoted, plus the
 * layouts of records and enums it read the contents of.  A unit's results
 * are still good if every read yields the same digest in the new
 * file-scope state (summary_valid).  All digests are 64-bit, structural,
 * and independent of interner ids, type ids, source positions. */
#ifndef CEREAL_CSUM_H
#define CEREAL_CSUM_H

#include "c/check.h"

/* Namespaces of summary entries and reads. */
enum {
    SUM_ORD,        /* ordinary identifiers at file scope */
    SUM_TAG,        /* struct/union/enum tags at file scope */
    SUM_EXT,        /* the external-linkage view of a name (block-scope
                       externs, implicit declarations) */
    SUM_LAYOUT,     /* a record's / enum's contents, keyed by identity */
    SUM_STATE       /* checker state carried between units: "pack" */
};

/* What a read needs of the entity. */
enum {
    RD_NAME,        /* its interface: kind, type, linkage, attributes (for a
                       tag: which type it is, not what is in it) */
    RD_FULL,        /* the above plus whether it is defined: the unit
                       declares or defines the name itself */
    RD_LAYOUT       /* SUM_LAYOUT: size, alignment, members (enums:
                       underlying type, completeness) */
};

/* SumEntry.flags (stable bit values; they are part of the digests). */
enum {
    SF_DEFINED = 1, SF_TENTATIVE = 2, SF_INLINE = 4, SF_THREAD = 8,
    SF_NORETURN = 16, SF_WEAK = 32, SF_IMPLICIT = 64, SF_ERROR = 128,
    SF_PROTO_DEF = 256, SF_KR_DEF = 512, SF_CONST_INIT = 1024,
    SF_DECL_EXTERNAL = 2048, SF_COMPLETE = 4096, SF_REGISTER = 8192
};

typedef struct SumEntry {
    const char *name;
    uint8_t ns;             /* SUM_ORD, SUM_TAG, SUM_EXT, SUM_STATE */
    uint8_t kind;           /* SUM_ORD/EXT: CS_OBJ..CS_ENUMCONST; SUM_TAG:
                               0 struct, 1 union, 2 enum */
    uint8_t linkage;        /* LK_* */
    uint8_t sc;             /* StorageClass as written */
    uint32_t flags;         /* SF_* */
    uint64_t type_dig;      /* structural digest of the type */
    uint64_t layout_dig;    /* tags: layout digest (incomplete: a constant) */
    uint64_t value;         /* enumerators: the value */
    uint64_t iface;         /* digest of what readers see (no definition) */
    uint64_t full;          /* iface plus the definition-present bits */
    uint64_t digest;        /* what the unit's digest takes of it: full, and
                               for tags the layout, with ns and name */
    char *type_text;        /* the type as printed (--dump-summaries), or NULL */
} SumEntry;

typedef struct SumRead {
    const char *name;       /* "-" if anonymous */
    uint8_t ns, mode;       /* SUM_*, RD_* */
    uint64_t key;           /* SUM_LAYOUT: the record's identity digest */
    uint64_t digest;        /* 0: the name was not bound */
} SumRead;

typedef struct UnitSummary {
    uint64_t key;           /* the unit's identity (tags are identified by
                               it); see checker_set_unit_key */
    uint64_t digest;        /* all the entries */
    uint64_t sig;           /* the entries without implicit declarations
                               made by function bodies */
    bool errors;            /* the unit had syntax errors */
    SumEntry *entries;      /* sorted by (ns, name) */
    size_t nentries;
    SumRead *reads;         /* sorted by (ns, mode, name, key) */
    size_t nreads;
    bool owned;             /* loaded by summary_load: free with summary_free */
    void *priv;
} UnitSummary;

/* The summary of the unit checker_unit last checked (NULL unless
 * CheckOptions.summaries); valid until the next checker_unit. */
const UnitSummary *checker_summary(const Checker *c);
/* Sets the identity of the next unit.  The default, when none is set, is a
 * digest of the names the unit declares (so editing a body or a member
 * list keeps it).  P3 may pass its own stable unit key. */
void checker_set_unit_key(Checker *c, uint64_t key);

/* The digest of a file-scope entity in the checker's current state, in the
 * shape summary_valid wants: ns, name, key (SUM_LAYOUT), mode; 0 if absent. */
uint64_t checker_file_digest(void *checker, int ns, const char *name,
                             uint64_t key, int mode);

typedef uint64_t (*SumLookup)(void *ctx, int ns, const char *name,
                              uint64_t key, int mode);
/* Is every read of s still satisfied?  lookup answers each with the digest
 * in the state to validate against (checker_file_digest with ctx = the
 * Checker is one).  *bad gets the first read that is not. */
bool summary_valid(const UnitSummary *s, SumLookup lookup, void *ctx,
                   const SumRead **bad);

/* Stable text serialization; summary_load reads back the read sets and unit
 * digests (entries are not reloaded) of every `unit` block of a dump.
 * Returns the number of units, -1 on a syntax error. */
void summary_write(FILE *out, const UnitSummary *s, size_t index);
long summary_load(FILE *in, UnitSummary **out);
void summary_free(UnitSummary *s, size_t n);

/* ---- hooks, called by the checker (csum.c) -------------------------------- */

/* The read stamps clookup looks at before calling csum_read (inline fast
 * path: a name already read in this unit costs two loads). */
typedef struct CSumFast {
    uint32_t *rs[2];        /* by namespace, by ident: the unit's stamp */
    size_t rn[2];
    uint32_t seq;
} CSumFast;

struct CSum;
struct CSum *csum_new(Checker *c);
void csum_unit_begin(Checker *c);
void csum_unit_end(Checker *c);
void csum_free(Checker *c);
/* A file-scope lookup in namespace ns (0 ordinary, 1 tag) found binding b
 * (log index + 1, 0: none) and it is the file-scope one. */
void csum_read(Checker *c, int ns, uint32_t ident, uint32_t b);
/* The external-linkage view of ident was read. */
void csum_read_ext(Checker *c, uint32_t ident);
/* Is about to be declared, redeclared, completed or defined at file scope
 * (ns 0 ordinary, 1 tag, 2 external): the unit reads what was there, and
 * the name joins its summary. */
void csum_touch(Checker *c, int ns, uint32_t ident);
/* A declaration of ident (pushdecl): file scope or an external one. */
void csum_decl(Checker *c, uint32_t ident, bool filescope, bool external);
/* A record's layout used #pragma pack state. */
void csum_read_pack(Checker *c);

#endif
