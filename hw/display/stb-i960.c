/*
 * Intel i960 (Jx core) decoder and disassembler.
 *
 * See stb-i960.h for why this is a plain C module rather than a QEMU target.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "stb-i960.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Opcode tables                                                      */
/*                                                                    */
/* Only what this firmware actually contains is filled in; a NULL slot */
/* makes the disassembler print the raw word and the harness count it, */
/* which is how the table gets grown - by evidence, not by typing the  */
/* whole architecture manual in and hoping.                            */
/* ------------------------------------------------------------------ */

/* CTRL: opcode 0x08..0x1f, index = op - 0x08 */
static const char *const ctrl_tab[0x18] = {
    /* 08 */ "b",       "call",    "ret",     "bal",
    /* 0c */ NULL,      NULL,      NULL,      NULL,
    /* 10 */ "bno",     "bg",      "be",      "bge",
    /* 14 */ "bl",      "bne",     "ble",     "bo",
    /* 18 */ "faultno", "faultg",  "faulte",  "faultge",
    /* 1c */ "faultl",  "faultne", "faultle", "faulto",
};

/* COBR: opcode 0x20..0x3f, index = op - 0x20 */
static const char *const cobr_tab[0x20] = {
    /* 20 */ "testno",  "testg",   "teste",   "testge",
    /* 24 */ "testl",   "testne",  "testle",  "testo",
    /* 28 */ NULL,      NULL,      NULL,      NULL,
    /* 2c */ NULL,      NULL,      NULL,      NULL,
    /* 30 */ "bbc",     "cmpobg",  "cmpobe",  "cmpobge",
    /* 34 */ "cmpobl",  "cmpobne", "cmpoble", "bbs",
    /* 38 */ "cmpibno", "cmpibg",  "cmpibe",  "cmpibge",
    /* 3c */ "cmpibl",  "cmpibne", "cmpible", "cmpibo",
};

/* MEM: opcode 0x80..0xcf, index = op - 0x80 */
static const char *const mem_tab[0x50] = {
    /* 80 */ "ldob", NULL, "stob", NULL, "bx",   "balx", "callx", NULL,
    /* 88 */ "ldos", NULL, "stos", NULL, "lda",  NULL,   NULL,    NULL,
    /* 90 */ "ld",   NULL, "st",   NULL, NULL,   NULL,   NULL,    NULL,
    /* 98 */ "ldl",  NULL, "stl",  NULL, NULL,   NULL,   NULL,    NULL,
    /* a0 */ "ldt",  NULL, "stt",  NULL, NULL,   NULL,   NULL,    NULL,
    /* a8 */ NULL,   NULL, NULL,   NULL, NULL,   NULL,   NULL,    NULL,
    /* b0 */ "ldq",  NULL, "stq",  NULL, NULL,   NULL,   NULL,    NULL,
    /* b8 */ NULL,   NULL, NULL,   NULL, NULL,   NULL,   NULL,    NULL,
    /* c0 */ "ldib", NULL, "stib", NULL, NULL,   NULL,   NULL,    NULL,
    /* c8 */ "ldis", NULL, "stis", NULL, NULL,   NULL,   NULL,    NULL,
};

/* REG: 12-bit opcode.  Sparse, so a sorted table + binary search. */
typedef struct { uint16_t op; const char *name; uint8_t nsrc; } RegOp;

/*
 * nsrc says how many source operands print:
 *   3 = src1, src2, srcdst   (the usual three-address form)
 *   2 = src1, srcdst         (mov, not, ...)
 *   1 = src1                 (bx-like, modpc, ...)
 *   0 = no operands          (ret, flushreg, ...)
 *   4 = src1, src2           (the compares: they set AC.cc, not a register)
 */
#define NSRC_CMP 4
static const RegOp reg_tab[] = {
    { 0x580, "notbit",   3 }, { 0x581, "and",      3 },
    { 0x582, "andnot",   3 }, { 0x583, "setbit",   3 },
    { 0x584, "notand",   3 }, { 0x586, "xor",      3 },
    { 0x587, "or",       3 }, { 0x588, "nor",      3 },
    { 0x589, "xnor",     3 }, { 0x58a, "not",      2 },
    { 0x58b, "ornot",    3 }, { 0x58c, "clrbit",   3 },
    { 0x58d, "notor",    3 }, { 0x58e, "nand",     3 },
    { 0x58f, "alterbit", 3 },
    { 0x590, "addo",     3 }, { 0x591, "addi",     3 },
    { 0x592, "subo",     3 }, { 0x593, "subi",     3 },
    { 0x594, "cmpob",    NSRC_CMP }, { 0x595, "cmpib",    NSRC_CMP },
    { 0x596, "cmpos",    NSRC_CMP }, { 0x597, "cmpis",    NSRC_CMP },
    { 0x598, "shro",     3 }, { 0x59a, "shrdi",    3 },
    { 0x59b, "shri",     3 }, { 0x59c, "shlo",     3 },
    { 0x59d, "rotate",   3 }, { 0x59e, "shli",     3 },
    { 0x5a0, "cmpo",     NSRC_CMP }, { 0x5a1, "cmpi",     NSRC_CMP },
    { 0x5a2, "concmpo",  NSRC_CMP }, { 0x5a3, "concmpi",  NSRC_CMP },
    { 0x5a4, "cmpinco",  3 }, { 0x5a5, "cmpinci",  3 },
    { 0x5a6, "cmpdeco",  3 }, { 0x5a7, "cmpdeci",  3 },
    { 0x5ac, "scanbyte", NSRC_CMP }, { 0x5ad, "bswap",    2 },
    { 0x5ae, "chkbit",   NSRC_CMP },
    { 0x5b0, "addc",     3 }, { 0x5b2, "subc",     3 },
    { 0x5b4, "intdis",   0 }, { 0x5b5, "inten",    0 },
    { 0x5cc, "mov",      2 },
    { 0x5d8, "eshro",    3 }, { 0x5dc, "movl",     2 },
    { 0x5ec, "movt",     2 }, { 0x5fc, "movq",     2 },
    { 0x610, "atmod",    3 }, { 0x612, "atadd",    3 },
    { 0x615, "synld",    2 },
    { 0x640, "spanbit",  2 }, { 0x641, "scanbit",  2 },
    { 0x645, "modac",    3 },
    { 0x650, "modify",   3 }, { 0x651, "extract",  3 },
    { 0x654, "modtc",    3 }, { 0x655, "modpc",    3 },
    { 0x658, "intctl",   2 }, { 0x659, "sysctl",   3 },
    { 0x65b, "icctl",    3 }, { 0x65c, "dcctl",    3 },
    { 0x65d, "halt",     1 },
    { 0x660, "calls",    1 }, { 0x66b, "mark",     0 },
    { 0x66c, "fmark",    0 }, { 0x66d, "flushreg", 0 },
    { 0x66f, "syncf",    0 },
    { 0x670, "emul",     3 }, { 0x671, "ediv",     3 },
    { 0x701, "mulo",     3 }, { 0x708, "remo",     3 },
    { 0x70b, "divo",     3 },
    { 0x741, "muli",     3 }, { 0x748, "remi",     3 },
    { 0x749, "modi",     3 }, { 0x74b, "divi",     3 },
    /*
     * Jx conditional add/subtract/select.  These do not exist on the
     * KA/KB/CA cores, so their presence here is independent confirmation
     * that this firmware targets a Jx - the IQ80960RP of section 13.
     */
    { 0x780, "addono",     3 }, { 0x781, "addino",     3 }, { 0x782, "subono",     3 },
    { 0x783, "subino",     3 }, { 0x784, "selno",      3 },
    { 0x790, "addog",      3 }, { 0x791, "addig",      3 }, { 0x792, "subog",      3 },
    { 0x793, "subig",      3 }, { 0x794, "selg",       3 },
    { 0x7a0, "addoe",      3 }, { 0x7a1, "addie",      3 }, { 0x7a2, "suboe",      3 },
    { 0x7a3, "subie",      3 }, { 0x7a4, "sele",       3 },
    { 0x7b0, "addoge",     3 }, { 0x7b1, "addige",     3 }, { 0x7b2, "suboge",     3 },
    { 0x7b3, "subige",     3 }, { 0x7b4, "selge",      3 },
    { 0x7c0, "addol",      3 }, { 0x7c1, "addil",      3 }, { 0x7c2, "subol",      3 },
    { 0x7c3, "subil",      3 }, { 0x7c4, "sell",       3 },
    { 0x7d0, "addone",     3 }, { 0x7d1, "addine",     3 }, { 0x7d2, "subone",     3 },
    { 0x7d3, "subine",     3 }, { 0x7d4, "selne",      3 },
    { 0x7e0, "addole",     3 }, { 0x7e1, "addile",     3 }, { 0x7e2, "subole",     3 },
    { 0x7e3, "subile",     3 }, { 0x7e4, "selle",      3 },
    { 0x7f0, "addoo",      3 }, { 0x7f1, "addio",      3 }, { 0x7f2, "suboo",      3 },
    { 0x7f3, "subio",      3 }, { 0x7f4, "selo",       3 },
};

