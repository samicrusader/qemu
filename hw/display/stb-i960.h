/*
 * Intel i960 (Jx core) interpreter for the STB Tulsa device model.
 *
 * The i960RP on the Tulsa is a *peripheral* processor, not the machine's
 * CPU, so this is a plain C interpreter rather than a QEMU target: no TCG,
 * no target/ directory, no new architecture.  It is stepped from the device
 * model and talks to board memory through a read/write callback pair.
 *
 * Decode is validated against IDA's disassembly of the shipped firmware
 * (fw_ida/STBT960_v4.elf, entry 0xa02e8c80).  Unknown encodings are
 * reported, never guessed - a silently mis-decoded instruction would
 * invalidate every trace taken after it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef STB_I960_H
#define STB_I960_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Registers                                                          */
/*                                                                    */
/* Encodings name 32 registers: 0-15 are the local set r0-r15, 16-31  */
/* the globals g0-g15.  Four of each set are architectural:           */
/*   r0 = PFP (previous frame pointer)   r1 = SP                      */
/*   r2 = RIP (return instruction ptr)   g15 = FP                     */
/* ------------------------------------------------------------------ */
#define I960_R0   0
#define I960_SP   1
#define I960_RIP  2
#define I960_G0   16
#define I960_FP   31

#define I960_NREG 32

/* Arithmetic-controls condition code, AC bits 2:0. */
#define I960_CC_MASK      0x7
#define I960_CC_FALSE     0x0
#define I960_CC_GREATER   0x1
#define I960_CC_EQUAL     0x2
#define I960_CC_LESS      0x4

typedef enum {
    I960_FMT_UNKNOWN = 0,
    I960_FMT_REG,          /* opcode 0x58..0x7f, 12-bit opcode   */
    I960_FMT_MEM,          /* opcode 0x80..0xcf, MEMA or MEMB    */
    I960_FMT_CTRL,         /* opcode 0x08..0x1f, 22-bit displ    */
    I960_FMT_COBR,         /* opcode 0x20..0x3f, 11-bit displ    */
} I960Format;

/* How a MEM instruction forms its effective address. */
typedef enum {
    I960_AM_OFFSET,            /* MEMA: exp                           */
    I960_AM_OFFSET_ABASE,      /* MEMA: exp(abase)                    */
    I960_AM_ABASE,             /* MEMB: (abase)                       */
    I960_AM_IP_DISP,           /* MEMB: disp(ip)                      */
    I960_AM_ABASE_INDEX,       /* MEMB: (abase)[index*scale]          */
    I960_AM_DISP,              /* MEMB: disp                          */
    I960_AM_DISP_ABASE,        /* MEMB: disp(abase)                   */
    I960_AM_DISP_INDEX,        /* MEMB: disp[index*scale]             */
    I960_AM_DISP_ABASE_INDEX,  /* MEMB: disp(abase)[index*scale]      */
    I960_AM_RESERVED,
} I960AddrMode;

typedef struct {
    uint32_t     ip;          /* address this instruction was fetched from */
    uint32_t     word0;       /* first word                                */
    uint32_t     word1;       /* MEMB displacement word, if len == 8       */
    unsigned     len;         /* 4 or 8                                    */
    I960Format   fmt;
    uint16_t     opcode;      /* 8-bit for MEM/CTRL/COBR, 12-bit for REG   */
    const char  *mnemonic;    /* NULL when the opcode is not in the table  */

    /* REG */
    uint8_t      srcdst, src1, src2;
    bool         src1_lit, src2_lit, dst_lit;

    /* MEM */
    I960AddrMode am;
    uint8_t      abase, index, scale;
    int32_t      disp;        /* MEMA offset or MEMB displacement word     */

    /* CTRL / COBR */
    int32_t      displacement;  /* already shifted left 2, sign extended   */
    uint32_t     target;        /* ip + displacement                       */
    bool         prediction;    /* COBR/CTRL bit 1: branch prediction hint */
} I960Insn;

/* ------------------------------------------------------------------ */
/* Core                                                               */
/* ------------------------------------------------------------------ */

struct I960Core;

/*
 * Board memory access.  Returns false on a fault; the interpreter stops
 * and records it rather than inventing a value, because on this board a
 * bad address means the model is missing a device, not that the firmware
 * is wrong.
 */
typedef bool (*I960ReadFn)(void *opaque, uint32_t addr, void *buf, unsigned len);
typedef bool (*I960WriteFn)(void *opaque, uint32_t addr, const void *buf, unsigned len);

