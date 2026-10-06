/* type.h - the type table.  See docs/TYPES.md.
 *
 * A TypeId is an index into the table shifted left four bits, with the
 * qualifiers in the low bits, so qualifying a type never allocates.  The
 * builtin types have index == TypeKind.  Derived types (pointer, array,
 * function, vector, complex) are hash-consed: building the same type twice
 * gives the same id, so identity of canonical types is integer equality.
 * Typedef entries are one per declaration (they are how a type prints) and
 * each entry knows its canonical type, with every typedef stripped.
 *
 * Records and enums have their own tables; their table entry is created
 * once per tag declaration and the record completes in place. */
#ifndef CEREAL_TYPE_H
#define CEREAL_TYPE_H

#include "common.h"
#include "c/target.h"
#include "intern.h"
#include "srcmgr.h"

typedef uint32_t TypeId;

enum {
    TQ_CONST = 1,
    TQ_VOLATILE = 2,
    TQ_RESTRICT = 4,
    TQ_ATOMIC = 8,
    TQ_MASK = 15
};

#define TYPE_IDX(t) ((t) >> 4)
#define TYPE_QUALS(t) ((t) & TQ_MASK)
#define TYPE_UNQUAL(t) ((t) & ~(TypeId)TQ_MASK)
#define TYPE_MK(idx, q) ((TypeId)(idx) << 4 | (q))
#define TYPE_B(kind) TYPE_MK(TY_##kind, 0)    /* builtin: TYPE_B(INT) */

/* TypeEnt.flags */
enum {
    TF_INCOMPLETE = 1,       /* array: [] */
    TF_SIZED = 4,            /* VLA: the count is a known constant (array of variably modified type) */
    TF_VARIADIC = 2,         /* function: ... */
    TF_NOPROTO = 4,          /* function: () declared without a prototype */
    TF_ALIGNED = 8,          /* typedef: .align overrides */
    TF_SYSHDR = 16,          /* typedef: declared in a system header */
    TF_MAYALIAS = 32,        /* typedef: may_alias */
    TF_NOCF = 64,            /* function: nocf_check (part of the type) */
    TF_FLEX = 64,            /* incomplete array: a flexible array member (shares the bit) */
    TF_TXUNSAFE = 128        /* function: transaction_unsafe */
};

typedef struct TypeEnt {
    uint8_t kind;            /* TypeKind */
    uint8_t flags;
    uint16_t align;          /* typedef: aligned attribute, log2 + 1 */
    uint32_t base;           /* TypeId (see TypeKind) */
    uint32_t extra;          /* params offset / record / enum / name */
    TypeId canon;            /* the type with typedefs stripped */
    uint64_t n;              /* array count / param count / vector bytes */
} TypeEnt;

/* Record.flags */
enum {
    RF_UNION = 1,
    RF_COMPLETE = 2,
    RF_DEFINING = 4,         /* between { and } */
    RF_PACKED = 8,
    RF_FLEXIBLE = 16,        /* ends in a flexible array member */
    RF_CONST_MEMBER = 32,    /* a member (recursively) is const */
    RF_TRANSPARENT = 64,     /* transparent_union */
    RF_NOKEYWORD = 128,      /* prints without struct (__va_list_tag) */
    RF_VLA = 256,            /* has a variably modified member */
    RF_DESIGNATED = 512,     /* designated_init */
    RF_USER_ALIGN = 1024,    /* aligned attribute given (copy copies it) */
    RF_VMOD = 2048,          /* a member is variably modified (C_TYPE_VARIABLY_MODIFIED) */
    RF_SSO = 4096,           /* scalar_storage_order: pointers are not constants */
    RF_MAYALIAS = 8192,      /* may_alias */
    RF_IN_STRUCT = 16384     /* defined inside a struct or union body */
};

/* Field.flags */
enum {
    FF_BITFIELD = 1,
    FF_PACKED = 2
};

typedef struct Field {
    uint32_t name;           /* ident id; 0: unnamed */
    TypeId ty;
    uint64_t off_bits;
    uint32_t width;          /* bit-fields */
    uint16_t flags;
    uint32_t align;          /* aligned attribute (bytes), 0: none */
    SrcLoc loc;
    uint32_t dep, dmsg;      /* CSF_DEPRECATED/UNAVAILABLE bits; message */
    uint32_t aset;           /* attribute names (Checker.anames set id), 0: none */
} Field;

