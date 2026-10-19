/* mir.h - machine instructions (x86-64) between the code generator and its
 * output (docs/TAST_T9_PLAN.md, back end).
 *
 * The generator (cgx86.c) fills a function's instruction list; a printer
 * (mir.c) writes it as AT&T assembly.  Another output (an ELF object writer)
 * reads the same list, so the generator never knows its format. */
#ifndef CEREAL_MIR_H
#define CEREAL_MIR_H

#include <stdint.h>
#include <stdio.h>

#include "common.h"

/* General registers, then %xmm0..%xmm7 (R_X0 + n); the width of an
 * instruction does not apply to them. */
enum { R_AX, R_CX, R_DX, R_SI, R_DI, R_R8, R_R9, R_BP, R_SP, R_R11, R_GP_COUNT,
       R_X0 = R_GP_COUNT, R_COUNT = R_X0 + 8 };

/* Operand kinds. */
enum { MK_NONE, MK_REG, MK_IMM, MK_MEM /* disp(reg) */, MK_LAB /* local label */,
       MK_SYM /* a symbol's name, by string: a call target, or name(%rip) */ };

typedef struct MOp {
    uint8_t k;
    uint8_t reg;            /* MK_REG; MK_MEM: the base register */
    int64_t v;              /* MK_IMM: value; MK_MEM: displacement; MK_LAB: number */
    const char *name;       /* MK_SYM: the symbol (owned by the caller) */
} MOp;

#define MIR_OPS(X)                                                              \
    X(MOV, "mov") X(MOVABS, "movabs") X(MOVSX, "movs") X(MOVZX, "movz")         \
    X(LEA, "lea") X(ADD, "add") X(SUB, "sub") X(IMUL, "imul") X(AND, "and")     \
    X(OR, "or") X(XOR, "xor") X(SHL, "shl") X(SAR, "sar") X(SHR, "shr")         \
    X(NEG, "neg") X(NOT, "not") X(CMP, "cmp") X(TEST, "test") X(SETCC, "set")   \
    X(CQO, "cqto") X(IDIV, "idiv") X(DIV, "div") X(PUSH, "push") X(POP, "pop")  \
    X(CALL, "call") X(JMP, "jmp") X(JCC, "j") X(LEAVE, "leave") X(RET, "ret")   \
    X(LABEL, "") X(XCHG, "xchg") X(XADD, "lock xadd")                                                                \
    /* scalar SSE: w is the float's width (4: ss, 8: sd) */                     \
    X(FMOV, "mov") X(FADD, "add") X(FSUB, "sub") X(FMUL, "mul")                 \
    X(FDIV, "div") X(FCMP, "ucomi")                                             \
    X(CVTI2F, "cvtsi2")    /* 64-bit integer a to float b */                    \
    X(CVTF2I, "cvtt")      /* float a (truncated) to 64-bit integer b */        \
    X(CVTF2F, "cvt")       /* w: the source's width; the other one results */   \
    X(MOVQX, "mov")        /* bits between a general register and an xmm */     \
    X(MOVAPS, "movaps")

typedef enum {
#define X(n, s) M_##n,
    MIR_OPS(X)
#undef X
    M_COUNT
} MOpcode;

/* Condition codes. */
enum { CC_E, CC_NE, CC_L, CC_LE, CC_G, CC_GE, CC_B, CC_BE, CC_A, CC_AE, CC_P, CC_NP,
       CC_COUNT };

typedef struct MInst {
    uint8_t op;
    uint8_t w;              /* operand width in bytes (1, 2, 4, 8); MOVSX/MOVZX: the source's */
    uint8_t cc;             /* SETCC, JCC */
    MOp a, b;               /* AT&T order: a is the source, b the destination */
} MInst;

typedef struct MFunc {
    const char *name;
    bool global;
    VEC(MInst) code;
} MFunc;

/* A data object: its bytes (.bss: only a size) and the 8-byte absolute
 * addresses inside them (the bytes there are zero).  A name starting ".L"
 * is local to the file and leaves no symbol (string literals). */
enum { MD_RODATA, MD_DATA, MD_BSS };

typedef struct MReloc {
    uint32_t off;
    char *name;             /* the target symbol (owned) */
    int64_t addend;
} MReloc;

typedef struct MData {
    char *name;             /* owned */
    bool global;
    int sec;                /* MD_* */
    uint32_t align;
    uint64_t size;
    uint8_t *bytes;         /* size of them (owned); NULL in .bss */
    VEC(MReloc) rels;
} MData;

void mdata_free(MData *d);

/* Writes the function as AT&T assembly (one instruction per line, local
 * labels .L<name>_<n>). */
void mir_print(const MFunc *f, FILE *out);
void mir_print_data(const MData *d, FILE *out);

/* An ELF64 relocatable object being built (mir_elf.c): functions are
 * encoded as they come, mobj_write lays out the file. */
typedef struct MObj MObj;
MObj *mobj_new(void);
bool mobj_add(MObj *o, const MFunc *f);     /* false: an instruction it cannot encode */
void mobj_add_data(MObj *o, const MData *d);
bool mobj_write(MObj *o, const char *path);
void mobj_free(MObj *o);

#endif
