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

        if (in->opcode == 0x84) {                     /* bx: no srcdst */
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
