/*
 * Rendition Verite V2200 internal RISC interpreter.
 *
 * See stb-v2200.h for provenance.  Section numbers in comments refer to
 * the V2200 technical specification, RevB 1/98.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "stb-v2200.h"

/* ------------------------------------------------------------------ */
/* The register file                                                   */
/*                                                                     */
/* %0..%31 and %56/%57 are hardwired constants (spec table 2.2b).  The */
/* microcode leans on them constantly - h0msk and h1msk alone appear in */
/* every FIFO read - so getting one wrong corrupts data rather than     */
/* faulting, which is the worst failure mode available.                 */
/* ------------------------------------------------------------------ */
static const struct { uint8_t index; uint32_t value; } v2200_consts[] = {
    {  0, 0x00000000 }, {  1, 0x00000000 }, {  2, 0x00008000 },
    {  3, 0x80000000 }, {  4, 0xFF000000 }, {  5, 0x00FF0000 },
    {  6, 0x0000FF00 }, {  7, 0x000000FF }, {  8, 0xFFFF0000 },
    {  9, 0x0000FFFF }, { 10, 0xFF00FF00 }, { 11, 0x00FF00FF },
    { 12, 0x00000000 }, { 13, 0xFFFFFFFF }, { 14, 0x00000000 },
    { 15, 0x00010000 }, { 16, 0xFFFFFFFF }, { 17, 0x00010000 },
    { 18, 0x007FFFFF }, { 19, 0x02000000 }, { 20, 0x00800000 },
    { 21, 0x00020000 }, { 22, 0x0000B000 }, { 23, 0x0000B800 },
    { 24, 0x0000C000 }, { 25, 0x0000C800 }, { 26, 0x0000D000 },
    { 27, 0x0000D800 }, { 28, 0x0000E000 }, { 29, 0x0000E800 },
    { 30, 0x0000F000 }, { 31, 0x0000F800 },
    { 56, 0xFFFFF000 }, { 57, 0xFFFFF800 },
};

static bool is_const(unsigned i)
{
    return (i < 32) || i == 56 || i == 57;
}

static const char *const v2200_names[64] = {
    "zero", "zer1", "if0_5", "msb", "b0msk", "b1msk", "b2msk", "b3msk",
    "h0msk", "h1msk", "b0b2msk", "b1b3msk", "zer2", "minus1", "zer3",
    "if1_0", "allfs", "bit16", "fpfrac", "bit25", "bit23", "bit17",
    "b000", "b800", "c000", "c800", "d000", "d800", "e000", "e800",
    "f000", "f800", "swfifo", "fifo", "seed", "seg", "scale", "flag",
    "clip", "timer", "drawctl", "npfifo", NULL, NULL, NULL, NULL,
    "winclip", "riscintr", NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, "msk12", "msk11", NULL, NULL, NULL, NULL, NULL, NULL,
};

const char *v2200_reg_name(unsigned index)
{
    if (index < 64 && v2200_names[index]) {
        return v2200_names[index];
    }
    return NULL;
}

/*
 * Indirect register access, spec 3.3.  An operand index with b7:b6 == 01
 * is a window onto %128..%255:
 *
 *   Index = 128 | (((Seg >> (idx[5:4] * 4)) & 7) * 16) | idx[3:0]
 */
static unsigned resolve(V2200Core *c, unsigned idx)
{
    unsigned sel, base;

    if ((idx & 0xC0) != 0x40) {
        return idx;
    }
    sel  = (idx >> 4) & 3;
    base = (c->reg[V2200_R_SEG] >> (sel * 4)) & 7;
    return 128 | (base * 16) | (idx & 0xF);
}

static uint32_t swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) |
           (v << 24);
}

static void set_flag(V2200Core *c, uint32_t bit, bool on)
{
    if (on) {
        c->reg[V2200_R_FLAG] |= bit;
    } else {
        c->reg[V2200_R_FLAG] &= ~bit;
    }
}

static void refresh_flag(V2200Core *c)
{
    uint32_t peek;

    set_flag(c, V2200_FLAG_FIFOINRDY,
             c->fifo_peek && c->fifo_peek(c->opaque, &peek));
    set_flag(c, V2200_FLAG_FIFOOUTRDY, true);
}