typedef struct Record {
    uint32_t tag;            /* ident id; 0: anonymous */
    SrcLoc loc;              /* definition, else first declaration */
    uint32_t fields, nfields; /* range in TypeTable.fields */
    uint64_t size;           /* bytes */
    uint32_t align;          /* bytes */
    uint16_t flags;
    TypeId ty;               /* its TY_STRUCT / TY_UNION type */
    uint32_t dep, dmsg;      /* CSF_DEPRECATED/UNAVAILABLE bits; message */
    uint32_t aset;           /* attribute names, as Field.aset */
} Record;

typedef struct Enum {
    uint32_t tag;
    SrcLoc loc;
    TypeId underlying;       /* uint until a value is negative */
    bool complete, packed;
    bool in_struct;          /* defined inside a struct or union body */
    TypeId ty;
    uint32_t dep, dmsg;
} Enum;

typedef struct TypeTable {
    const Target *tgt;
    Interner *in;
    VEC(TypeEnt) ents;
    uint32_t *slots;         /* hash-cons table: entry index, 0 empty */
    uint32_t slot_mask;
    uint32_t nhashed;
    VEC(TypeId) params;      /* function parameter lists */
    VEC(Record) recs;
    VEC(Field) fields;
    VEC(Enum) enums;
    TypeId va_list;          /* __builtin_va_list */
    bool any_packed;         /* a packed record or member was laid out */
    /* csum.c: records / enums below these indices are older than the unit
     * being checked; reading their contents calls rd_hook (idx, is_enum) */
    uint32_t unit_rec0, unit_enum0;
    void (*rd_hook)(void *ctx, uint32_t idx, bool is_enum);
    void *rd_ctx;
    StrBuf qbuf[4];          /* type_q */
    unsigned qnext;
    bool aka;                /* type_print: strip typedefs (aka spelling) */
    bool noq_top;            /* type_print: a top-level typedef without quals */
} TypeTable;

void types_init(TypeTable *tt, const Target *tgt, Interner *in);
void types_free(TypeTable *tt);

static inline const TypeEnt *type_ent(const TypeTable *tt, TypeId t)
{
    return &tt->ents.data[TYPE_IDX(t)];
}
static inline TypeKind type_kind(const TypeTable *tt, TypeId t)
{
    return (TypeKind)tt->ents.data[TYPE_IDX(t)].kind;
}
/* The canonical type: typedefs stripped, and the qualifiers of an array
 * moved to its element type (C99 6.7.3p8). */
TypeId type_canon(TypeTable *tt, TypeId t);
/* The kind of the canonical type. */
static inline TypeKind type_ckind(const TypeTable *tt, TypeId t)
{
    return (TypeKind)tt->ents.data[TYPE_IDX(tt->ents.data[TYPE_IDX(t)].canon)].kind;
}

/* ---- construction ---------------------------------------------------- */

TypeId type_ptr(TypeTable *tt, TypeId to);
TypeId type_array(TypeTable *tt, TypeId elem, uint64_t n);
TypeId type_array_incomplete(TypeTable *tt, TypeId elem);
TypeId type_array_flex(TypeTable *tt, TypeId elem);
TypeId type_vla(TypeTable *tt, TypeId elem);
TypeId type_vla_sized(TypeTable *tt, TypeId elem, uint64_t n);
/* A VLA that remembers its size expression's spelling (TYPE_EXTRA 0: none). */
TypeId type_vla_x(TypeTable *tt, TypeId elem, uint64_t n, bool sized,
                  uint32_t txt);
uint32_t type_vla_text(TypeTable *tt, const char *s);
/* flags: TF_VARIADIC, TF_NOPROTO. */
TypeId type_func(TypeTable *tt, TypeId ret, const TypeId *params,
                 uint32_t n, unsigned flags);
TypeId type_vector(TypeTable *tt, TypeId elem, uint64_t bytes);
TypeId type_complex(TypeTable *tt, TypeId real);
TypeId type_typedef(TypeTable *tt, uint32_t name, TypeId to);
/* Adds qualifiers (an array's go to its element when canonicalized). */
static inline TypeId type_qual(TypeId t, unsigned q) { return t | q; }

/* A new record / enum and its type; complete them later. */
TypeId type_new_record(TypeTable *tt, uint32_t tag, bool is_union,
                       SrcLoc loc);
TypeId type_new_enum(TypeTable *tt, uint32_t tag, SrcLoc loc);
/* A distinct copy of a complete record type (same tag, shared fields) that
 * is incompatible with the original and marked RF_SSO. */
TypeId type_clone_record(TypeTable *tt, TypeId t);
Record *type_record(TypeTable *tt, TypeId t);   /* canonical struct/union */
Enum *type_enum(TypeTable *tt, TypeId t);       /* canonical enum */

