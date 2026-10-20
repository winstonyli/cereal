/* mir_elf.c - x86-64 machine code for mir.h and an ELF64 relocatable object.
 *
 * Encodings follow what GNU as picks for the AT&T text mir.c prints, so the
 * .text bytes can be compared with the assembler's (tests/asm_exec.py):
 * register-to-register moves and arithmetic use the "reg is the source"
 * opcodes, immediates take the 8-bit form when they fit (and the short
 * %rax form otherwise), displacements the shortest of 0/8/32 bits, and jumps
 * start short and grow until every one fits. */
#include <string.h>

#include "c/mir.h"

typedef struct Enc {
    uint8_t b[16];
    int n;
    int rel_at;                 /* a %rip-relative symbol's rel32 starts here (-1: none) ... */
    const char *rel_name;       /* ... and names it */
    bool rel_got;               /* through the GOT */
    bool rel_weak;              /* the symbol is weak */
} Enc;

static void put(Enc *e, int v) { e->b[e->n++] = (uint8_t)v; }

static void put32(Enc *e, int64_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        put(e, (int)((uint64_t)v >> (8 * i) & 0xff));
}

/* The condition codes' low nibble in jcc, setcc and cmovcc. */
static const uint8_t x86_cc[CC_COUNT] = {4, 5, 0xc, 0xe, 0xf, 0xd, 2, 6, 7, 3, 0xa, 0xb};

static int hw(int r)
{
    static const uint8_t t[R_GP_COUNT] = {0, 1, 2, 6, 7, 8, 9, 5, 4, 11};
    return r >= R_X0 ? r - R_X0 : t[r];
}

static bool is_xmm(const MOp *o) { return o->k == MK_REG && o->reg >= R_X0; }

/* A byte register that needs a REX prefix to be spelled: spl, bpl, sil, dil. */
static bool rex_byte(int w, const MOp *o)
{
    return w == 1 && o->k == MK_REG && o->reg < R_X0 && hw(o->reg) >= 4 && hw(o->reg) < 8;
}

/* [legacy prefix] [REX] opcode modrm [sib] [disp]: `reg` is the ModRM reg
 * field (a register number or an opcode extension), `rm` a register or
 * memory operand. */
static void ins(Enc *e, int pfx, bool w8, bool force, const uint8_t *opc, int nopc, int reg,
                const MOp *rm)
{
    int rex = (w8 ? 8 : 0) | (reg >> 3 & 1) << 2, i;
    rex |= hw(rm->reg) >> 3 & 1;
    if (pfx)
        put(e, pfx);
    if (rex || force)
        put(e, 0x40 | rex);
    for (i = 0; i < nopc; i++)
        put(e, opc[i]);
    if (rm->k == MK_REG) {
        put(e, 0xc0 | (reg & 7) << 3 | (hw(rm->reg) & 7));
    } else if (rm->k == MK_SYM) {       /* disp32(%rip), filled in by a relocation */
        put(e, (reg & 7) << 3 | 5);
        e->rel_at = e->n;
        e->rel_name = rm->name;
        e->rel_got = rm->reg & 1;
        e->rel_weak = rm->reg & 2;
        put32(e, 0);
    } else {
        int b = hw(rm->reg) & 7, mod;
        int64_t d = rm->v;
        mod = d == 0 && b != 5 ? 0 : d >= -128 && d <= 127 ? 1 : 2;
        put(e, mod << 6 | (reg & 7) << 3 | b);
        if (b == 4)
            put(e, 0x24);
        if (mod == 1)
            put(e, (int)(d & 0xff));
        else if (mod == 2)
            put32(e, d);
    }
}

static void ins1(Enc *e, int pfx, bool w8, bool force, int opc, int reg, const MOp *rm)
{
    uint8_t o = (uint8_t)opc;
    ins(e, pfx, w8, force, &o, 1, reg, rm);
}

static void ins2(Enc *e, int pfx, bool w8, int opc, int reg, const MOp *rm)
{
    uint8_t o[2] = {0x0f, (uint8_t)opc};
    ins(e, pfx, w8, false, o, 2, reg, rm);
}