static void halt(V2200Core *c, const char *why, uint32_t word)
{
    c->halt_reason = why;
    c->halt_pc = c->pc;
    c->halt_word = word;
    c->halted = true;
}

/* ---- source and destination ------------------------------------- */

static uint32_t read_reg(V2200Core *c, unsigned idx)
{
    unsigned r = resolve(c, idx);
    uint32_t v = 0;

    switch (r) {
    case V2200_R_FIFO:
    case V2200_R_SWFIFO:
        if (!c->fifo_in || !c->fifo_in(c->opaque, &v)) {
            halt(c, "read from an empty input FIFO", r);
            return 0;
        }
        return r == V2200_R_SWFIFO ? swap32(v) : v;
    case V2200_R_NPFIFO:
    case V2200_R_DRAWCTL:          /* npswfifo when read */
        if (!c->fifo_peek || !c->fifo_peek(c->opaque, &v)) {
            return 0;
        }
        return r == V2200_R_DRAWCTL ? swap32(v) : v;
    case V2200_R_FLAG:
        refresh_flag(c);
        return c->reg[V2200_R_FLAG];
    case V2200_R_WINCLIP:
        /* The last four clip flags, in b5:b2 (spec 3.4). */
        return c->reg[V2200_R_CLIP] & 0x3C;
    default:
        break;
    }
    return c->reg[r];
}

static void write_reg(V2200Core *c, unsigned idx, uint32_t v)
{
    unsigned r = resolve(c, idx);

    if (is_const(r)) {
        return;                     /* "Writes are ignored" */
    }
    switch (r) {
    case V2200_R_FIFO:
    case V2200_R_SWFIFO:
        if (c->fifo_out && !c->fifo_out(c->opaque, v)) {
            halt(c, "write to a full output FIFO", r);
        }
        return;
    case V2200_R_WINCLIP:
    case V2200_R_SCALE:
    case V2200_R_SEED:
    case V2200_R_NPFIFO:
        return;                     /* read only */
    case V2200_R_RISCINTR:
        /* Write-one-to-clear on the status bits; the enables are plain. */
        c->reg[r] = (c->reg[r] & ~(v & 0xFFFF)) | (v & 0xFFFF0000u);
        return;
    default:
        break;
    }
    c->reg[r] = v;
}

uint32_t v2200_reg_read(V2200Core *c, unsigned index)
{
    return c->reg[index & 0xFF];
}

void v2200_reg_write(V2200Core *c, unsigned index, uint32_t v)
{
    c->reg[index & 0xFF] = v;
}

/* ---- memory ------------------------------------------------------ */

static bool ld(V2200Core *c, uint32_t a, unsigned len, uint32_t *out)
{
    uint8_t b[4];
    unsigned i;
    uint32_t v = 0;

    if (!c->read || !c->read(c->opaque, a, b, len)) {
        halt(c, "unreadable address", a);
        return false;
    }
    for (i = 0; i < len; i++) {                 /* big-endian */
        v = (v << 8) | b[i];
    }
    *out = v;
    return true;
}

static bool st(V2200Core *c, uint32_t a, unsigned len, uint32_t v)
{
    uint8_t b[4];
    unsigned i;

    for (i = 0; i < len; i++) {
        b[i] = (uint8_t)(v >> ((len - 1 - i) * 8));
    }
    if (!c->write || !c->write(c->opaque, a, b, len)) {
        halt(c, "unwritable address", a);
        return false;
    }
    return true;
}

/* ---- seed and scale (spec 3.5) ----------------------------------- */

static uint32_t scale_of(uint32_t x)
{
    uint32_t n = 0;

    while (x) {
        x >>= 1;
        n++;
    }
    return n;                       /* leading 1 position + 1, 0 for 0 */
}

/*
 * The seed ROM is a hardware table this model does not have.  What the
 * spec defines is its shape - a left-aligned reciprocal of the normalised
 * input, indexed by thirteen bits - so the value is computed exactly here
 * and the ROM's quantisation is the only difference.  Every use in the
 * Tulsa microcode is a perspective or scale divide, where that difference
 * is below a pixel, but it is an approximation and is marked as one.
 */
