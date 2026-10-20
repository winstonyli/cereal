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
       MK_SYM /* a symbol's name, by string: a call target, or name(%rip); reg bit 1: its
               * address through the GOT, name@GOTPCREL(%rip) (a mov source); bit 2: the
               * symbol is weak (an undefined weak reference resolves to 0) */ };

typedef struct MOp {
    uint8_t k;
    uint8_t reg;            /* MK_REG; MK_MEM: the base register */
    int64_t v;              /* MK_IMM: value; MK_MEM: displacement; MK_LAB: number */
    const char *name;       /* MK_SYM: the symbol (owned by the caller) */
} MOp;

/* Every operation is a row of one of four tables; the printer (mir.c) and the ELF encoder
 * (mir_elf.c) read the same row.  MIR_OPS: operations with their own printing and encoding. */
#define MIR_OPS(X)                                                                  X(MOV, "mov") X(MOVABS, "movabs") X(MOVSX, "movs") X(MOVZX, "movz")             X(LEA, "lea") X(ADD, "add") X(SUB, "sub") X(IMUL, "imul") X(AND, "and")         X(OR, "or") X(XOR, "xor") X(SHL, "shl") X(SAR, "sar") X(SHR, "shr")             X(NEG, "neg") X(NOT, "not") X(CMP, "cmp") X(TEST, "test") X(SETCC, "set")       X(IDIV, "idiv") X(DIV, "div") X(PUSH, "push") X(POP, "pop")                     X(CALL, "call") X(JMP, "jmp") X(JCC, "j")                                       X(LABEL, "") X(XCHG, "xchg") X(XADD, "lock xadd") X(CMPXCHG, "lock cmpxchg")             X(CMOV, "cmov") X(BSWAP, "bswap") X(BSR, "bsr") X(BSF, "bsf")      X(BYTES, "")  /* raw machine bytes: a.name the text, w the count, a.v the bytes */        X(FMOV, "mov")         /* scalar SSE: w is the float's width (4: ss, 8: sd) */     X(CVTF2F, "cvt")       /* w: the source's width; the other one results */       X(MOVQX, "mov")        /* bits between a general register and an xmm */         X(MOVAPS, "movaps")                                                             X(MOVUPS, "movups")    /* 16 bytes between an xmm and an xmm or memory (a to b) */

/* MIR_FIXED: no operands, a fixed byte sequence (x, little-endian): X(name, text, count, bytes). */
#define MIR_FIXED(X)                                                                X(CQO, "cqto", 2, 0x9948) X(LEAVE, "leave", 1, 0xc9) X(RET, "ret", 1, 0xc3)     X(REPMOVSB, "rep movsb", 2, 0xa4f3) X(REPSTOSB, "rep stosb", 2, 0xaaf3)         X(NOP, "nop", 1, 0x90) X(PAUSE, "pause", 2, 0x90f3) X(UD2, "ud2", 2, 0x0b0f)     X(SFENCE, "sfence", 3, 0xf8ae0f) X(LFENCE, "lfence", 3, 0xe8ae0f)               X(MFENCE, "mfence", 3, 0xf0ae0f)

/* MIR_MEM: one memory operand a, encoded 0f <opc> /<ext>: X(name, text, opc, ext). */
#define MIR_MEM(X)                                                                  X(STMXCSR, "stmxcsr", 0xae, 3) X(LDMXCSR, "ldmxcsr", 0xae, 2) X(CLFLUSH, "clflush", 0xae, 7)

/* MIR_FP: scalar SSE, xmm-or-memory a to xmm or general register b, encoded <prefix> 0f <opc>:
 * X(name, text, prefix, opc, 64-bit general operand).  The text takes "ss" or "sd" (by w); the
 * prefix is PF_W (f3 for w 4, f2 for w 8) or PF_P (none for w 4, 66 for w 8: ucomiss/ucomisd).
 * rcp and rsqrt exist as ss only (w is 4). */
enum { PF_W, PF_P };
#define MIR_FP(X)                                                                   X(FADD, "add%s", PF_W, 0x58, 0) X(FSUB, "sub%s", PF_W, 0x5c, 0)                 X(FMUL, "mul%s", PF_W, 0x59, 0) X(FDIV, "div%s", PF_W, 0x5e, 0)                 X(FCMP, "ucomi%s", PF_P, 0x2e, 0) X(FSQRT, "sqrt%s", PF_W, 0x51, 0) X(FRCP, "rcp%s", PF_W, 0x53, 0)               X(FRSQRT, "rsqrt%s", PF_W, 0x52, 0)                                             X(CVTI2F, "cvtsi2%sq", PF_W, 0x2a, 1)  /* 64-bit integer a to float b */        X(CVTF2I, "cvtt%s2siq", PF_W, 0x2c, 1) /* float a (truncated) to 64-bit integer b */     X(CVTF2IR, "cvt%s2siq", PF_W, 0x2d, 1) /* float a (rounded as MXCSR says) to 64-bit integer b */

typedef enum {
#define X(n, s) M_##n,
    MIR_OPS(X)
#undef X
#define X(n, s, c, b) M_##n,
    MIR_FIXED(X)
#undef X
#define X(n, s, o, e) M_##n,
    MIR_MEM(X)
#undef X
#define X(n, s, p, o, r) M_##n,
    MIR_FP(X)
#undef X
    M_COUNT
} MOpcode;

/* The row of a MIR_FIXED, MIR_MEM or MIR_FP operation (NULL members: not of that table). */
typedef struct MRow {
    const char *text;       /* all three: the text (MIR_FP: a format for "ss"/"sd") */
    uint8_t n;              /* MIR_FIXED: the byte count; 0 otherwise */
    int64_t bytes;          /* MIR_FIXED: the bytes */
    uint8_t kind;           /* 0 none, 'f' fixed, 'm' MIR_MEM, 'p' MIR_FP */
    uint8_t pfx, opc, ext;  /* MIR_FP: prefix kind, opcode, 64-bit; MIR_MEM: opcode, extension */
} MRow;
const MRow *mir_row(int op);

/* Condition codes. */
enum { CC_E, CC_NE, CC_L, CC_LE, CC_G, CC_GE, CC_B, CC_BE, CC_A, CC_AE, CC_P, CC_NP,
       CC_COUNT };

typedef struct MInst {
    uint8_t op;
    uint8_t w;              /* operand width in bytes (1, 2, 4, 8); MOVSX/MOVZX: the source's */
    uint8_t cc;             /* SETCC, JCC */
    MOp a, b;               /* AT&T order: a is the source, b the destination */
} MInst;

typedef struct MPub {       /* a name for a label, for data that holds its address */
    char *name;
    int64_t lab;
} MPub;

typedef struct MFunc {
    const char *name;
    bool global;
    bool weak;              /* defined weak (global is also set) */
    VEC(MInst) code;
    VEC(MPub) pubs;
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
    bool weak;
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