/* A member as the checker collects it, before layout. */
typedef struct FieldIn {
    uint32_t name;
    TypeId ty;
    int32_t width;           /* -1: not a bit-field */
    uint32_t align;          /* aligned attribute, 0: none */
    uint16_t wina;           /* warn_if_not_aligned attribute, 0: none */
    bool packed;
    SrcLoc loc;
    uint32_t dep, dmsg;
    uint32_t aset;
} FieldIn;

/* Lays out and completes a record.  pack: #pragma pack value in bytes (0:
 * none); align: the record's aligned attribute (0: none). */
bool type_packed_unnecessary(TypeTable *tt, TypeId t, const FieldIn *f,
                             uint32_t n, unsigned pack, unsigned align, int ms,
                             long only);
void type_complete_record(TypeTable *tt, TypeId t, const FieldIn *f,
                          uint32_t n, unsigned pack, unsigned align,
                          bool packed, int ms);  /* ms: 1 ms_struct, -1 gcc_struct, 0 target default */

/* Function accessors (canonical or not). */
static inline const TypeId *type_params(const TypeTable *tt, TypeId f)
{
    return tt->params.data + tt->ents.data[TYPE_IDX(f)].extra;
}

/* ---- queries (all look through typedefs) ----------------------------- */

bool type_is_complete(TypeTable *tt, TypeId t);
bool type_is_integer(TypeTable *tt, TypeId t);   /* enums and _Bool too */
bool type_is_signed(TypeTable *tt, TypeId t);
bool type_is_float(TypeTable *tt, TypeId t);     /* real floating */
bool type_is_arith(TypeTable *tt, TypeId t);
bool type_is_scalar(TypeTable *tt, TypeId t);
bool type_is_object(TypeTable *tt, TypeId t);    /* not function */
bool type_is_void(TypeTable *tt, TypeId t);
bool type_is_record(TypeTable *tt, TypeId t);
bool type_is_vm(TypeTable *tt, TypeId t);        /* variably modified */
/* The pointed-to / element / return type, with qualifiers. */
TypeId type_base(TypeTable *tt, TypeId t);
/* sizeof in bytes; 0 with *ok false if incomplete or a VLA. */
uint64_t type_size(TypeTable *tt, TypeId t, bool *ok);
unsigned type_align(TypeTable *tt, TypeId t);        /* __alignof__ */
unsigned type_member_align(TypeTable *tt, TypeId t); /* in a record */
/* Integer rank / width in bits (integers only). */
unsigned type_int_bits(TypeTable *tt, TypeId t);
int type_int_rank(TypeTable *tt, TypeId t);
/* The signed/unsigned integer type of the same rank. */
TypeId type_to_unsigned(TypeTable *tt, TypeId t);

/* C99 6.2.7 compatibility (qualifiers must match). */
bool type_compatible(TypeTable *tt, TypeId a, TypeId b);
bool type_tu_mixed(TypeTable *tt, TypeId a, TypeId b);
/* The composite of two compatible types (6.2.7p3). */
TypeId type_composite(TypeTable *tt, TypeId a, TypeId b);
/* The type of a parameter declared with type t: arrays and functions
 * become pointers, qualifiers dropped (for the function type). */
TypeId type_param_adjust(TypeTable *tt, TypeId t);
/* The promoted type of an argument without a prototype (6.5.2.2p6). */
TypeId type_default_promote(TypeTable *tt, TypeId t);
TypeId type_int_promote(TypeTable *tt, TypeId t);

/* ---- alias sets (alias.c) --------------------------------------------- */

enum { AL_SAME, AL_MAY, AL_DISJOINT };
/* How the alias sets of two object types relate, as gcc's
 * -Wstrict-aliasing asks: AL_SAME (one set, or a character type), AL_MAY
 * (they conflict: one holds the other, or a char member) or AL_DISJOINT. */
int type_alias_rel(TypeTable *tt, TypeId a, TypeId b);
/* The pointer, as written, points to a may_alias type. */
bool type_ptr_may_alias(TypeTable *tt, TypeId p);

/* ---- printing (gcc's spelling) --------------------------------------- */

void type_print(TypeTable *tt, StrBuf *sb, TypeId t);
/* 'T' {aka 'int'}: quoted, with the typedef-stripped spelling if it
 * differs. */
void type_quote(TypeTable *tt, StrBuf *sb, TypeId t);
/* type_quote into a buffer owned by the table, valid until the next call
 * (four rotate, so one message can quote up to four types). */
const char *type_q(TypeTable *tt, TypeId t);
const char *type_q_decl(TypeTable *tt, TypeId t);

/* ---- layout dump (--dump-types) -------------------------------------- */

void type_dump_record(TypeTable *tt, StrBuf *sb, TypeId t);

#endif