static const RegOp *reg_lookup(uint16_t op)
{
    size_t lo = 0, hi = sizeof(reg_tab) / sizeof(reg_tab[0]);

    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (reg_tab[mid].op == op) {
            return &reg_tab[mid];
        }
        if (reg_tab[mid].op < op) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const char *i960_reg_name(unsigned r)
{
    static const char *const names[I960_NREG] = {
        "pfp", "sp",  "rip", "r3",  "r4",  "r5",  "r6",  "r7",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
        "g0",  "g1",  "g2",  "g3",  "g4",  "g5",  "g6",  "g7",
        "g8",  "g9",  "g10", "g11", "g12", "g13", "g14", "fp",
    };
    return r < I960_NREG ? names[r] : "?";
}

/* ------------------------------------------------------------------ */
/* Decode                                                             */
/* ------------------------------------------------------------------ */

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t sext(uint32_t v, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

/*
 * MEMB mode field, word0 bits 13:10.  Bit 12 distinguishes MEMA (0) from
 * MEMB (1); within MEMB, bit 12 of the mode nibble selects the forms that
 * carry a second word.
 */
static I960AddrMode memb_mode(unsigned mode, bool *has_disp)
{
    *has_disp = false;
    switch (mode) {
    case 0x4: return I960_AM_ABASE;
    case 0x5: *has_disp = true; return I960_AM_IP_DISP;
    case 0x7: return I960_AM_ABASE_INDEX;
    case 0xc: *has_disp = true; return I960_AM_DISP;
    case 0xd: *has_disp = true; return I960_AM_DISP_ABASE;
    case 0xe: *has_disp = true; return I960_AM_DISP_INDEX;
    case 0xf: *has_disp = true; return I960_AM_DISP_ABASE_INDEX;
    default:  return I960_AM_RESERVED;
    }
}

bool i960_decode_buf(const uint8_t *p, size_t avail, uint32_t addr,
                     I960Insn *out)
{
    uint32_t w;
    unsigned op;

    memset(out, 0, sizeof(*out));
    out->ip = addr;
    out->len = 4;

    if (avail < 4) {
        return false;
    }
    w = ld32(p);
    out->word0 = w;
    op = w >> 24;

    if (op >= 0x08 && op <= 0x1f) {
        out->fmt = I960_FMT_CTRL;
        out->opcode = op;
        out->mnemonic = ctrl_tab[op - 0x08];
        out->prediction = (w >> 1) & 1;
        out->displacement = sext(w & 0x00fffffc, 24);
        out->target = addr + (uint32_t)out->displacement;
        return true;
    }

    if (op >= 0x20 && op <= 0x3f) {
        out->fmt = I960_FMT_COBR;
        out->opcode = op;
        out->mnemonic = cobr_tab[op - 0x20];
        out->srcdst = (w >> 19) & 0x1f;        /* src1 */
        out->src2   = (w >> 14) & 0x1f;
        out->src1_lit = (w >> 13) & 1;          /* M1: src1 is a literal   */
        out->prediction = (w >> 1) & 1;
        out->displacement = sext(w & 0x1ffc, 13);
        out->target = addr + (uint32_t)out->displacement;
        return true;
    }

    if (op >= 0x58 && op <= 0x7f) {
        out->fmt = I960_FMT_REG;
        out->opcode = (uint16_t)((op << 4) | ((w >> 7) & 0xf));
        out->srcdst = (w >> 19) & 0x1f;
        out->src2   = (w >> 14) & 0x1f;
        out->src1   = w & 0x1f;
        out->src1_lit = (w >> 11) & 1;   /* M1 */
        out->src2_lit = (w >> 12) & 1;   /* M2 */
        out->dst_lit  = (w >> 13) & 1;   /* M3: srcdst is a literal/ctrl   */
        const RegOp *r = reg_lookup(out->opcode);
        out->mnemonic = r ? r->name : NULL;
        return true;
    }

    if (op >= 0x80 && op <= 0xcf) {
        out->fmt = I960_FMT_MEM;
        out->opcode = op;
        out->mnemonic = mem_tab[op - 0x80];
        out->srcdst = (w >> 19) & 0x1f;
        out->abase  = (w >> 14) & 0x1f;

        if (!((w >> 12) & 1)) {
            /*
             * MEMA.  Bit 13 selects between a bare offset and
             * offset(abase); the offset is the low 12 bits, unsigned.
             */
            out->am = ((w >> 13) & 1) ? I960_AM_OFFSET_ABASE : I960_AM_OFFSET;
            out->disp = (int32_t)(w & 0xfff);
            return true;
        }

        bool has_disp;
        unsigned mode = (w >> 10) & 0xf;
        out->am    = memb_mode(mode, &has_disp);
        out->scale = (w >> 7) & 0x7;
        out->index = w & 0x1f;
        if (has_disp) {
            if (avail < 8) {
                return false;
            }
            out->word1 = ld32(p + 4);
            out->disp = (int32_t)out->word1;
            out->len = 8;
        }
        return true;
    }

    out->fmt = I960_FMT_UNKNOWN;
    return true;
}

bool i960_decode(I960Core *c, uint32_t addr, I960Insn *out)
{
    uint8_t buf[8];

    if (!c->read(c->opaque, addr, buf, 4)) {
        return false;
    }
    /*
     * Decode once against 4 bytes to learn the length, then top up.  This
     * avoids reading a second word that may be past the end of a mapping
     * for every single-word instruction.
     */
    i960_decode_buf(buf, 4, addr, out);
    if (out->fmt == I960_FMT_MEM && ((out->word0 >> 12) & 1)) {
        bool has_disp;
        memb_mode((out->word0 >> 10) & 0xf, &has_disp);
        if (has_disp) {
            if (!c->read(c->opaque, addr + 4, buf + 4, 4)) {
                return false;
            }
            return i960_decode_buf(buf, 8, addr, out);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Disassembly                                                        */
/* ------------------------------------------------------------------ */

static int op_str(char *b, size_t n, unsigned r, bool lit)
{
    if (lit) {
        return r > 9 ? snprintf(b, n, "0x%X", r) : snprintf(b, n, "%u", r);
    }
    return snprintf(b, n, "%s", i960_reg_name(r));
}

static const char *scale_str(unsigned s)
{
    switch (s) {
    case 0: return "*1";
    case 1: return "*2";
    case 2: return "*4";
    case 3: return "*8";
    case 4: return "*16";
    default: return "*?";
    }
}

int i960_format(const I960Insn *in, char *buf, size_t buflen)
{
    char a[32], b[32];

    if (!in->mnemonic && in->fmt != I960_FMT_MEM) {
        return snprintf(buf, buflen, ".word 0x%08x", in->word0);
    }

    switch (in->fmt) {
    case I960_FMT_CTRL:
        if (!in->mnemonic) {
            break;
        }
        if (in->opcode == 0x0a) {           /* ret takes no operand */
            return snprintf(buf, buflen, "%s", in->mnemonic);
        }
        return snprintf(buf, buflen, "%-8s 0x%08x", in->mnemonic, in->target);

    case I960_FMT_COBR:
        if (!in->mnemonic) {
            break;
        }
        if (in->opcode >= 0x20 && in->opcode <= 0x27) {   /* test* reg */
            return snprintf(buf, buflen, "%-8s %s", in->mnemonic,
                            i960_reg_name(in->srcdst));
        }
        op_str(a, sizeof(a), in->srcdst, in->src1_lit);
        return snprintf(buf, buflen, "%-8s %s, %s, 0x%08x", in->mnemonic,
                        a, i960_reg_name(in->src2), in->target);

    case I960_FMT_REG: {
        const RegOp *r = reg_lookup(in->opcode);
        if (!r) {
            break;
        }
        op_str(a, sizeof(a), in->src1, in->src1_lit);
        op_str(b, sizeof(b), in->src2, in->src2_lit);
        switch (r->nsrc) {
        case 0:
            return snprintf(buf, buflen, "%s", r->name);
        case 1:
            return snprintf(buf, buflen, "%-8s %s", r->name, a);
        case 2:
            return snprintf(buf, buflen, "%-8s %s, %s", r->name, a,
                            i960_reg_name(in->srcdst));
        case NSRC_CMP:
            return snprintf(buf, buflen, "%-8s %s, %s", r->name, a, b);
        default:
            return snprintf(buf, buflen, "%-8s %s, %s, %s", r->name, a, b,
                            i960_reg_name(in->srcdst));
        }
    }

    case I960_FMT_MEM: {
        const char *m = in->mnemonic;
        char ea[64];
        bool store;

        if (!m) {
            break;
        }
        store = (in->opcode == 0x82 || in->opcode == 0x8a ||
                 in->opcode == 0x92 || in->opcode == 0x9a ||
                 in->opcode == 0xa2 || in->opcode == 0xb2 ||
                 in->opcode == 0xc2 || in->opcode == 0xca);

        switch (in->am) {
        case I960_AM_OFFSET:
            snprintf(ea, sizeof(ea), "0x%x", (unsigned)in->disp);
            break;
        case I960_AM_OFFSET_ABASE:
            if (in->disp == 0) {
                snprintf(ea, sizeof(ea), "(%s)", i960_reg_name(in->abase));
            } else {
                snprintf(ea, sizeof(ea), "0x%x(%s)", (unsigned)in->disp,
                         i960_reg_name(in->abase));
            }
            break;
        case I960_AM_ABASE:
            snprintf(ea, sizeof(ea), "(%s)", i960_reg_name(in->abase));
            break;
        case I960_AM_IP_DISP:
            /* The displacement is relative to the *next* word. */
            snprintf(ea, sizeof(ea), "0x%08x(ip)",
                     in->ip + 8 + (uint32_t)in->disp);
            break;
        case I960_AM_ABASE_INDEX:
            snprintf(ea, sizeof(ea), "(%s)[%s%s]", i960_reg_name(in->abase),
                     i960_reg_name(in->index), scale_str(in->scale));
            break;
        case I960_AM_DISP:
            snprintf(ea, sizeof(ea), "0x%08x", (unsigned)in->disp);
            break;
        case I960_AM_DISP_ABASE:
            snprintf(ea, sizeof(ea), "0x%08x(%s)", (unsigned)in->disp,
                     i960_reg_name(in->abase));
            break;
        case I960_AM_DISP_INDEX:
            snprintf(ea, sizeof(ea), "0x%08x[%s%s]", (unsigned)in->disp,
                     i960_reg_name(in->index), scale_str(in->scale));
            break;
        case I960_AM_DISP_ABASE_INDEX:
            snprintf(ea, sizeof(ea), "0x%08x(%s)[%s%s]", (unsigned)in->disp,
                     i960_reg_name(in->abase), i960_reg_name(in->index),
                     scale_str(in->scale));
            break;
        default:
            snprintf(ea, sizeof(ea), "<reserved>");
            break;
        }

        /* bx and callx branch to the effective address; no register. */
        if (in->opcode == 0x84 || in->opcode == 0x86) {
            return snprintf(buf, buflen, "%-8s %s", m, ea);
        }
        if (store) {
            return snprintf(buf, buflen, "%-8s %s, %s", m,
                            i960_reg_name(in->srcdst), ea);
        }
        return snprintf(buf, buflen, "%-8s %s, %s", m, ea,
                        i960_reg_name(in->srcdst));
    }

    default:
        break;
    }

    return snprintf(buf, buflen, ".word 0x%08x", in->word0);
}

void i960_reset(I960Core *c, uint32_t entry)
{
    memset(c->reg, 0, sizeof(c->reg));
    c->ip = entry;
    c->ac = 0;
    c->pc = 0;
    c->tc = 0;
    c->halt_reason = NULL;
    c->icount = 0;
}

/* ================================================================== */
/* Execution                                                          */
/* ================================================================== */

#define HALT(c, in, why) do {                                           \
        (c)->halt_reason = (why);                                       \
        (c)->halt_ip = (in)->ip;                                        \
        (c)->halt_word = (in)->word0;                                   \
        return false;                                                   \
    } while (0)

static bool rd32m(I960Core *c, uint32_t a, uint32_t *v)
{
    uint8_t b[4];
    if (!c->read(c->opaque, a, b, 4)) {
        return false;
    }
    *v = ld32(b);
    return true;
}

static bool wr32m(I960Core *c, uint32_t a, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return c->write(c->opaque, a, b, 4);
}

/* REG source operands: M1/M2 turn the 5-bit field into a literal 0-31. */
static uint32_t s1(const I960Core *c, const I960Insn *in)
{
    return in->src1_lit ? in->src1 : c->reg[in->src1];
}
static uint32_t s2(const I960Core *c, const I960Insn *in)
{
    return in->src2_lit ? in->src2 : c->reg[in->src2];
}

/* Condition code, AC bits 2:0. */
static void set_cc(I960Core *c, unsigned cc)
{
    c->ac = (c->ac & ~(uint32_t)I960_CC_MASK) | (cc & I960_CC_MASK);
}

static void cmp_set(I960Core *c, uint32_t a, uint32_t b, bool sgn)
{
    bool lt = sgn ? ((int32_t)a < (int32_t)b) : (a < b);
    bool eq = (a == b);
    set_cc(c, eq ? I960_CC_EQUAL : (lt ? I960_CC_LESS : I960_CC_GREATER));
}

/*
 * Effective address.  `lda` uses this without touching memory, which is
 * how the firmware materialises constants and frame offsets.
 */
static bool effa(I960Core *c, const I960Insn *in, uint32_t *out)
{
    uint32_t base = 0, idx = 0;

    switch (in->am) {
    case I960_AM_OFFSET:
        *out = (uint32_t)in->disp;
        return true;
    case I960_AM_OFFSET_ABASE:
        *out = c->reg[in->abase] + (uint32_t)in->disp;
        return true;
    case I960_AM_ABASE:
        *out = c->reg[in->abase];
        return true;
    case I960_AM_IP_DISP:
        /* Relative to the word after the two-word instruction. */
        *out = in->ip + 8 + (uint32_t)in->disp;
        return true;
    case I960_AM_DISP:
        *out = (uint32_t)in->disp;
        return true;
    case I960_AM_DISP_ABASE:
        *out = c->reg[in->abase] + (uint32_t)in->disp;
        return true;
    case I960_AM_ABASE_INDEX:
        base = c->reg[in->abase];
        idx  = c->reg[in->index] << in->scale;
        *out = base + idx;
        return true;
    case I960_AM_DISP_INDEX:
        idx  = c->reg[in->index] << in->scale;
        *out = (uint32_t)in->disp + idx;
        return true;
    case I960_AM_DISP_ABASE_INDEX:
        base = c->reg[in->abase];
        idx  = c->reg[in->index] << in->scale;
        *out = base + idx + (uint32_t)in->disp;
        return true;
    default:
        return false;
    }
}

/*
 * call/ret.  See the register-window note in stb-i960.h for why the local
 * set is always spilled rather than cached.
 */
/*
 * A frame pointer this low is never real.  The i960 interrupt table lives
 * at the bottom of memory, so a 64-byte register spill to a small FP
 * writes straight over the vectors and the failure resurfaces thousands
 * of instructions later as "no handler installed" - a symptom with no
 * visible connection to its cause.  Refuse it instead.
 */
#define I960_MIN_FRAME 0x1000u

static bool spill_oldest(I960Core *c)
{
    I960RegSet *s = &c->cache[0];
    int i;

    if (s->fp < I960_MIN_FRAME) {
        c->halt_reason = "register spill to an implausible frame pointer";
        c->halt_ip = c->ip;
        c->halt_word = s->fp;
        return false;
    }
    for (i = 0; i < 16; i++) {
        if (!wr32m(c, s->fp + (uint32_t)i * 4, s->reg[i])) {
            return false;
        }
    }
    memmove(&c->cache[0], &c->cache[1],
            sizeof(c->cache[0]) * (I960_NFRAMES - 1));
    c->depth--;
    return true;
}

static void flog(I960Core *c, char kind, uint32_t ip, uint32_t pfp,
                 uint32_t rip, uint32_t newfp, bool from_cache)
{
    unsigned i = c->flog_n++ % I960_FRAMELOG;
    c->flog[i].ip = ip;      c->flog[i].pfp = pfp;
    c->flog[i].rip = rip;    c->flog[i].newfp = newfp;
    c->flog[i].depth = c->depth;
    c->flog[i].kind = kind;  c->flog[i].from_cache = from_cache;
}

static bool do_call(I960Core *c, uint32_t target, uint32_t retaddr)
{
    uint32_t oldfp = c->reg[I960_FP];
    uint32_t newfp = (c->reg[I960_SP] + (I960_FRAME_ALIGN - 1)) &
                     ~(uint32_t)(I960_FRAME_ALIGN - 1);
    I960RegSet *s;

    if (c->depth == I960_NFRAMES && !spill_oldest(c)) {
        return false;
    }
    if (oldfp < I960_MIN_FRAME) {
        c->halt_reason = "call from an implausible frame pointer";
        c->halt_ip = c->ip;
        c->halt_word = oldfp;
        return false;
    }
    /*
     * RIP belongs to the CALLER, not the callee.
     *
     * `call` stores the return address in r2 of the register set that is
     * current when it executes - the caller's - and that set is then
     * cached and spilled with the return address in it.  `ret` restores
     * the previous set and only then takes IP from its r2.  So RIP is
     * written here, BEFORE the snapshot, and the new frame's own r2 means
     * nothing until it makes a call of its own.
     *
     * An earlier version had this the other way round, giving the return
     * address to the callee and reading IP from the pre-restore r2.  That
     * is self-consistent for ordinary calls, which is why it survived
     * 200,000 instructions, and it is wrong: it loses by exactly one
     * frame whenever software builds or edits a frame by hand.  Both
     * places this firmware does that then misbehave - the type-7 task
     * start (TCT_Build_Task_Stack puts the task entry in the inner save
     * area's RIP) and the context switch (TCT_Control_To_Thread patches
     * [PFP+8] with 0xa02e974c, the mirror of its own register save, and
     * rets) - and the switch instead resumed at the save prologue with
     * stale globals.
     */
    c->reg[I960_RIP] = retaddr;

    s = &c->cache[c->depth++];
    s->fp = oldfp;
    s->is_irq = false;
    memcpy(s->reg, c->reg, sizeof(s->reg));

    /*
     * Write-through.  The hardware cache is write-back - a set reaches
     * memory only when it is evicted - but this firmware enters functions
     * by branch as well as by call, so it rets more often than it calls
     * and drains the cache below its own bottom.  With write-back, memory
     * at those frames was never written and the restore read whatever was
     * lying there.  Writing on every call costs 16 stores and makes the
     * two paths agree, so a cache miss is merely slower, never wrong.
     */
    for (int k = 0; k < 16; k++) {
        if (!wr32m(c, oldfp + (uint32_t)k * 4, s->reg[k])) {
            return false;
        }
    }

    /* PFP and SP describe the frame you are IN: the new one. */
    c->reg[I960_R0] = oldfp;          /* PFP, return type 000 = local */
    c->reg[I960_FP] = newfp;
    c->reg[I960_SP] = newfp + 64;      /* past the register save area */
    c->ip = target;
    flog(c, 'c', retaddr - 4, oldfp, retaddr, newfp, false);
    return true;
}

/*
 * Restoring a register set on `ret`.
 *
 * The cache is write-through (see do_call), so the copy in the frame's
 * memory image is always complete and always current: a cached set belongs
 * to a suspended frame, and a suspended frame is not executing, so nothing
 * can have changed it since the call wrote it out.  **Memory is therefore
 * authoritative and the cache is only a shortcut**, valid exactly when its
 * tag is the frame being returned to.
 *
 * That is not a micro-optimisation, it is the whole context switch.
 * Nucleus PLUS resumes a task by overwriting PFP with a frame address it
 * chose, patching that frame's RIP slot in memory, and executing `ret`
 * (TCT_Control_To_Thread, 0xa02e96f0-0xa02e9748).  The frame it names was
 * never pushed by any call on this path, so a LIFO pop restores the wrong
 * sixteen registers and the task resumes with another function's locals -
 * which is precisely what happened: the switch landed in the TCB writer
 * with its thread pointer holding 4.
 *
 * On a mismatch the cache is discarded entirely rather than popped.  The
 * frames it holds are no longer on the return path - that is what a switch
 * means - and every one of them is already in memory, so dropping them
 * loses nothing and keeps a stale tag from matching a later frame that
 * happens to reuse the address.
 */
static bool restore_set(I960Core *c, uint32_t newfp)
{
    if (c->depth && c->cache[c->depth - 1].fp == newfp) {
        c->depth--;
        memcpy(c->reg, c->cache[c->depth].reg, sizeof(c->cache[0].reg));
        return true;
    }
    if (c->depth) {
        c->tag_mismatches++;
        c->last_mem_restore_tag = c->cache[c->depth - 1].fp;
        c->depth = 0;                       /* the chain moved under us */
    }
    return false;
}

static bool restore_from_memory(I960Core *c, uint32_t newfp)
{
    int i;

    c->mem_restores++;
    c->last_mem_restore_ip = c->ip;
    c->last_mem_restore_fp = newfp;
    c->last_mem_restore_depth = c->depth;
    for (i = 0; i < 16; i++) {
        if (!rd32m(c, newfp + (uint32_t)i * 4, &c->reg[i])) {
            return false;
        }
    }
    return true;
}

static bool do_ret(I960Core *c)
{
    uint32_t pfp = c->reg[I960_R0];
    uint32_t rip = c->reg[I960_RIP];
    uint32_t newfp = pfp & ~(uint32_t)(I960_FRAME_ALIGN - 1);
    unsigned type = pfp & 0x7;
    bool used_cache = false;

    if (pfp == 0) {
        /*
         * The bottom of the chain.  On the real board the ROM called into
         * this firmware and there is a frame beneath it; here there is
         * not, so say that plainly instead of restoring a zeroed set and
         * branching to address zero.
         */
        c->halt_reason = "returned past the bottom of the frame chain";
        c->halt_ip = c->ip;
        c->halt_word = 0;
        return false;
    }

    if (type == 7) {                                /* interrupt return */
        uint32_t saved_pc, saved_ac;
        uint32_t fp = c->reg[I960_FP];

        if (!rd32m(c, fp - 16, &saved_pc) ||
            !rd32m(c, fp - 12, &saved_ac)) {
            return false;
        }
        /*
         * No cache entry is required: the kernel hand-builds these frames
         * to start a task, and such a frame was never pushed by anything.
         */
        if (!restore_set(c, newfp) && !restore_from_memory(c, newfp)) {
            return false;
        }
        c->ac = saved_ac;
        c->pc = saved_pc;
        c->reg[I960_FP] = newfp;
        c->ip = c->reg[I960_RIP];       /* from the restored set */
        flog(c, 'R', fp, pfp, c->ip, newfp, false);
        return true;
    }
    if (type != 0) {
        /*
         * Fault and supervisor returns restore more than the register set
         * and are not modelled yet.  Stop loudly: silently doing a local
         * return would corrupt the trace from that point on.
         */
        c->halt_reason = "ret with non-local return type";
        c->halt_ip = c->ip;
        c->halt_word = pfp;
        return false;
    }

    /*
     * One pop per ret, unconditionally: the cache mirrors call depth, and
     * that is true even when the firmware branches between functions,
     * because the ret still unwinds whatever the last call pushed.
     *
     * A tag check here was tried and was a mistake.  It skipped the pop
     * on a mismatch, so depth only ever grew, and the cache drifted out
     * of step until a ret found it empty and read a register set out of
     * uninitialised memory.  The mismatches it was reacting to were the
     * FP alignment bug, not a real divergence.  The tag is kept, because
     * it is where a spill has to be written, and a mismatch is counted so
     * genuine drift is still visible.
     */
    used_cache = restore_set(c, newfp);
    if (!used_cache && !restore_from_memory(c, newfp)) {
        return false;
    }
    c->reg[I960_FP] = newfp;
    c->ip = c->reg[I960_RIP];       /* the restored frame's return address */
    flog(c, 'r', rip, pfp, c->ip, newfp, used_cache);
    return true;
}

I960IrqResult i960_interrupt(I960Core *c, unsigned vector)
{
    uint32_t oldfp = c->reg[I960_FP];
    uint32_t newfp = (c->reg[I960_SP] + (I960_FRAME_ALIGN - 1)) &
                     ~(uint32_t)(I960_FRAME_ALIGN - 1);
    uint32_t handler = 0, saved_ip = c->ip;
    unsigned prio = vector >> 3;
    I960RegSet *s;

    if (vector < 8 || vector > 255) {
        return I960_IRQ_NO_HANDLER;
    }
    /*
     * Priority is the mask.  The firmware sets PC.priority to 31 as its
     * first act, so anything that does not check this delivers interrupts
     * the real core would hold pending - and produces a trace that looks
     * fine and is not.
     */
    if (prio != 31 && prio <= i960_pc_priority(c->pc)) {
        return I960_IRQ_MASKED;
    }
    if (!rd32m(c, c->intbase + 0x24 + (uint32_t)(vector - 8) * 4, &handler)) {
        return I960_IRQ_FAULT;
    }
    if (handler == 0) {
        return I960_IRQ_NO_HANDLER;
    }

    if (c->depth == I960_NFRAMES && !spill_oldest(c)) {
        return I960_IRQ_FAULT;
    }
    s = &c->cache[c->depth++];
    s->fp = oldfp;
    memcpy(s->reg, c->reg, sizeof(s->reg));

    /*
     * The resume address goes into the SAVED set's RIP, not into the
     * record.  Nucleus PLUS proves this is how the return works: to start
     * a task it hand-builds two stacked save areas, puts the task entry
     * in the inner one's RIP slot (st g4, 0x8(g1) with g4 = 0xa02ec308)
     * and leaves the record's IP word zero.  A type-7 ret that read IP
     * from the record would start every task at address 0.
     */
    s->reg[I960_RIP] = saved_ip;
    s->is_irq = true;

    /*
     * The interrupt record goes in memory, and its layout is not guessed:
     * Nucleus PLUS builds one by hand at 0xa02e9488 to start a task, and
     * that code is the specification -
     *
     *   st g3, -0x10(g2)   saved PC    (from modpc)
     *   st g4, -0x0c(g2)   saved AC    (from modac)
     *   st g6, -0x08(g2)   saved IP
     *   or g1, 7, g4 / st g4, (g2)     PFP with return type 111
     *
     * An earlier version kept this beside the cached register set on the
     * grounds that nothing read it.  Nothing reads the one the processor
     * writes; the kernel writes its own and rets into it, so the record
     * has to be real memory in the layout the kernel uses.
     */
    for (int k = 0; k < 16; k++) {          /* write-through, as for call */
        if (!wr32m(c, oldfp + (uint32_t)k * 4, s->reg[k])) {
            return I960_IRQ_FAULT;
        }
    }
    /* PC and AC only; the IP word is not where the resume address lives. */
    /*
     * The interrupt record, three words below the new frame:
     *
     *   newfp - 16   saved process controls
     *   newfp - 12   saved arithmetic controls
     *   newfp -  8   the VECTOR NUMBER
     *
     * The third word is not optional and it is not an IP.  The firmware's
     * common interrupt entry loads all three with one `ldt` at
     * 0xa02e9b10, passes the third to TCC_Dispatch_LISR, and that uses it
     * to index the vector -> LISR-id table at 0xa033c210.  Leaving it zero
     * makes every LISR-dispatched interrupt look like vector 0, which has
     * no LISR, and Nucleus stops the system with error 4 - a `b .` at
     * 0xa02eabb8 that spins forever.
     *
     * The timer does not go through that path (its handler is installed
     * directly), which is why this was invisible until the Messaging
     * Unit's doorbell was rung.
     */
    if (!wr32m(c, newfp - 16, c->pc) ||
        !wr32m(c, newfp - 12, c->ac) ||
        !wr32m(c, newfp -  8, vector)) {
        return I960_IRQ_FAULT;
    }

    c->pc = (c->pc & ~I960_PC_PRIO_MASK) | I960_PC_STATE |
            ((uint32_t)prio << I960_PC_PRIO_SHIFT);

    c->reg[I960_R0]  = (oldfp & ~(uint32_t)0x7) | 0x7;   /* interrupt ret */
    c->reg[I960_RIP] = saved_ip;      /* what the handler actually reads */
    c->reg[I960_FP]  = newfp;
    c->reg[I960_SP]  = newfp + 64;     /* past the register save area */
    c->ip = handler;
    flog(c, 'i', saved_ip, c->reg[I960_R0], saved_ip, newfp, false);
    return I960_IRQ_DELIVERED;
}

/* Branch condition against AC.cc, for both CTRL and COBR forms. */
static bool cond_true(const I960Core *c, unsigned mask)
{
    unsigned cc = c->ac & I960_CC_MASK;
    return mask ? ((cc & mask) != 0) : (cc == 0);
}

bool i960_step(I960Core *c)
{
    I960Insn ins;
    I960Insn *in = &ins;
    uint32_t next, ea, a, b, r;

    /*
     * FP is a 64-byte-aligned quantity by architecture: the processor
     * ignores g15's low six bits, which is what leaves room for the
     * return-type field in PFP.  The firmware exploits that - its init
     * does `lda 0xa02f6350, g1 / mov g1, fp` with a value aligned to 16,
     * not 64 - so the invariant has to be enforced here rather than
     * assumed.  Storing the unaligned value verbatim made every ret
     * compute PFP & ~63 and miss its own frame by 0x10.
     */
    c->reg[I960_FP] &= ~(uint32_t)(I960_FRAME_ALIGN - 1);

    if (!i960_decode(c, c->ip, in)) {
        c->halt_reason = "instruction fetch failed";
        c->halt_ip = c->ip;
        return false;
    }
    next = c->ip + in->len;
    c->icount++;

    switch (in->fmt) {

    case I960_FMT_CTRL:
        switch (in->opcode) {
        case 0x08:                                  /* b   */
            c->ip = in->target; return true;
        case 0x09:                                  /* call */
            return do_call(c, in->target, next);
        case 0x0a:                                  /* ret */
            return do_ret(c);
        case 0x0b:                                  /* bal */
            c->reg[30] = next;                      /* g14 */
            c->ip = in->target; return true;
        default:
            if (in->opcode >= 0x10 && in->opcode <= 0x17) {
                c->ip = cond_true(c, in->opcode & 7) ? in->target : next;
                return true;
            }
            HALT(c, in, "unimplemented CTRL opcode");
        }

    case I960_FMT_COBR:
        if (in->opcode >= 0x20 && in->opcode <= 0x27) {   /* test<cc> */
            c->reg[in->srcdst] = cond_true(c, in->opcode & 7) ? 1 : 0;
            c->ip = next;
            return true;
        }
        if (in->opcode == 0x30 || in->opcode == 0x37) {   /* bbc / bbs */
            uint32_t bit = s1(c, in) & 31;
            bool set = (c->reg[in->src2] >> bit) & 1;
            bool take = (in->opcode == 0x37) ? set : !set;
            set_cc(c, take ? 0x2 : 0x0);
            c->ip = take ? in->target : next;
            return true;
        }
        if (in->opcode >= 0x31 && in->opcode <= 0x3f) {   /* cmp+branch */
            bool sgn = in->opcode >= 0x38;
            a = in->src1_lit ? in->srcdst : c->reg[in->srcdst];
            b = c->reg[in->src2];
            cmp_set(c, a, b, sgn);
            c->ip = cond_true(c, in->opcode & 7) ? in->target : next;
            return true;
        }
        HALT(c, in, "unimplemented COBR opcode");

    case I960_FMT_MEM:
        if (!effa(c, in, &ea)) {
            HALT(c, in, "reserved MEM addressing mode");
        }
        switch (in->opcode) {
        case 0x8c:                                  /* lda */
            c->reg[in->srcdst] = ea; c->ip = next; return true;
        case 0x84:                                  /* bx */
            c->ip = ea; return true;
        case 0x85:                                  /* balx */
            c->reg[in->srcdst] = next; c->ip = ea; return true;
        case 0x86:                                  /* callx */
            return do_call(c, ea, next);
        case 0x90:                                  /* ld */
            if (!rd32m(c, ea, &c->reg[in->srcdst])) goto memfault;
            c->ip = next; return true;
        case 0x92:                                  /* st */
            if (!wr32m(c, ea, c->reg[in->srcdst])) goto memfault;
            c->ip = next; return true;
        case 0x80: case 0xc0: {                     /* ldob / ldib */
            uint8_t v;
            if (!c->read(c->opaque, ea, &v, 1)) goto memfault;
            c->reg[in->srcdst] = (in->opcode == 0xc0)
                               ? (uint32_t)(int32_t)(int8_t)v : v;
            c->ip = next; return true;
        }
        case 0x82: case 0xc2: {                     /* stob / stib */
            uint8_t v = (uint8_t)c->reg[in->srcdst];
            if (!c->write(c->opaque, ea, &v, 1)) goto memfault;
            c->ip = next; return true;
        }
        case 0x88: case 0xc8: {                     /* ldos / ldis */
            uint8_t t[2];
            uint32_t v;
            if (!c->read(c->opaque, ea, t, 2)) goto memfault;
            v = (uint32_t)t[0] | ((uint32_t)t[1] << 8);
            c->reg[in->srcdst] = (in->opcode == 0xc8)
                               ? (uint32_t)(int32_t)(int16_t)v : v;
            c->ip = next; return true;
        }
        case 0x8a: case 0xca: {                     /* stos / stis */
            uint32_t v = c->reg[in->srcdst];
            uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
            if (!c->write(c->opaque, ea, t, 2)) goto memfault;
            c->ip = next; return true;
        }
        case 0x98: case 0xa0: case 0xb0: {          /* ldl / ldt / ldq */
            int n = (in->opcode == 0x98) ? 2 : (in->opcode == 0xa0) ? 3 : 4;
            for (int i = 0; i < n; i++) {
                if (!rd32m(c, ea + (uint32_t)i * 4,
                           &c->reg[(in->srcdst + i) & 31])) goto memfault;
            }
            c->ip = next; return true;
        }
        case 0x9a: case 0xa2: case 0xb2: {          /* stl / stt / stq */
            int n = (in->opcode == 0x9a) ? 2 : (in->opcode == 0xa2) ? 3 : 4;
            for (int i = 0; i < n; i++) {
                if (!wr32m(c, ea + (uint32_t)i * 4,
                           c->reg[(in->srcdst + i) & 31])) goto memfault;
            }
            c->ip = next; return true;
        }
        default:
            HALT(c, in, "unimplemented MEM opcode");
        }
    memfault:
        c->halt_reason = "memory access refused";
        c->halt_ip = in->ip;
        c->halt_word = ea;
        return false;

    case I960_FMT_REG:
        a = s1(c, in);
        b = s2(c, in);
        switch (in->opcode) {
        case 0x5cc: r = a; break;                       /* mov      */
        case 0x590: r = b + a; break;                   /* addo     */
        case 0x591: r = (uint32_t)((int32_t)b + (int32_t)a); break;
        case 0x592: r = b - a; break;                   /* subo     */
        case 0x593: r = (uint32_t)((int32_t)b - (int32_t)a); break;
        case 0x581: r = b & a; break;                   /* and      */
        case 0x582: r = b & ~a; break;                  /* andnot   */
        case 0x587: r = b | a; break;                   /* or       */
        case 0x58b: r = b | ~a; break;                  /* ornot    */
        case 0x586: r = b ^ a; break;                   /* xor      */
        case 0x588: r = ~(b | a); break;                /* nor      */
        case 0x589: r = ~(b ^ a); break;                /* xnor     */
        case 0x58e: r = ~(b & a); break;                /* nand     */
        case 0x584: r = ~b & a; break;                  /* notand   */
        case 0x58d: r = ~b | a; break;                  /* notor    */
        case 0x58a: r = ~a; break;                      /* not      */
        case 0x583: r = b | (1u << (a & 31)); break;    /* setbit   */
        case 0x58c: r = b & ~(1u << (a & 31)); break;   /* clrbit   */
        case 0x580: r = b ^ (1u << (a & 31)); break;    /* notbit   */
        case 0x58f:                                     /* alterbit */
            /* Sets or clears the bit according to AC.cc bit 1. */
            r = (c->ac & 0x2) ? (b |  (1u << (a & 31)))
                              : (b & ~(1u << (a & 31)));
            break;
        /*
         * Shift counts are taken whole, not masked: the architecture
         * defines a count above 31 as shifting the operand away, and the
         * firmware relies on it.  Masking would silently turn a shift of
         * 32 into a no-op.
         */
        case 0x598: r = (a > 31) ? 0 : (b >> a); break;             /* shro */
        case 0x59c: r = (a > 31) ? 0 : (b << a); break;             /* shlo */
        case 0x59e: r = (a > 31) ? 0 : (uint32_t)((int32_t)b << a); break;
        case 0x59b: r = (a > 31) ? (uint32_t)((int32_t)b >> 31)
                                 : (uint32_t)((int32_t)b >> a); break;
        case 0x59d: {                                   /* rotate   */
            unsigned n = a & 31;
            r = n ? ((b << n) | (b >> (32 - n))) : b;
            break;
        }
        case 0x701: r = b * a; break;                   /* mulo     */
        case 0x741: r = (uint32_t)((int32_t)b * (int32_t)a); break;
        case 0x70b: if (!a) { HALT(c, in, "divide by zero"); }
            r = b / a; break;                           /* divo     */
        case 0x74b: if (!a) { HALT(c, in, "divide by zero"); }
            r = (uint32_t)((int32_t)b / (int32_t)a); break;
        case 0x708: if (!a) { HALT(c, in, "divide by zero"); }
            r = b % a; break;                           /* remo     */
        case 0x748: if (!a) { HALT(c, in, "divide by zero"); }
            r = (uint32_t)((int32_t)b % (int32_t)a); break;
        case 0x749: if (!a) { HALT(c, in, "divide by zero"); }
            r = (uint32_t)((int32_t)b - ((int32_t)b / (int32_t)a) * (int32_t)a);
            break;                                      /* modi     */

        /* Compares write only AC.cc. */
        case 0x5a0: cmp_set(c, a, b, false); c->ip = next; return true;
        case 0x5a1: cmp_set(c, a, b, true);  c->ip = next; return true;
        case 0x594: cmp_set(c, a & 0xff, b & 0xff, false);
            c->ip = next; return true;
        case 0x595: cmp_set(c, (uint32_t)(int32_t)(int8_t)a,
                            (uint32_t)(int32_t)(int8_t)b, true);
            c->ip = next; return true;
        case 0x596: cmp_set(c, a & 0xffff, b & 0xffff, false);
            c->ip = next; return true;
        case 0x597: cmp_set(c, (uint32_t)(int32_t)(int16_t)a,
                            (uint32_t)(int32_t)(int16_t)b, true);
            c->ip = next; return true;
        case 0x5ae:                                     /* chkbit   */
            set_cc(c, ((b >> (a & 31)) & 1) ? I960_CC_EQUAL : 0);
            c->ip = next; return true;

        case 0x641: {                                   /* scanbit  */
            int i;
            r = 0xffffffffu; set_cc(c, 0);
            for (i = 31; i >= 0; i--) {
                if (a & (1u << i)) { r = (uint32_t)i;
                                     set_cc(c, I960_CC_EQUAL); break; }
            }
            break;
        }
        case 0x640: {                                   /* spanbit  */
            int i;
            r = 0xffffffffu; set_cc(c, 0);
            for (i = 31; i >= 0; i--) {
                if (!(a & (1u << i))) { r = (uint32_t)i;
                                        set_cc(c, I960_CC_EQUAL); break; }
            }
            break;
        }
        case 0x650: {                                   /* modify   */
            uint32_t mask = a, src = b;
            r = (c->reg[in->srcdst] & ~mask) | (src & mask);
            break;
        }
        /*
         * Control registers.  PC, AC and TC are modelled as plain words:
         * the firmware reads and writes them (priority, trace enables,
         * interrupt state) long before anything depends on their side
         * effects, so storing them faithfully is enough to keep going,
         * and the effects can be added when something observes them.
         *
         * All three have the same shape - mask of 0 means read only.
         */
        case 0x655: {                                   /* modpc    */
            uint32_t old = c->pc;
            if (b) { c->pc = (c->reg[in->srcdst] & b) | (c->pc & ~b); }
            r = old;
            break;
        }
        case 0x645: {                                   /* modac    */
            uint32_t old = c->ac;
            if (a) { c->ac = (b & a) | (c->ac & ~a); }
            r = old;
            break;
        }
        case 0x654: {                                   /* modtc    */
            uint32_t old = c->tc;
            if (a) { c->tc = (b & a) | (c->tc & ~a); }
            r = old;
            break;
        }

        /*
         * Cache and bus control.  There are no caches here and the board
         * model has no notion of one, so these report "nothing cached"
         * rather than pretending to flush something.
         */
        case 0x65b:                                     /* icctl    */
        case 0x65c: r = 0; break;                       /* dcctl    */
        case 0x658: r = 0; break;                       /* intctl   */
        case 0x659: r = 0; break;                       /* sysctl   */
        case 0x65d:                                     /* halt     */
            c->halt_reason = "halt instruction";
            c->halt_ip = in->ip;
            c->halt_word = a;
            return false;
        case 0x66b:                                     /* mark     */
        case 0x66c: c->ip = next; return true;          /* fmark    */

        case 0x5ad: {                                   /* bswap    */
            r = ((a & 0x000000ffu) << 24) | ((a & 0x0000ff00u) << 8) |
                ((a & 0x00ff0000u) >> 8)  | ((a & 0xff000000u) >> 24);
            break;
        }
        case 0x5ac: {                                   /* scanbyte  */
            unsigned k;
            set_cc(c, 0);
            for (k = 0; k < 4; k++) {
                if (((a >> (k * 8)) & 0xff) == ((b >> (k * 8)) & 0xff)) {
                    set_cc(c, I960_CC_EQUAL);
                    break;
                }
            }
            c->ip = next; return true;
        }
        case 0x651: {                                   /* extract   */
            uint32_t bitpos = a & 31, len = b;
            uint32_t v = c->reg[in->srcdst] >> bitpos;
            r = (len >= 32) ? v : (v & ((1u << len) - 1));
            break;
        }
        case 0x5b0: {                                   /* addc      */
            uint64_t t = (uint64_t)a + b + ((c->ac >> 1) & 1);
            r = (uint32_t)t;
            c->ac = (c->ac & ~0x6u) | (uint32_t)((t >> 32) & 1) << 1;
            break;
        }
        case 0x5b2: {                                   /* subc      */
            uint64_t t = (uint64_t)b - a - (1 - ((c->ac >> 1) & 1));
            r = (uint32_t)t;
            c->ac = (c->ac & ~0x6u) | (uint32_t)(((t >> 32) & 1) ^ 1) << 1;
            break;
        }
        case 0x5a2: {                                   /* concmpo   */
            if (!(c->ac & I960_CC_EQUAL)) { cmp_set(c, a, b, false); }
            c->ip = next; return true;
        }
        case 0x5a3: {                                   /* concmpi   */
            if (!(c->ac & I960_CC_EQUAL)) { cmp_set(c, a, b, true); }
            c->ip = next; return true;
        }
        case 0x5a4: cmp_set(c, a, b, false); r = a + 1; break;  /* cmpinco */
        case 0x5a5: cmp_set(c, a, b, true);  r = a + 1; break;  /* cmpinci */
        case 0x5a6: cmp_set(c, a, b, false); r = a - 1; break;  /* cmpdeco */
        case 0x5a7: cmp_set(c, a, b, true);  r = a - 1; break;  /* cmpdeci */

        case 0x5dc: case 0x5ec: case 0x5fc: {           /* movl/t/q  */
            int n = (in->opcode == 0x5dc) ? 2 : (in->opcode == 0x5ec) ? 3 : 4;
            /* Source is a register group; literals are not legal here. */
            for (int k = 0; k < n; k++) {
                c->reg[(in->srcdst + k) & 31] =
                    in->src1_lit ? 0 : c->reg[(in->src1 + k) & 31];
            }
            c->ip = next; return true;
        }
        case 0x670: {                                   /* emul      */
            uint64_t t = (uint64_t)a * (uint64_t)b;
            c->reg[in->srcdst & ~1u]       = (uint32_t)t;
            c->reg[(in->srcdst & ~1u) + 1] = (uint32_t)(t >> 32);
            c->ip = next; return true;
        }
        case 0x671: {                                   /* ediv      */
            uint64_t num = ((uint64_t)c->reg[(in->src2 + 1) & 31] << 32) |
                           c->reg[in->src2 & 31];
            if (!a) { HALT(c, in, "divide by zero"); }
            c->reg[in->srcdst & ~1u]       = (uint32_t)(num % a);
            c->reg[(in->srcdst & ~1u) + 1] = (uint32_t)(num / a);
            c->ip = next; return true;
        }
        case 0x5d8: {                                   /* eshro     */
            uint64_t t = ((uint64_t)c->reg[(in->src2 + 1) & 31] << 32) |
                         c->reg[in->src2 & 31];
            r = (uint32_t)(t >> (a & 31));
            break;
        }

        /*
         * Atomic read-modify-write.  Single-threaded here, so the
         * atomicity is free; what matters is the data movement.
         */
        case 0x610: {                                   /* atmod     */
            uint32_t ea2 = a & ~3u, old2;
            if (!rd32m(c, ea2, &old2)) { HALT(c, in, "atmod read"); }
            if (!wr32m(c, ea2, (c->reg[in->srcdst] & b) | (old2 & ~b))) {
                HALT(c, in, "atmod write");
            }
            r = old2;
            break;
        }
        case 0x612: {                                   /* atadd     */
            uint32_t ea2 = a & ~3u, old2;
            if (!rd32m(c, ea2, &old2)) { HALT(c, in, "atadd read"); }
            if (!wr32m(c, ea2, old2 + b)) { HALT(c, in, "atadd write"); }
            r = old2;
            break;
        }
        case 0x615: {                                   /* synld     */
            if (!rd32m(c, a & ~3u, &r)) { HALT(c, in, "synld read"); }
            set_cc(c, I960_CC_EQUAL);
            break;
        }

        case 0x660:                                     /* calls    */
            /*
             * The service runs on the host side and returns here, so the
             * only architectural effect modelled is that execution
             * continues after the call.  A real calls would build a
             * supervisor frame that the service's own ret unwinds; since
             * the service never executes i960 instructions, there is no
             * frame to unwind and synthesising one would only create
             * state nothing reads.
             */
            if (!c->syscall || !c->syscall(c->opaque, c, a)) {
                c->halt_reason = "unhandled supervisor call";
                c->halt_ip = in->ip;
                c->halt_word = a;
                return false;
            }
            c->ip = next;
            return true;

        case 0x5b4: c->ip = next; return true;          /* intdis   */
        case 0x5b5: c->ip = next; return true;          /* inten    */
        case 0x66d:                                     /* flushreg */
            while (c->depth) {
                if (!spill_oldest(c)) { HALT(c, in, "flushreg spill"); }
            }
            c->ip = next; return true;
        case 0x66f: c->ip = next; return true;          /* syncf    */

        default:
            /* Conditional add/subtract/select, the Jx family. */
            if (in->opcode >= 0x780 && in->opcode <= 0x7f4) {
                unsigned lo = in->opcode & 0xf;
                unsigned cc = (in->opcode >> 4) & 0x7;
                bool take = cond_true(c, cc);
                switch (lo) {
                case 0x0: r = take ? b + a : c->reg[in->srcdst]; break;
                case 0x1: r = take ? (uint32_t)((int32_t)b + (int32_t)a)
                                   : c->reg[in->srcdst]; break;
                case 0x2: r = take ? b - a : c->reg[in->srcdst]; break;
                case 0x3: r = take ? (uint32_t)((int32_t)b - (int32_t)a)
                                   : c->reg[in->srcdst]; break;
                case 0x4: r = take ? b : a; break;      /* sel<cc>  */
                default:
                    HALT(c, in, "unimplemented conditional REG opcode");
                }
                break;
            }
            HALT(c, in, "unimplemented REG opcode");
        }
        c->reg[in->srcdst] = r;
        c->ip = next;
        return true;

    default:
        HALT(c, in, "undecodable word");
    }
}