static void do_seed(V2200Core *c, uint32_t x)
{
    uint32_t sc = scale_of(x);
    uint32_t norm, seed;

    c->reg[V2200_R_SCALE] = sc;
    if (!sc) {
        c->reg[V2200_R_SEED] = 0xFFFFFFFFu;
        return;
    }
    norm = x << (32 - sc);                      /* 1.xxx in Q31, b31 set */
    seed = (uint32_t)(((uint64_t)1 << 62) / norm);
    c->reg[V2200_R_SEED] = seed;
    c->reg[V2200_R_SEG] = (c->reg[V2200_R_SEG] & 0x0007FFFFu) |
                          ((norm >> 19) << 19);
}

/* ---- the ALU ----------------------------------------------------- */

static uint32_t f2i(uint32_t bits, uint32_t shift)
{
    int exp = (int)((bits >> 23) & 0xFF) - 127;
    int64_t mant = (int64_t)((bits & 0x7FFFFF) | 0x800000);
    int64_t r;
    int sh;

    if (((bits >> 23) & 0xFF) == 0) {
        return 0;
    }
    sh = exp - 23 + (int)shift;
    if (sh >= 0) {
        if (sh > 40) {
            return (bits & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
        }
        r = mant << sh;
    } else {
        r = mant >> (-sh);
        if (bits & 0x80000000u) {
            /* round toward minus infinity */
            if ((-sh) < 63 && (mant & (((int64_t)1 << (-sh)) - 1))) {
                r++;
            }
        }
    }
    if (bits & 0x80000000u) {
        r = -r;
    }
    if (r > 0x7FFFFFFF) {
        return 0x7FFFFFFFu;
    }
    if (r < -(int64_t)0x80000000) {
        return 0x80000000u;
    }
    return (uint32_t)r;
}

static uint32_t alu(V2200Core *c, unsigned fn, uint32_t s2, uint32_t s1,
                    bool *writes_d)
{
    int32_t a = (int32_t)s2, b = (int32_t)s1;
    int64_t p;

    *writes_d = true;
    switch (fn) {
    case 0x0: return s2 + s1;                           /* add     */
    case 0x1: return s2 - s1;                           /* sub     */
    case 0x2: return s2 & ~s1;                          /* andn    */
    case 0x3: return s1 - s2;                           /* subf    */
    case 0x4: return s2 & s1;                           /* and     */
    case 0x5: return s2 | s1;                           /* or      */
    case 0x6: return ~(s2 | s1);                        /* nor     */
    case 0x7: return s2 ^ s1;                           /* xor     */
    case 0x8: p = (int64_t)a * b; return (uint32_t)p;
    case 0x9: p = (int64_t)a * b; return (uint32_t)(p >> 8);
    case 0xA: p = (int64_t)a * b; return (uint32_t)(p >> 16);
    case 0xB: p = (int64_t)a * b; return (uint32_t)(p >> 24);
    case 0xC: p = (int64_t)a * b; return (uint32_t)(p >> 32);
    case 0xD: p = (int64_t)a * b; return (uint32_t)(p >> 31);
    case 0xE:                                           /* clip    */
        c->reg[V2200_R_CLIP] = ((c->reg[V2200_R_CLIP] << 1) & 0xFC) |
                               ((a - b <= 0) ? 0 : 4);
        *writes_d = false;
        return 0;
    case 0xF: return f2i(s2, s1 & 0x1F);                /* f2i     */
    default: break;
    }
    *writes_d = false;
    return 0;
}

static bool alu_x(V2200Core *c, unsigned op, unsigned d, unsigned s2i,
                  unsigned s1i, bool imm, uint32_t *out, bool *writes_d)
{
    uint32_t s2 = read_reg(c, s2i);
    uint32_t s1 = imm ? s1i : read_reg(c, s1i);
    int32_t a = (int32_t)s2, b = (int32_t)s1;

    *writes_d = true;
    switch (op & 0xF) {
    case 0x0: *out = s2 + (s1 << 16); return true;       /* addif  */
    case 0x1: *out = s2 - (s1 << 16); return true;       /* subif  */
    case 0x2:                                            /* seedsr */
        if (!imm && op == 0x53) {
            /* scalar form, same operands */
        }
        do_seed(c, s2);
        *out = (c->reg[V2200_R_SCALE] >= s1)
             ? c->reg[V2200_R_SEED] >> (c->reg[V2200_R_SCALE] - s1)
             : c->reg[V2200_R_SEED];
        return true;
    case 0x3:
        if (!imm) {                                      /* abs    */
            *out = (uint32_t)(b < 0 ? -(int64_t)b : b);
            return true;
        }
        *out = (s1 << 16) - s2;                          /* subfif */
        return true;
    case 0x4: *out = s1 ? ((s2 >> (s1 & 31)) | (s2 << (32 - (s1 & 31))))
                        : s2; return true;               /* rotr   */
    case 0x5: *out = s2 << (s1 & 31); return true;       /* sl     */
    case 0x6: *out = (uint32_t)(a >> (s1 & 31)); return true;  /* asr */
    case 0x7: *out = s2 >> (s1 & 31); return true;       /* sr     */
    case 0x8: *out = (a < b); return true;               /* slt    */
    case 0x9: *out = (s2 < s1); return true;             /* sltu   */
    case 0xA: *out = (s2 == s1); return true;            /* seq    */
    case 0xB: *out = s2 + (s1 << 8); return true;        /* addsl8 */
    case 0xC: *out = (a < b) ? s2 : s1; return true;     /* min    */
    case 0xD: *out = (a > b) ? s2 : s1; return true;     /* max    */
    case 0xE:                                            /* at     */
        if (!imm) {
            *out = (s2 + s1) & 0xFFFF0000u;
            return true;
        }
        *writes_d = false;
        return true;                                     /* sprii handled */
    default:
        break;
    }
    (void)d;
    *writes_d = false;
    return false;
}

/* ---- the step/draw/fill engines ---------------------------------- */

static void engine_op(V2200Core *c, unsigned op, unsigned d, unsigned s2,
                      unsigned s1)
{
    c->engine_ops++;
    if (c->engine) {
        c->engine(c->opaque, op, d, s2, s1);
    }
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

void v2200_reset(V2200Core *c, uint32_t entry)
{
    unsigned i;

    memset(c->reg, 0, sizeof(c->reg));
    for (i = 0; i < sizeof(v2200_consts) / sizeof(v2200_consts[0]); i++) {
        c->reg[v2200_consts[i].index] = v2200_consts[i].value;
    }
    /* Seg bits 3, 7, 11 and 15 always read as 1 (spec 3.3). */
    c->reg[V2200_R_SEG] = 0x8888;
    c->reg[V2200_R_FLAG] = V2200_FLAG_ICACHEOFF | V2200_FLAG_FIFOOUTRDY;
    c->pc = entry;
    c->branch_pending = false;
    c->halted = false;
    c->halt_reason = NULL;
    c->icount = c->stalls = c->pix_writes = c->engine_ops = 0;
}

bool v2200_step(V2200Core *c)
{
    uint32_t w, next;
    unsigned op, D, S2, S1;
    bool taken_pending = c->branch_pending;
    uint32_t taken_pc = c->branch_pc;
    bool branched = false;

    if (c->halted) {
        return false;
    }
    if (!ld(c, c->pc, 4, &w)) {
        return false;
    }
    next = c->pc + 4;
    op = w >> 24;
    D  = (w >> 16) & 0xFF;
    S2 = (w >> 8) & 0xFF;
    S1 = w & 0xFF;
    /*
     * The processor stalls - it does not fault - when it reads an empty
     * input FIFO (spec 3.1).  Catch that before anything is written, so a
     * stalled instruction can simply be re-executed when the host pushes
     * the next word.  Only %32 and %33 pop; %40 and %41 peek.
     */
    {
        /*
         * Which operand fields are register numbers rather than
         * immediates or offsets differs by format, and getting it wrong
         * here is invisible: an `andi r, flag, #32` would look like a
         * read of %32 (swfifo) and stall the machine forever.
         */
        bool s1_reg = false, s2_reg = false;
        unsigned a1, a2;

        if (op <= 0x0F) {                       /* ALU, S1 immediate   */
            s2_reg = true;
        } else if (op <= 0x3F) {                /* ALU, both registers */
            s1_reg = s2_reg = true;
        } else if (op <= 0x4F) {                /* scalar-immediate    */
            s2_reg = (op != 0x4E);
        } else if (op <= 0x5F) {
            s1_reg = s2_reg = true;
        } else if (op <= 0x65) {                /* branches            */
            s1_reg = true;
        } else if (op >= 0x70 && op <= 0x75) {  /* loads: S2 = offset  */
            s1_reg = true;
        } else if (op >= 0x78) {                /* stores: D = offset  */
            s1_reg = s2_reg = true;
        } else if (op >= 0xC0) {                /* engines, sort, …    */
            s1_reg = s2_reg = true;
        }
        a1 = s1_reg ? resolve(c, S1) : 0xFFFF;
        a2 = s2_reg ? resolve(c, S2) : 0xFFFF;

        if (a1 == V2200_R_FIFO || a1 == V2200_R_SWFIFO ||
            a2 == V2200_R_FIFO || a2 == V2200_R_SWFIFO) {
            uint32_t peek;

            if (!c->fifo_peek || !c->fifo_peek(c->opaque, &peek)) {
                c->stalls++;
                c->branch_pending = taken_pending;
                c->branch_pc = taken_pc;
                return true;            /* re-execute when data arrives */
            }
        }
    }

    c->icount++;
    c->reg[V2200_R_TIMER]++;
    c->branch_pending = false;

    if (op <= 0x3F) {                           /* vectorizable ALU */
        unsigned fn = op & 0xF, form = op >> 4;
        bool wd;

        if (fn == 0xF && form >= 2) {
            halt(c, "f2i has no vector form", w);
            return false;
        }
        if (form <= 1) {                        /* immediate, scalar */
            uint32_t s2 = read_reg(c, S2);
            uint32_t s1 = form ? read_reg(c, S1) : S1;
            uint32_t r = alu(c, fn, s2, s1, &wd);

            if (wd) {
                write_reg(c, D, r);
            }
        } else {                                /* vector, vector-scalar */
            uint32_t s2a = read_reg(c, S2);
            uint32_t s2b = read_reg(c, S2 | 1);
            uint32_t s1a, s1b;
            uint32_t ra, rb;

            if (form == 3) {                    /* one scalar, both lanes */
                s1a = s1b = read_reg(c, S1);
            } else {
                s1a = read_reg(c, S1);
                s1b = read_reg(c, S1 | 1);
            }
            ra = alu(c, fn, s2a, s1a, &wd);
            rb = alu(c, fn, s2b, s1b, &wd);
            if (wd) {
                write_reg(c, D & ~1u, ra);
                write_reg(c, D | 1, rb);
            }
        }
    } else if (op >= 0x40 && op <= 0x5F) {
        bool imm = op < 0x50;
        uint32_t r = 0;
        bool wd = false;

        if (op == 0x4E) {                       /* sprii reg,#immed16 */
            unsigned px = S1;
            uint32_t v = (w >> 8) & 0xFFFF;

            c->pix[px] = v;
            c->pix_writes++;
            if (c->pix_write) {
                c->pix_write(c->opaque, px, v);
            }
        } else if (op == 0x4F) {                /* spri reg,S2 */
            unsigned px = S1;
            uint32_t v = read_reg(c, S2);

            c->pix[px] = v;
            c->pix_writes++;
            if (c->pix_write) {
                c->pix_write(c->opaque, px, v);
            }
        } else if (op == 0x5F) {                /* spr (S1),S2 */
            unsigned px = read_reg(c, S1) & 0xFF;
            uint32_t v = read_reg(c, S2);

            c->pix[px] = v;
            c->pix_writes++;
            if (c->pix_write) {
                c->pix_write(c->opaque, px, v);
            }
        } else if (!alu_x(c, op, D, S2, S1, imm, &r, &wd)) {
            halt(c, "undefined scalar ALU opcode", w);
            return false;
        } else if (wd) {
            write_reg(c, D, r);
        }
    } else if (op >= 0x60 && op <= 0x65) {      /* conditional branches */
        int32_t off = (int16_t)((w >> 8) & 0xFFFF);
        int32_t v = (int32_t)read_reg(c, S1);
        bool take;

        switch (op) {
        case 0x60: take = (v == 0); break;
        case 0x61: take = (v != 0); break;
        case 0x62: take = (v >= 0); break;
        case 0x63: take = (v < 0);  break;
        case 0x64: take = (v > 0);  break;
        default:   take = (v <= 0); break;
        }
        if (take) {
            c->branch_pending = true;
            c->branch_pc = c->pc + 4 + off * 4;
            branched = true;
        }
    } else if (op == 0x68 || op == 0x6A) {      /* rjmp, rjmpl */
        int32_t d = (int32_t)(w & 0xFFFFFF);

        if (d & 0x800000) {
            d -= 0x1000000;
        }
        if (op == 0x6A) {
            write_reg(c, V2200_RA, c->pc + 8);
        }
        c->branch_pending = true;
        c->branch_pc = c->pc + 4 + d * 4;
        branched = true;
    } else if (op == 0x6C || op == 0x6E) {      /* jmp, jmpl */
        if (op == 0x6E) {
            write_reg(c, V2200_RA, c->pc + 8);
        }
        c->branch_pending = true;
        c->branch_pc = (w & 0xFFFFFF) << 2;
        branched = true;
    } else if (op == 0x6B) {                    /* getpc - no delay slot */
        write_reg(c, D, c->pc + 8);
    } else if (op == 0x6D) {                    /* halt */
        halt(c, "halt instruction", w & 0xFFFFFF);
        return false;
    } else if (op == 0x6F) {                    /* jmprl D,(S1) */
        uint32_t t = read_reg(c, S1);

        write_reg(c, D, c->pc + 8);
        c->branch_pending = true;
        c->branch_pc = t;
        branched = true;
    } else if (op >= 0x70 && op <= 0x7F) {      /* loads and stores */
        static const uint8_t scale[16] = { 1, 2, 4, 8, 8, 8, 0, 0,
                                           1, 2, 4, 0, 8, 8, 8, 8 };
        unsigned k = op & 0xF;
        uint32_t v;

        switch (op) {
        case 0x76: write_reg(c, D, w & 0xFFFF); break;          /* li  */
        case 0x77: write_reg(c, D, (w & 0xFFFF) << 16); break;  /* lui */
        case 0x73:                                              /* pre */
            break;
        case 0x70: case 0x71: case 0x72: {
            uint32_t a = read_reg(c, S1) + S2 * scale[k];

            if (!ld(c, a, scale[k], &v)) {
                return false;
            }
            write_reg(c, D, v);
            break;
        }
        case 0x74: case 0x75: {                                 /* lv  */
            uint32_t a = read_reg(c, S1) + S2 * 8;
            uint32_t hi, lo;

            if (!ld(c, a, 4, &hi) || !ld(c, a + 4, 4, &lo)) {
                return false;
            }
            write_reg(c, D & ~1u, hi);
            write_reg(c, D | 1, lo);
            break;
        }
        case 0x78: case 0x79: case 0x7A: {
            uint32_t a = read_reg(c, S1) + D * scale[k];

            if (!st(c, a, scale[k], read_reg(c, S2))) {
                return false;
            }
            break;
        }
        case 0x7C: {                                            /* sv  */
            uint32_t a = read_reg(c, S1) + D * 8;

            if (!st(c, a, 4, read_reg(c, S2 & ~1u)) ||
                !st(c, a + 4, 4, read_reg(c, S2 | 1))) {
                return false;
            }
            break;
        }
        case 0x7D: case 0x7E: case 0x7F: {
            /*
             * sy, scr and scb scatter the four bytes of S2 across a
             * strided run: sy every two bytes from the base, scr every
             * four from base+1, scb every four from base+3.
             */
            uint32_t a = read_reg(c, S1) + D * 8;
            unsigned step = (op == 0x7D) ? 2 : 4;
            unsigned bias = (op == 0x7D) ? 0 : (op == 0x7E) ? 1 : 3;
            uint32_t s = read_reg(c, S2);
            unsigned i;

            for (i = 0; i < 4; i++) {
                if (!st(c, a + bias + i * step, 1,
                        (s >> ((3 - i) * 8)) & 0xFF)) {
                    return false;
                }
            }
            break;
        }
        default:
            halt(c, "undefined load/store opcode", w);
            return false;
        }
    } else if (op >= 0xC0 && op <= 0xC3) {      /* getxy/getyx/getra/getgb */
        uint32_t s1 = read_reg(c, S1);
        uint32_t even, odd;

        switch (op) {
        case 0xC0: even = s1 & 0xFFFF0000u; odd = s1 << 16; break;
        case 0xC1: even = s1 << 16; odd = s1 & 0xFFFF0000u; break;
        case 0xC2: even = s1 & 0xFF0000; odd = (s1 >> 8) & 0xFF0000; break;
        default:   even = (s1 << 8) & 0xFF0000;
                   odd  = (s1 << 16) & 0xFF0000; break;
        }
        write_reg(c, D & ~1u, even);
        write_reg(c, D | 1, odd);
    } else if (op == 0xC4) {                    /* sort S3,S2,S1 */
        uint32_t v[3] = { read_reg(c, D), read_reg(c, S2), read_reg(c, S1) };
        unsigned idx[3] = { D, S2, S1 };
        unsigned order[3] = { 2, 1, 0 };        /* smallest .. largest  */
        unsigned i, j, t;
        uint32_t seg;

        for (i = 0; i < 3; i++) {
            for (j = i + 1; j < 3; j++) {
                if ((int32_t)v[order[j]] < (int32_t)v[order[i]]) {
                    t = order[i]; order[i] = order[j]; order[j] = t;
                }
            }
        }
        seg = c->reg[V2200_R_SEG] & ~0x7777u;
        seg |= ((idx[order[0]] >> 4) & 7);
        seg |= ((idx[order[1]] >> 4) & 7) << 4;
        seg |= ((idx[order[2]] >> 4) & 7) << 8;
        c->reg[V2200_R_SEG] = seg | 0x8888;
        c->reg[V2200_R_CLIP] = (c->reg[V2200_R_CLIP] << 4) & 0xFC;
    } else if (op >= 0xD0 && op <= 0xDF) {      /* step / stepv */
        bool vec = op >= 0xD8;
        bool wd;

        engine_op(c, op, D, S2, S1);
        if (vec) {
            uint32_t ra = alu(c, 0, read_reg(c, S2 & ~1u),
                              read_reg(c, S1 & ~1u), &wd);
            uint32_t rb = alu(c, 0, read_reg(c, S2 | 1),
                              read_reg(c, S1 | 1), &wd);

            write_reg(c, D & ~1u, ra);
            write_reg(c, D | 1, rb);
        } else {
            write_reg(c, D, read_reg(c, S2) + read_reg(c, S1));
        }
    } else if ((op >= 0xE0 && op <= 0xE5) || (op >= 0xF0 && op <= 0xFE)) {
        engine_op(c, op, D, S2, S1);            /* draws */
    } else if (op >= 0xE7 && op <= 0xEF) {      /* the Fill Engine */
        uint32_t base = read_reg(c, S2 & ~1u);
        uint32_t delta = read_reg(c, S1);

        engine_op(c, op, D, S2, S1);
        write_reg(c, D, base + delta);          /* next scanline address */
    } else {
        halt(c, "undefined opcode", w);
        return false;
    }

    if (c->halted) {
        return false;
    }
    if (taken_pending) {
        if (branched) {
            halt(c, "branch in a branch delay slot", w);
            return false;
        }
        c->pc = taken_pc;
    } else {
        c->pc = next;
    }
    return true;
}
