/* ast.h - syntax trees: flat post-order node arrays (docs/PARSER.md).
 *
 * A parsed unit (one external declaration) is an array of Nodes in
 * post-order: a node's children are the subtrees just before it, and each
 * node records the size of its subtree, so the last child of node i ends
 * at i - 1, the one before it at i - 1 - size(last child), and so on down
 * to i - size(i) + 1.  The root is the last node.  Nodes point at tokens by
 * index *relative to the unit* (Node.tok), never at locations, so a unit
 * is position independent.
 *
 * Optional parts are told apart by tag class (declarator, specifier,
 * expression, ...); where position matters and a part is missing (the
 * clauses of `for`), an N_NONE placeholder keeps the shape fixed.  The
 * comment on each tag lists its children. */
#ifndef CEREAL_AST_H
#define CEREAL_AST_H

#include "common.h"
#include "token.h"

#define NODE_LIST(X)                                                        \
    /* units */                                                             \
    X(FUNC_DEF)      /* specs declarator declared scope kr-decl* compound       \
                        scope-end */                                        \
    X(DECL)          /* specs init-decl* (tok: first token) */              \
    X(STATIC_ASSERT) /* expr string? */                                     \
    X(TOP_ASM)       /* string */                                           \
    X(EMPTY)         /* a stray ';' */                                      \
    X(PRAGMA)        /* tok: the pragma */                                  \
    X(ERROR)         /* tok: first token skipped; aux: tokens skipped */    \
    X(NONE)          /* placeholder */                                      \
    /* specifiers */                                                        \
    X(SPECS)         /* spec* */                                            \
    X(STORAGE)       /* tok */                                              \
    X(QUAL)          /* tok */                                              \
    X(FUNCSPEC)      /* tok */                                              \
    X(TYPESPEC)      /* tok */                                              \
    X(TYPEDEF_NAME)  /* tok */                                              \
    X(STRUCT)        /* tag? attr* (open member*)? (tok: struct/union;         \
                        NF_BODY) */                                         \
    X(ENUM)          /* tag? attr* (open enumerator*)? (NF_BODY) */          \
    X(TAG)           /* tok */                                              \
    X(MEMBER_DECL)   /* specs member* */                                    \
    X(MEMBER)        /* declarator? width? attr* (NF_BITFIELD) */           \
    X(ENUMERATOR)    /* attr* expr? (tok: name) */                          \
    X(TYPEOF)        /* expr | type-name */                                 \
    X(ATOMIC_TYPE)   /* type-name */                                        \
    X(ALIGNAS)       /* type-name | expr */                                 \
    X(ATTRIBUTE)     /* attr-item* (tok: __attribute__) */                  \
    X(ATTR_ITEM)     /* expr* (tok: name) */                                \
    /* declarators */                                                       \
    X(NAME)          /* tok */                                              \
    X(PTR)           /* qual/attr* declarator? */                           \
    X(ARRAY)         /* declarator? qual* size? (NF_STATIC, NF_STAR) */     \
    X(FUNC)          /* declarator? scope (param | kr-ident)* scope-end        \
                        (NF_VARIADIC, NF_KR) */                             \
    X(PARAM)         /* specs declarator? */                                \
    X(KR_IDENT)      /* tok */                                              \
    X(INIT_DECL)     /* declarator attr* asm-label? attr* declared            \
                        initializer? */                                     \
    X(ASM_LABEL)     /* string */                                           \
    X(TYPE_NAME)     /* specs declarator? */                                \
    /* initializers */                                                      \
    X(INIT_LIST)     /* (initializer | designated)* */                      \
    X(DESIGNATED)    /* designator+ initializer */                          \
    X(DESIG_FIELD)   /* tok: the field */                                   \
    X(DESIG_INDEX)   /* expr */                                             \
    X(DESIG_RANGE)   /* expr expr */                                        \
    /* statements */                                                        \
    X(COMPOUND)      /* (scope | body) item* scope-end? (tok: '{';             \
                        NF_ERROR: unclosed) */                              \
    X(LABEL)         /* attr* stmt? (tok: name) */                          \
    X(CASE)          /* expr expr? stmt (NF_RANGE) */                       \
    X(DEFAULT)       /* stmt */                                             \
    X(EXPR_STMT)     /* expr? */                                            \
    X(IF)            /* scope expr sub sub? scope-end; sub: scope stmt       \
                        scope-end */                                        \
    X(SWITCH)        /* scope expr sub scope-end */                          \
    X(WHILE)         /* scope expr sub scope-end */                          \
    X(DO)            /* scope sub expr scope-end */                          \
    X(FOR)           /* scope (decl | expr | none) (expr | none)             \
                        (expr | none) sub scope-end */                      \
    X(GOTO)          /* tok: label */                                       \
    X(GOTO_EXPR)     /* expr */                                             \
    X(CONTINUE)                                                             \
    X(BREAK)                                                                \
    X(RETURN)        /* expr? */                                            \
    X(ASM)           /* qual* string asm-section* */                        \
    X(ASM_SECTION)   /* (asm-operand | string | name)* (aux: 1..4) */       \
    X(ASM_OPERAND)   /* name? string expr */                                \
    X(LOCAL_LABEL)   /* name+ */                                            \
    X(ATTR_STMT)     /* attr+ */                                            \
    /* expressions */                                                       \
    X(IDENT)         /* tok */                                              \
    X(NUMBER)        /* tok */                                              \
    X(CHAR)          /* tok */                                              \
    X(STRING)        /* tok: first piece; aux: pieces */                    \
    X(PAREN)         /* expr */                                             \
    X(CALL)          /* expr arg* */                                        \
    X(INDEX)         /* expr expr */                                        \
    X(MEMBER_EXPR)   /* expr (tok: the field; NF_ARROW) */                  \
    X(POSTFIX)       /* expr (tok: ++ or --) */                             \
    X(UNARY)         /* expr (tok: the operator) */                         \
    X(SIZEOF_EXPR)   /* expr */                                             \
    X(SIZEOF_TYPE)   /* type-name */                                        \
    X(ALIGNOF_EXPR)  /* expr */                                             \
    X(ALIGNOF_TYPE)  /* type-name */                                        \
    X(CAST)          /* type-name expr */                                   \
    X(COMPOUND_LIT)  /* type-name init-list */                              \
    X(BINARY)        /* expr expr (tok: the operator; ',' too) */           \
    X(ASSIGN)        /* expr expr (tok: the operator) */                    \
    X(COND)          /* expr expr? expr (NF_OMITTED: a ?: b) */             \
    X(STMT_EXPR)     /* compound */                                         \
    X(VA_ARG)        /* expr type-name */                                   \
    X(OFFSETOF)      /* type-name name (desig-field | desig-index)* */      \
    X(TYPES_COMPAT)  /* type-name type-name */                              \
    X(CONVERTVECTOR) /* expr type-name */                                   \
    X(GENERIC)       /* expr generic-assoc+ */                              \
    X(GENERIC_ASSOC) /* (type-name | none) expr */                          \
    X(ADDR_LABEL)    /* tok: the label */                                   \
    X(HAS_ATTR)      /* (expr | type-name) attr-item */                     \
    /* markers: leaves placing scope events in post-order for the checker */ \
    X(SCOPE)         /* a scope opens (NF_PARAMS: a function body's, with \
                        its parameters) */                                  \
    X(SCOPE_END)     /* the innermost scope closes */                       \
    X(DECLARED)      /* the declarator just before (past attr* and an     \
                        asm label) is in scope from here (NF_BODY: a       \
                        function definition's) */                           \
    X(OPEN)          /* '{' of a struct/union/enum body (aux: 0 struct,   \
                        1 union, 2 enum) */                                 \
    X(BODY)          /* '{' of a function body */