/*
 * Supervisor call.  `calls` enters supervisor mode through the system
 * procedure table, and on this board that table is built by the board ROM
 * - a ceramic PLCC that has never been dumped.
 *
 * Rather than invent a table, the core hands the service index out here.
 * The firmware only has four `calls` sites in 122 KB (indices 243, 245,
 * 246 and 249), three of them identical trampolines, so answering four
 * services is a far smaller problem than reproducing a ROM.  This is
 * deliberately high-level emulation of the ROM sitting underneath
 * low-level emulation of the firmware, and the seam is here so that it is
 * obvious rather than buried.
 *
 * Return false for an index you do not answer: the core then stops rather
 * than letting the firmware run on with a result it never received.
 */
typedef bool (*I960SyscallFn)(void *opaque, struct I960Core *c, unsigned index);

#define I960_FRAME_ALIGN 64
#define I960_NFRAMES     8      /* on-chip local register sets */

typedef struct {
    uint32_t fp;                /* the frame these locals belong to */
    uint32_t reg[16];
} I960RegSet;

typedef struct I960Core {
    uint32_t    reg[I960_NREG];
    uint32_t    ip;
    uint32_t    ac;           /* arithmetic controls, cc in bits 2:0 */
    uint32_t    pc;           /* process controls                    */
    uint32_t    tc;           /* trace controls                      */

    I960ReadFn    read;
    I960WriteFn   write;
    I960SyscallFn syscall;
    void         *opaque;

    /* Stop reason, set when step() returns false. */
    const char *halt_reason;
    uint32_t    halt_ip;
    uint32_t    halt_word;

    /* On-chip local register sets, innermost last. */
    I960RegSet  cache[I960_NFRAMES];
    unsigned    depth;

    /*
     * Interrupt table base.  Normally taken from the PRCB, which the
     * board ROM builds; this firmware puts its table at 0 - it writes a
     * handler to absolute 0x34, which is entry 0x24 + (12-8)*4, i.e.
     * vector 12, and then enables bit 12 of the mask register.
     */
    uint32_t    intbase;
    bool        irq_enabled;

    uint64_t    icount;
} I960Core;

void     i960_reset(I960Core *c, uint32_t entry);

/*
 * Decode one instruction at `addr`.  Reads 4 bytes, and a further 4 only
 * when the first word says MEMB-with-displacement, so it never over-reads
 * past the end of a mapping.
 */
bool     i960_decode(I960Core *c, uint32_t addr, I960Insn *out);

/* Render a decoded instruction in IDA's syntax, for trace comparison. */
int      i960_format(const I960Insn *in, char *buf, size_t buflen);

/* Decode without a core, straight out of a buffer - used by the harness. */
bool     i960_decode_buf(const uint8_t *p, size_t avail, uint32_t addr,
                         I960Insn *out);

const char *i960_reg_name(unsigned r);

/* ------------------------------------------------------------------ */
/* Execution                                                          */
/* ------------------------------------------------------------------ */

/*
 * Execute one instruction.  Returns false and fills in halt_reason when
 * the core cannot continue: an unimplemented opcode, a memory access the
 * board model refused, or a reserved encoding.  It never guesses - a core
 * that invents a result keeps running and produces a trace that looks
 * plausible and is wrong, which is worse than stopping.
 */
bool i960_step(I960Core *c);

/*
 * Deliver an interrupt.  Returns false if the vector has no handler, so
 * a missing entry is a visible event rather than a jump to zero.
 *
 * The frame goes on the current stack rather than a separate interrupt
 * stack: the i960 only switches stacks when the interrupt arrives in user
 * mode, and this firmware runs supervisor throughout.
 *
 * The interrupt record's exact layout is not documented here and nothing
 * in this firmware reads it - the handler at 0xa02e8e6c takes the
 * interrupted address from RIP (`mov rip, r5`), not from the record.  So
 * RIP is what matters and is set faithfully; the record is written in a
 * self-consistent layout purely so the matching ret can undo it.
 */
bool i960_interrupt(I960Core *c, unsigned vector);

/*
 * Register windows.
 *
 * An earlier version of this always spilled - `call` wrote r0-r15 to the
 * current frame, `ret` read them back - on the argument that it was
 * observationally identical for software that does not inspect the cache.
 * That argument is wrong for this firmware, and the firmware says so:
 * eleven instructions in it assign g15 (FP) directly, clustered in the
 * Nucleus PLUS scheduler around 0xa02e98xx.
 *
 * Reassigning FP does not invalidate the on-chip sets.  A real `ret`
 * restores the most recently pushed set from the cache and takes the new
 * FP from PFP; it does not re-read memory at wherever FP now points.  The
 * spill-only model read whichever bytes happened to be at the new FP -
 * seeded image data - and the corruption surfaced as a bogus PFP about
 * 200,000 instructions later, nowhere near its cause.
 *
 * So the cache is modelled properly: a LIFO of local register sets,
 * spilled to their own frames only on overflow, and `flushreg` means
 * something again.
 */

#endif /* STB_I960_H */
