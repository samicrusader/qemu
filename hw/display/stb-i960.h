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

/*
 * Process controls.  Priority is bits 20:16, and it is the whole of the
 * interrupt mask on this architecture - there is no separate disable bit.
 * An interrupt is taken only when its own priority exceeds the current
 * one, priority 31 being the exception that always lands.
 */
#define I960_PC_PRIO_SHIFT 16
#define I960_PC_PRIO_MASK  0x001f0000u
#define I960_PC_STATE      0x00002000u

static inline unsigned i960_pc_priority(uint32_t pc)
{
    return (pc & I960_PC_PRIO_MASK) >> I960_PC_PRIO_SHIFT;
}

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

/*
 * Interrupts.
 *
 * The table is NOT at address 0 - it is the one the board ROM owns, whose
 * address the firmware fetches from [[0xa02e8c70] + 0x10], so `intbase`
 * has to be set from that rather than assumed.  The entry for vector v is
 * at `intbase + (v + 1) * 4`, equivalently `+ 0x24 + (v - 8) * 4`.
 *
 * A Jx dedicated-mode source's vector is `(IMAP field << 4) | 2` - the low
 * nibble is fixed - and the priority is `vector >> 3`.  Six vectors in this
 * firmware fit it: 0x72, 0x82, 0xb2, 0xc2, 0xd2 (the timer, from
 * IMAP2 = 0x000d0000) and 0xe2.  A source's IPND/IMSK bit is a separate
 * number from its vector: the timer is bit 12 and vector 0xd2.
 *
 * Confusing the two is how this model spent a session believing the timer
 * was vector 12 at priority 1.  The firmware stores its handler both at
 * [table] + 0x34c and at absolute 0x34, and delivering vector 12 against a
 * table at 0 reads the latter - so it ran, and was wrong about the vector,
 * its priority and the table's location all at once.
 */

/*
 * Frames are aligned to SIXTEEN bytes, not to the 64 the register save
 * area occupies.  PFP's low four bits carry the return type (bits 2:0)
 * and the prereturn-trace flag (bit 3), which is exactly why sixteen is
 * the granularity.
 *
 * Getting this wrong is subtle and expensive: masking PFP with ~63
 * truncates any frame that is 16- but not 64-aligned, and both the
 * firmware's init (FP = 0xa02f6350) and the frames Nucleus PLUS builds
 * by hand (0xa0351190) are exactly that.  Every such ret then restored
 * the wrong 64 bytes.
 */
#define I960_FRAME_ALIGN 16
#define I960_NFRAMES     8      /* on-chip local register sets */

/*
 * RIP (r2) belongs to the CALLER.  `call` writes the return address into
 * the register set that is current when it executes, that set is then
 * cached and spilled with the return address in it, and `ret` restores
 * the previous set and only then takes IP from its r2.
 *
 * The opposite arrangement - return address to the callee, IP from r2
 * before the restore - is self-consistent for ordinary calls and wrong by
 * exactly one frame the moment software builds or edits a frame by hand.
 * This firmware does that twice: TCT_Build_Task_Stack puts a task's entry
 * point in the inner save area's RIP, and TCT_Control_To_Thread patches
 * [PFP+8] before returning into a suspended task.  Both are the i960's
 * documented idiom for a context switch, and both need this.
 */

typedef struct {
    uint32_t fp;                /* the frame these locals belong to */
    uint32_t reg[16];
    /*
     * Interrupt state, when this set was pushed by an interrupt rather
     * than a call.  It lives here instead of in guest memory because
     * nothing in the firmware reads it - the handler takes the
     * interrupted address from RIP - and the architectural record would
     * sit at newfp-16, which overlaps the previous frame's save area and
     * corrupted a PFP into a PC value when it was written there.
     */
    uint32_t saved_ip, saved_ac, saved_pc;
    bool     is_irq;
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

    /*
     * How often a ret had to read its register set back from memory
     * instead of popping the cache.  On real hardware that happens only
     * after the cache overflows; here it is also what a desynchronised
     * frame chain looks like, so it is counted rather than silent.
     */
    uint64_t    mem_restores;
    uint64_t    tag_mismatches;
    uint32_t    last_mem_restore_ip, last_mem_restore_fp;
    uint32_t    last_mem_restore_tag;
    unsigned    last_mem_restore_depth;

    /*
     * A ring of recent frame transitions.  Frame bugs are only ever
     * diagnosed backwards - the ret that fails is rarely the one that
     * did the damage - so each call, ret and interrupt records what it
     * did to the chain.
     */
#define I960_FRAMELOG 24
    struct {
        uint32_t ip, pfp, rip, newfp;
        unsigned depth;
        char     kind;          /* 'c' call, 'r' ret, 'i' irq, 'R' irq-ret */
        bool     from_cache;
    } flog[I960_FRAMELOG];
    unsigned    flog_n;

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
 * Deliver an interrupt, or say why not.  Masked and unhandled are
 * genuinely different answers - masked means try again once the priority
 * drops, unhandled means the firmware never installed anything - and
 * collapsing them into one false makes a routine mask look like a fault.
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
typedef enum {
    I960_IRQ_DELIVERED = 0,
    I960_IRQ_MASKED,        /* priority too low - the caller holds it pending */
    I960_IRQ_NO_HANDLER,    /* the interrupt table entry is zero */
    I960_IRQ_FAULT,         /* a memory access during delivery failed */
} I960IrqResult;

I960IrqResult i960_interrupt(I960Core *c, unsigned vector);

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