typedef enum {
#define X(n) N_##n,
    NODE_LIST(X)
#undef X
    N_COUNT
} NodeTag;

enum {
    NF_ERROR = 1 << 0,     /* recovered from a syntax error */
    NF_BODY = 1 << 1,      /* STRUCT/ENUM: has a member list */
    NF_BITFIELD = 1 << 2,
    NF_STATIC = 1 << 3,    /* ARRAY: [static n] */
    NF_STAR = 1 << 4,      /* ARRAY: [*] */
    NF_VARIADIC = 1 << 5,  /* FUNC: ... */
    NF_KR = 1 << 6,        /* FUNC: identifier list */
    NF_ARROW = NF_STAR,    /* MEMBER_EXPR: -> */
    NF_OMITTED = NF_STATIC, /* COND: a ?: b */
    NF_RANGE = NF_STATIC,  /* CASE: case a ... b */
    NF_EXTENSION = NF_KR,  /* DECL/FUNC_DEF: after __extension__ */
    NF_NESTED = NF_BODY,   /* DECL+ERROR: gcc took it for a nested function */
    NF_PARAMS = 1 << 7,    /* SCOPE: a function body's, with parameters */
    NF_FWD = 1 << 7,       /* PARAM: GNU forward declaration (before ';') */
    NF_SEMI = NF_BODY      /* PARAM: the one just before that ';' */
};

typedef struct Node {
    uint8_t tag;
    uint8_t flags;
    uint16_t aux;
    uint32_t tok;          /* relative to the unit */
    uint32_t size;         /* nodes in the subtree, this one included */
} Node;

extern const char *const node_names[N_COUNT];

/* A token as the parser keeps it: with its presentation location (the
 * expansion point of a token from a macro). */
typedef struct PTok {
    Tok t;
    SrcLoc exp;
    uint8_t stdattr;    /* a '[' that starts '[[' (C2X attribute) */
} PTok;

/* One external declaration. */
typedef struct ParseUnit {
    const PTok *toks;
    uint32_t ntoks;
    const Node *nodes;
    uint32_t nnodes;       /* the root is nodes[nnodes - 1] */
    uint64_t first_tok;    /* index of toks[0] in the TU's stream */
} ParseUnit;

/* Children of nodes[i], first to last, into out (at most max); returns
 * how many there are. */
uint32_t node_children(const Node *nodes, uint32_t i, uint32_t *out,
                       uint32_t max);

#endif