/* The x86 group-1 operations: opcode extension. */
static int arith_ext(int op)
{
    switch (op) {
    case M_ADD: return 0;
    case M_OR: return 1;
    case M_AND: return 4;
    case M_SUB: return 5;
    case M_XOR: return 6;
    case M_CMP: return 7;
    default: return -1;
    }
}

/* The prefix of a scalar SSE row (mir.h, MIR_FP). */
static int fp_prefix(const MRow *r, const MInst *m)
{
    if (r->pfx == PF_P)
        return m->w == 4 ? 0 : 0x66;
    return m->w == 4 ? 0xf3 : 0xf2;
}

/* Everything but jumps; a call's rel32 is left zero (the relocation fills it). */
static bool enc_inst(const MInst *m, Enc *e)
{
    const MOp *a = &m->a, *b = &m->b;
    bool w8 = m->w == 8 || m->w == 0;
    int w = m->w ? m->w : 8, ext = arith_ext(m->op), pfx = w == 2 ? 0x66 : 0;
    e->n = 0;
    e->rel_at = -1;
    e->rel_got = false;
    e->rel_weak = false;
    if (ext >= 0) {
        if (a->k == MK_IMM) {
            int64_t v = a->v;
            if (w == 1) {
                ins1(e, 0, false, rex_byte(w, b), 0x80, ext, b);
                put(e, (int)(v & 0xff));
            } else if (v >= -128 && v <= 127) {
                ins1(e, pfx, w8, false, 0x83, ext, b);
                put(e, (int)(v & 0xff));
            } else if (b->k == MK_REG && b->reg == R_AX) {
                if (w8)
                    put(e, 0x48);
                put(e, ext << 3 | 5);
                put32(e, v);
            } else {
                ins1(e, pfx, w8, false, 0x81, ext, b);
                put32(e, v);
            }
        } else if (a->k == MK_REG) {
            ins1(e, pfx, w8, rex_byte(w, a) || rex_byte(w, b), ext << 3 | (w == 1 ? 0 : 1),
                 hw(a->reg), b);
        } else {
            ins1(e, pfx, w8, rex_byte(w, b), ext << 3 | (w == 1 ? 2 : 3), hw(b->reg), a);
        }
        return true;
    }
    switch (m->op) {
    case M_MOV:
        if (a->k == MK_IMM) {
            if (w == 4 || w == 1) {
                int r = hw(b->reg);
                if (r >= 8)
                    put(e, 0x41);
                put(e, (w == 1 ? 0xb0 : 0xb8) + (r & 7));
                if (w == 1)
                    put(e, (int)(a->v & 0xff));
                else
                    put32(e, a->v);
            } else {
                ins1(e, 0, true, false, 0xc7, 0, b);
                put32(e, a->v);
            }
        } else if (a->k == MK_REG) {
            ins1(e, pfx, w8, rex_byte(w, a) || rex_byte(w, b), w == 1 ? 0x88 : 0x89, hw(a->reg), b);
        } else {
            ins1(e, pfx, w8, rex_byte(w, b), w == 1 ? 0x8a : 0x8b, hw(b->reg), a);
        }
        break;
    case M_MOVABS: {
        int r = hw(b->reg), i;
        put(e, 0x48 | (r >> 3));
        put(e, 0xb8 + (r & 7));
        for (i = 0; i < 8; i++)
            put(e, (int)((uint64_t)a->v >> (8 * i) & 0xff));
        break;
    }
    case M_MOVSX:
        if (w == 4)
            ins1(e, 0, true, false, 0x63, hw(b->reg), a);
        else
            ins2(e, 0, true, w == 1 ? 0xbe : 0xbf, hw(b->reg), a);
        break;
    case M_MOVZX:
        if (w == 4) {
            if (a->k == MK_REG)
                ins1(e, 0, false, false, 0x89, hw(a->reg), b);
            else
                ins1(e, 0, false, false, 0x8b, hw(b->reg), a);
        } else {
            ins2(e, 0, true, w == 1 ? 0xb6 : 0xb7, hw(b->reg), a);
        }
        break;
    case M_LEA:
        if (a->k == MK_LAB) {           /* %rip-relative to a label: the displacement is patched in */
            put(e, 0x48 | (hw(b->reg) >= 8 ? 4 : 0));
            put(e, 0x8d);
            put(e, (hw(b->reg) & 7) << 3 | 5);
            put32(e, 0);
            break;
        }
        ins1(e, 0, true, false, 0x8d, hw(b->reg), a);
        break;
    case M_IMUL:
        if (a->k == MK_IMM) {
            bool s8 = a->v >= -128 && a->v <= 127;
            ins1(e, 0, true, false, s8 ? 0x6b : 0x69, hw(b->reg), b);
            if (s8)
                put(e, (int)(a->v & 0xff));
            else
                put32(e, a->v);
        } else {
            ins2(e, 0, true, 0xaf, hw(b->reg), a);
        }
        break;
    case M_CMOV: ins2(e, 0, w8, 0x40 + x86_cc[m->cc], hw(b->reg), a); break;
    case M_BSR: ins2(e, 0, w8, 0xbd, hw(b->reg), a); break;
    case M_BSF: ins2(e, 0, w8, 0xbc, hw(b->reg), a); break;
    case M_BSWAP:
        if (w8 || hw(a->reg) >= 8)
            put(e, 0x40 | (w8 ? 8 : 0) | (hw(a->reg) >> 3 & 1));
        put(e, 0x0f);
        put(e, 0xc8 + (hw(a->reg) & 7));
        break;
    case M_SHL: ins1(e, 0, w8, false, 0xd3, 4, b); break;
    case M_SHR: ins1(e, 0, w8, false, 0xd3, 5, b); break;
    case M_SAR: ins1(e, 0, w8, false, 0xd3, 7, b); break;
    case M_NOT: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 2, a); break;
    case M_NEG: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 3, a); break;
    case M_DIV: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 6, a); break;
    case M_IDIV: ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0xf6 : 0xf7, 7, a); break;
    case M_TEST:
        ins1(e, pfx, w8, rex_byte(w, a) || rex_byte(w, b), w == 1 ? 0x84 : 0x85, hw(a->reg), b);
        break;
    case M_SETCC: {
        ins2(e, 0, false, 0x90 + x86_cc[m->cc], 0, a);
        if (rex_byte(1, a)) {           /* insert the REX before the 0f */
            memmove(e->b + 1, e->b, (size_t)e->n);
            e->b[0] = 0x40;
            e->n++;
        }
        break;
    }
    case M_PUSH: case M_POP: {
        int r = hw(a->reg);
        if (r >= 8)
            put(e, 0x41);
        put(e, (m->op == M_PUSH ? 0x50 : 0x58) + (r & 7));
        break;
    }
    case M_BYTES: {
        int q;
        for (q = 0; q < m->w; q++)
            put(e, (int)((m->a.v >> (8 * q)) & 0xff));
        break;
    }
    case M_XCHG:
        ins1(e, pfx, w8, rex_byte(w, a), w == 1 ? 0x86 : 0x87, hw(a->reg), b);
        break;
    case M_CMPXCHG: {
        uint8_t o[2] = {0x0f, (uint8_t)(0xb0 + (w == 1 ? 0 : 1))};
        if (w == 2)
            put(e, 0x66);
        ins(e, 0xf0, w8, rex_byte(w, a), o, 2, hw(a->reg), b);
        break;
    }
    case M_CALL:
        if (a->k == MK_REG) {
            ins1(e, 0, false, false, 0xff, 2, a);
        } else {
            put(e, 0xe8);
            put32(e, 0);
        }
        break;
    case M_JMP:                 /* jmp *%reg (jumps to labels are laid out by mobj_add) */
        ins1(e, 0, false, false, 0xff, 4, a);
        break;
    case M_FMOV:
        if (b->k == MK_MEM)
            ins2(e, m->w == 4 ? 0xf3 : 0xf2, false, 0x11, hw(a->reg), b);
        else
            ins2(e, m->w == 4 ? 0xf3 : 0xf2, false, 0x10, hw(b->reg), a);
        break;
    case M_CVTF2F: ins2(e, m->w == 4 ? 0xf3 : 0xf2, false, 0x5a, hw(b->reg), a); break;
    case M_MOVQX:
        if (is_xmm(b))
            ins2(e, 0x66, w == 8, 0x6e, hw(b->reg), a);
        else
            ins2(e, 0x66, w == 8, 0x7e, hw(a->reg), b);
        break;
    case M_MOVUPS:
        if (b->k == MK_MEM)
            ins2(e, 0, false, 0x11, hw(a->reg), b);
        else
            ins2(e, 0, false, 0x10, hw(b->reg), a);
        break;
    case M_MOVAPS: ins2(e, 0, false, 0x28, hw(b->reg), a); break;
    default: {
        const MRow *r = mir_row(m->op);
        if (!r)
            return false;
        if (r->kind == 'f') {                   /* fixed bytes */
            int q;
            for (q = 0; q < r->n; q++)
                put(e, (int)((r->bytes >> (8 * q)) & 0xff));
        } else if (r->kind == 'm') {            /* 0f <opc> /<ext> m */
            ins2(e, 0, false, r->opc, r->ext, a);
        } else {                                /* scalar SSE */
            ins2(e, fp_prefix(r, m), r->ext != 0, r->opc, hw(b->reg), a);
        }
        break;
    }
    }
    return true;
}

/* ---- the object -------------------------------------------------------- */

enum { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS, NSEC };

static const char *const sec_name[NSEC] = {".text", ".rodata", ".data", ".bss"};
static const uint32_t sec_flags[NSEC] = {6 /* AX */, 2 /* A */, 3 /* WA */, 3};

enum { REL_64 = 1, REL_PC32 = 2, REL_PLT32 = 4, REL_REX_GOTPCRELX = 42 };

typedef struct ObjSym {
    char *name;                 /* NULL: a section symbol */
    int sec;                    /* its section, or -1: undefined */
    uint64_t value, size;
    uint8_t type;               /* 0 none, 1 object, 2 function, 3 section */
    bool global;
    bool weak;                  /* binding STB_WEAK (global is also set) */
    bool anon;                  /* a .L name: no symbol, relocations use the section's */
} ObjSym;

typedef struct ObjRel {
    uint32_t off;
    uint32_t sym;               /* index into MObj.syms */
    uint8_t type;
    int64_t addend;
} ObjRel;

struct MObj {
    VEC(uint8_t) bytes[NSEC];
    VEC(ObjRel) rels[NSEC];
    uint64_t bss;               /* .bss's size: it has no bytes */
    uint32_t salign[NSEC];      /* each section's alignment */
    VEC(ObjSym) syms;           /* [0..NSEC): the section symbols */
    uint32_t *hash;             /* open addressing over named symbols: index + 1 */
    size_t hcap;
};

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static size_t name_hash(const char *s)
{
    size_t h = 1469598103u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void hash_put(MObj *o, uint32_t id)
{
    size_t h = name_hash(o->syms.data[id].name) & (o->hcap - 1);
    while (o->hash[h])
        h = (h + 1) & (o->hcap - 1);
    o->hash[h] = id + 1;
}

/* The symbol called name, made (undefined, global) when new. */
static uint32_t sym_named(MObj *o, const char *name)
{
    size_t h;
    ObjSym s;
    if (o->syms.len * 2 >= o->hcap) {
        size_t i;
        free(o->hash);
        o->hcap = o->hcap ? o->hcap * 2 : 64;
        o->hash = xcalloc(o->hcap, sizeof *o->hash);
        for (i = 0; i < o->syms.len; i++)
            if (o->syms.data[i].name)
                hash_put(o, (uint32_t)i);
    }
    for (h = name_hash(name) & (o->hcap - 1); o->hash[h]; h = (h + 1) & (o->hcap - 1))
        if (!strcmp(o->syms.data[o->hash[h] - 1].name, name))
            return o->hash[h] - 1;
    memset(&s, 0, sizeof s);
    s.name = dup_str(name);
    s.sec = -1;
    s.global = true;
    vec_push(&o->syms, s);
    hash_put(o, (uint32_t)(o->syms.len - 1));
    return (uint32_t)(o->syms.len - 1);
}

MObj *mobj_new(void)
{
    MObj *o = xcalloc(1, sizeof *o);
    int i;
    for (i = 0; i < NSEC; i++) {
        ObjSym s;
        memset(&s, 0, sizeof s);
        s.sec = i;
        s.type = 3;
        vec_push(&o->syms, s);
    }
    return o;
}

static void rel_add(MObj *o, int sec, uint32_t off, uint32_t sym, int type, int64_t addend)
{
    ObjRel r;
    r.off = off;
    r.sym = sym;
    r.type = (uint8_t)type;
    r.addend = addend;
    vec_push(&o->rels[sec], r);
}

/* A jump to a label (not through a register). */
static bool label_jump(const MInst *m)
{
    return (m->op == M_JMP && m->a.k == MK_LAB) || m->op == M_JCC;
}

bool mobj_add(MObj *o, const MFunc *f)
{
    size_t n = f->code.len, i;
    Enc *enc = xcalloc(n ? n : 1, sizeof *enc);
    uint32_t *pos = xcalloc(n + 1, sizeof *pos), *lab;
    bool *lng = xcalloc(n ? n : 1, sizeof *lng), changed, ok = true;
    int64_t maxlab = 0;
    uint32_t base = (uint32_t)o->bytes[SEC_TEXT].len, fid;
    for (i = 0; i < n; i++) {
        const MInst *m = &f->code.data[i];
        if (m->op == M_LABEL && m->a.v > maxlab)
            maxlab = m->a.v;
        if (m->op != M_LABEL && !label_jump(m) && !enc_inst(m, &enc[i]))
            ok = false;
    }
    lab = xcalloc((size_t)maxlab + 1, sizeof *lab);
    do {
        uint32_t p = 0;
        changed = false;
        for (i = 0; i < n; i++) {
            const MInst *m = &f->code.data[i];
            pos[i] = p;
            if (m->op == M_LABEL)
                lab[m->a.v] = p;
            else if (label_jump(m))
                p += m->op == M_JMP ? (lng[i] ? 5 : 2) : (lng[i] ? 6 : 2);
            else
                p += (uint32_t)enc[i].n;
        }
        pos[n] = p;
        for (i = 0; i < n; i++) {
            const MInst *m = &f->code.data[i];
            int64_t d;
            if (!label_jump(m) || lng[i])
                continue;
            d = (int64_t)lab[m->a.v] - (int64_t)(pos[i] + 2);
            if (d < -128 || d > 127) {
                lng[i] = true;
                changed = true;
            }
        }
    } while (changed);
    if (!ok) {
        free(enc);
        free(pos);
        free(lab);
        free(lng);
        return false;
    }
    fid = sym_named(o, f->name);
    o->syms.data[fid].sec = SEC_TEXT;
    o->syms.data[fid].value = base;
    o->syms.data[fid].size = pos[n];
    o->syms.data[fid].type = 2;
    o->syms.data[fid].global = f->global;
    o->syms.data[fid].weak = f->weak;
    for (i = 0; i < f->pubs.len; i++) {         /* labels data points at: section-relative */
        uint32_t id = sym_named(o, f->pubs.data[i].name);
        o->syms.data[id].sec = SEC_TEXT;
        o->syms.data[id].value = base + lab[f->pubs.data[i].lab];
        o->syms.data[id].global = false;
        o->syms.data[id].anon = true;
    }
    for (i = 0; i < n; i++) {
        const MInst *m = &f->code.data[i];
        int k;
        if (m->op == M_LABEL)
            continue;
        if (label_jump(m)) {
            int64_t d = (int64_t)lab[m->a.v] - (int64_t)(pos[i] + (lng[i] ? (m->op == M_JMP ? 5 : 6) : 2));
            Enc e;
            e.n = 0;
            if (!lng[i]) {
                put(&e, m->op == M_JMP ? 0xeb : 0x70 + x86_cc[m->cc]);
                put(&e, (int)(d & 0xff));
            } else if (m->op == M_JMP) {
                put(&e, 0xe9);
                put32(&e, d);
            } else {
                put(&e, 0x0f);
                put(&e, 0x80 + x86_cc[m->cc]);
                put32(&e, d);
            }
            for (k = 0; k < e.n; k++)
                vec_push(&o->bytes[SEC_TEXT], e.b[k]);
            continue;
        }
        if (m->op == M_LEA && m->a.k == MK_LAB) {
            int64_t d = (int64_t)lab[m->a.v] - (int64_t)(pos[i] + 7);
            for (k = 0; k < 4; k++)
                enc[i].b[3 + k] = (uint8_t)(d >> (8 * k));
        }
        if (m->op == M_CALL && m->a.k != MK_REG) {
            uint32_t y = sym_named(o, m->a.name);
            o->syms.data[y].weak |= (m->a.reg & 2) != 0;
            rel_add(o, SEC_TEXT, base + pos[i] + 1, y, REL_PLT32, -4);
        }
        if (enc[i].rel_at >= 0) {       /* a symbol's address, %rip-relative */
            uint32_t y = sym_named(o, enc[i].rel_name);
            o->syms.data[y].weak |= enc[i].rel_weak;
            rel_add(o, SEC_TEXT, base + pos[i] + (uint32_t)enc[i].rel_at, y,
                    enc[i].rel_got ? REL_REX_GOTPCRELX : REL_PC32, -(enc[i].n - enc[i].rel_at));
        }
        for (k = 0; k < enc[i].n; k++)
            vec_push(&o->bytes[SEC_TEXT], enc[i].b[k]);
    }
    free(enc);
    free(pos);
    free(lab);
    free(lng);
    return true;
}

void mobj_add_data(MObj *o, const MData *d)
{
    static const int sec_of[] = {SEC_RODATA, SEC_DATA, SEC_BSS};
    int s = sec_of[d->sec];
    uint32_t id;
    uint64_t at = s == SEC_BSS ? o->bss : o->bytes[s].len;
    size_t i;
    at = (at + d->align - 1) / d->align * d->align;
    if (d->align > o->salign[s])
        o->salign[s] = d->align;
    if (s == SEC_BSS) {
        o->bss = at + d->size;
    } else {
        while (o->bytes[s].len < at)
            vec_push(&o->bytes[s], 0);
        for (i = 0; i < d->size; i++)
            vec_push(&o->bytes[s], d->bytes[i]);
    }
    id = sym_named(o, d->name);
    o->syms.data[id].sec = s;
    o->syms.data[id].value = at;
    o->syms.data[id].size = d->size;
    o->syms.data[id].type = 1;
    o->syms.data[id].global = d->global;
    o->syms.data[id].weak = d->weak;
    o->syms.data[id].anon = !strncmp(d->name, ".L", 2);
    for (i = 0; i < d->rels.len; i++)
        rel_add(o, s, (uint32_t)at + d->rels.data[i].off, sym_named(o, d->rels.data[i].name),
                REL_64, d->rels.data[i].addend);
}

/* ---- ELF64 -------------------------------------------------------------- */

typedef VEC(uint8_t) Bytes;

static void w8(Bytes *b, uint64_t v) { vec_push(b, (uint8_t)v); }
static void w16(Bytes *b, uint64_t v) { w8(b, v); w8(b, v >> 8); }
static void w32(Bytes *b, uint64_t v) { w16(b, v); w16(b, v >> 16); }
static void w64(Bytes *b, uint64_t v) { w32(b, v); w32(b, v >> 32); }

static uint32_t wstr(Bytes *b, const char *s)
{
    uint32_t at = (uint32_t)b->len;
    do
        w8(b, (uint8_t)*s);
    while (*s++);
    return at;
}

static void align8(Bytes *b)
{
    while (b->len & 7)
        w8(b, 0);
}

typedef struct Shdr {
    uint32_t name, type, link, info;
    uint64_t flags, off, size, align, ent;
} Shdr;

/* Like the assembler, resolve a call to a local (static) function defined in
 * the object: its displacement is final, so it needs no relocation. */
static void resolve_local_calls(MObj *o)
{
    size_t n = o->rels[SEC_TEXT].len;
    size_t k, keep = 0;
    for (k = 0; k < n; k++) {
        ObjRel r = o->rels[SEC_TEXT].data[k];
        const ObjSym *y = &o->syms.data[r.sym];
        if (r.type == REL_PLT32 && y->sec == SEC_TEXT && !y->global && !y->anon) {
            int64_t d = (int64_t)y->value + r.addend - (int64_t)r.off;
            int j;
            for (j = 0; j < 4; j++)
                o->bytes[SEC_TEXT].data[r.off + j] = (uint8_t)(d >> (8 * j));
            continue;
        }
        o->rels[SEC_TEXT].data[keep++] = r;
    }
    o->rels[SEC_TEXT].len = keep;
}

bool mobj_write(MObj *o, const char *path)
{
    Bytes img = {0}, strtab = {0}, symtab = {0}, shstr = {0}, h = {0};
    VEC(Shdr) sh = {0};
    uint32_t *final, nsyms = 0, nlocal, relsec[NSEC] = {0}, symtab_ix, i;
    size_t k;
    int s;
    bool ok;
    FILE *f;
    static const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1};

    resolve_local_calls(o);

    /* symbol order: null, locals (section symbols first), then globals */
    final = xcalloc(o->syms.len, sizeof *final);
    nsyms = 1;
    for (k = 0; k < o->syms.len; k++)
        if (!o->syms.data[k].global && !o->syms.data[k].anon)
            final[k] = nsyms++;
    nlocal = nsyms;
    for (k = 0; k < o->syms.len; k++)
        if (o->syms.data[k].global)
            final[k] = nsyms++;
    w8(&strtab, 0);
    w8(&shstr, 0);
    for (i = 0; i < 24; i++)
        w8(&symtab, 0);
    for (int pass = 0; pass < 2; pass++)
        for (k = 0; k < o->syms.len; k++) {
            const ObjSym *y = &o->syms.data[k];
            if (y->global != (pass == 1) || y->anon)
                continue;
            w32(&symtab, y->name ? wstr(&strtab, y->name) : 0);
            w8(&symtab, (y->weak ? 0x20 : y->global ? 0x10 : 0) | y->type);
            w8(&symtab, 0);
            w16(&symtab, y->sec < 0 ? 0 : (uint64_t)y->sec + 1);   /* sections 1.. in order */
            w64(&symtab, y->value);
            w64(&symtab, y->size);
        }

    for (i = 0; i < 64; i++)
        w8(&img, 0);
    {
        Shdr z;
        memset(&z, 0, sizeof z);
        vec_push(&sh, z);
    }
    for (s = 0; s < NSEC; s++) {
        Shdr x;
        memset(&x, 0, sizeof x);
        x.name = wstr(&shstr, sec_name[s]);
        x.type = s == SEC_BSS ? 8 : 1;
        x.flags = sec_flags[s];
        x.off = img.len;
        x.size = s == SEC_BSS ? o->bss : o->bytes[s].len;
        x.align = o->salign[s] ? o->salign[s] : 1;
        for (k = 0; k < o->bytes[s].len; k++)
            w8(&img, o->bytes[s].data[k]);
        align8(&img);
        vec_push(&sh, x);
    }
    symtab_ix = 1 + NSEC;               /* the relocation sections come first */
    for (s = 0; s < NSEC; s++) {
        Shdr x;
        char nm[32];
        if (!o->rels[s].len)
            continue;
        memset(&x, 0, sizeof x);
        snprintf(nm, sizeof nm, ".rela%s", sec_name[s]);
        x.name = wstr(&shstr, nm);
        x.type = 4;
        x.flags = 0x40;
        x.off = img.len;
        x.size = o->rels[s].len * 24;
        x.align = 8;
        x.ent = 24;
        x.info = (uint32_t)s + 1;
        for (k = 0; k < o->rels[s].len; k++) {
            const ObjRel *r = &o->rels[s].data[k];
            const ObjSym *y = &o->syms.data[r->sym];
            w64(&img, r->off);
            if (y->anon) {              /* the section's symbol, shifted to the object */
                w64(&img, (uint64_t)final[y->sec] << 32 | r->type);
                w64(&img, (uint64_t)(r->addend + (int64_t)y->value));
            } else {
                w64(&img, (uint64_t)final[r->sym] << 32 | r->type);
                w64(&img, (uint64_t)r->addend);
            }
        }
        relsec[s] = (uint32_t)sh.len;
        vec_push(&sh, x);
        symtab_ix++;
    }
    {
        Shdr x;
        memset(&x, 0, sizeof x);
        x.name = wstr(&shstr, ".symtab");
        x.type = 2;
        x.off = img.len;
        x.size = symtab.len;
        x.link = symtab_ix + 1;         /* .strtab follows */
        x.info = nlocal;
        x.align = 8;
        x.ent = 24;
        for (k = 0; k < symtab.len; k++)
            w8(&img, symtab.data[k]);
        vec_push(&sh, x);
        memset(&x, 0, sizeof x);
        x.name = wstr(&shstr, ".strtab");
        x.type = 3;
        x.off = img.len;
        x.size = strtab.len;
        x.align = 1;
        for (k = 0; k < strtab.len; k++)
            w8(&img, strtab.data[k]);
        vec_push(&sh, x);
        memset(&x, 0, sizeof x);
        x.name = wstr(&shstr, ".note.GNU-stack");
        x.type = 1;
        x.off = img.len;
        x.align = 1;
        vec_push(&sh, x);
        memset(&x, 0, sizeof x);
        x.name = wstr(&shstr, ".shstrtab");
        x.type = 3;
        x.off = img.len;
        x.size = shstr.len;
        x.align = 1;
        for (k = 0; k < shstr.len; k++)
            w8(&img, shstr.data[k]);
        vec_push(&sh, x);
    }
    for (s = 0; s < NSEC; s++)
        if (relsec[s])
            sh.data[relsec[s]].link = symtab_ix;
    align8(&img);
    {
        uint64_t off_sh = img.len;
        for (k = 0; k < sh.len; k++) {
            const Shdr *x = &sh.data[k];
            w32(&img, x->name);
            w32(&img, x->type);
            w64(&img, x->flags);
            w64(&img, 0);
            w64(&img, x->off);
            w64(&img, x->size);
            w32(&img, x->link);
            w32(&img, x->info);
            w64(&img, x->align);
            w64(&img, x->ent);
        }
        for (i = 0; i < 16; i++)
            w8(&h, ident[i]);
        w16(&h, 1);                     /* ET_REL */
        w16(&h, 62);                    /* EM_X86_64 */
        w32(&h, 1);
        w64(&h, 0);
        w64(&h, 0);
        w64(&h, off_sh);
        w32(&h, 0);
        w16(&h, 64);
        w16(&h, 0);
        w16(&h, 0);
        w16(&h, 64);
        w16(&h, (uint64_t)sh.len);
        w16(&h, (uint64_t)sh.len - 1);  /* .shstrtab is last */
        memcpy(img.data, h.data, h.len);
    }
    f = fopen(path, "wb");
    ok = f && fwrite(img.data, 1, img.len, f) == img.len;
    if (f && fclose(f))
        ok = false;
    vec_free(&img);
    vec_free(&strtab);
    vec_free(&symtab);
    vec_free(&shstr);
    vec_free(&h);
    vec_free(&sh);
    free(final);
    return ok;
}

void mobj_free(MObj *o)
{
    size_t i;
    int s;
    if (!o)
        return;
    for (i = 0; i < o->syms.len; i++)
        free(o->syms.data[i].name);
    for (s = 0; s < NSEC; s++) {
        vec_free(&o->bytes[s]);
        vec_free(&o->rels[s]);
    }
    vec_free(&o->syms);
    free(o->hash);
    free(o);
}
