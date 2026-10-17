/*
 * Rendition Verite V2200 internal RISC interpreter, for the STB Tulsa
 * device model.
 *
 * The Tulsa carries two V2200s - one driving the RGB framebuffer, one the
 * YUV - and each runs a 16 KB microcode program uploaded by the host as a
 * big-endian ELF with machine type 0x3d32.  Like the i960 beside them they
 * are peripheral processors, so this is a plain C interpreter rather than a
 * QEMU target.
 *
 * Decode and semantics come from Rendition's own "V2200 Integrated
 * 3D/2D/Video Accelerator - Technical Specifications (including RISC
 * PROGRAMMING)", 1159-001 ECN#0058 RevB 1/98, sections 2 and 3, and are
 * cross-checked against Sarah Walker's veritas disassembler, which agrees
 * with that document on every opcode and register name both of them define.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef STB_V2200_H
#define STB_V2200_H

#include <stdint.h>
#include <stdbool.h>

/*
 * The register file is 256 entries.  %0..%63 are special: most are
 * hardwired constants, which is how a machine whose widest immediate is
 * sixteen bits avoids ever materialising a mask.  %64..%127 are the
 * indirect windows onto %128..%255 through the four Seg pointers, and
 * %128..%255 are general purpose, with the top four architectural.
 */
#define V2200_NREG   256
#define V2200_SP     252
#define V2200_IRA    253
#define V2200_RA     254
#define V2200_FP     255

/* Special registers that are not constants (spec table 2.2b). */
#define V2200_R_SWFIFO    32
#define V2200_R_FIFO      33
#define V2200_R_SEED      34
#define V2200_R_SEG       35
#define V2200_R_SCALE     36
#define V2200_R_FLAG      37
#define V2200_R_CLIP      38
#define V2200_R_TIMER     39
#define V2200_R_DRAWCTL   40   /* write; npswfifo on read                 */
#define V2200_R_NPFIFO    41
#define V2200_R_WINCLIP   46
#define V2200_R_RISCINTR  47

/* Flag register bits (spec table 3.2). */
#define V2200_FLAG_SIGNALSYS     (1u << 0)
#define V2200_FLAG_PIXELBUSY     (1u << 1)
#define V2200_FLAG_FIFOOUTRDY    (1u << 2)
#define V2200_FLAG_ICACHEOFF     (1u << 3)
#define V2200_FLAG_STATUS        (0xfu << 4)
#define V2200_FLAG_SYSSTATUS     (0xfu << 8)
#define V2200_FLAG_PICKVALID     (1u << 16)
#define V2200_FLAG_PICKHIT       (1u << 17)
#define V2200_FLAG_FIFOOUTFORCE  (1u << 30)
#define V2200_FLAG_FIFOINRDY     (1u << 31)

/* RISC interrupt register bits (spec table 3.6). */
#define V2200_INTR_VERT          (1u << 0)
#define V2200_INTR_XINTR         (1u << 1)
#define V2200_INTR_VINEVEN       (1u << 2)
#define V2200_INTR_VINODD        (1u << 3)
#define V2200_INTR_WRITE0        (1u << 4)
#define V2200_INTR_WRITE1        (1u << 5)
#define V2200_INTR_VINODDLAST    (1u << 14)
#define V2200_INTR_WRITE1LAST    (1u << 15)
#define V2200_INTR_ENABLE_SHIFT  16
#define V2200_INTR_RISCINTR      (1u << 31)

typedef bool (*V2200ReadFn)(void *opaque, uint32_t addr, void *buf,
                            unsigned len);
typedef bool (*V2200WriteFn)(void *opaque, uint32_t addr, const void *buf,
                             unsigned len);

/*
 * The FIFOs are the host's side of the conversation: the driver pushes a
 * command word at a time into the input FIFO and reads results from the
 * output FIFO.  The core calls out for both rather than modelling them,
 * because on the real part they are PCI-visible registers.
 *
 * fifo_in returns false when the input FIFO is empty; the core then stalls
 * exactly as the hardware does.  fifo_out returns false when the output
 * FIFO is full.
 */
typedef bool (*V2200FifoInFn)(void *opaque, uint32_t *out);
typedef bool (*V2200FifoOutFn)(void *opaque, uint32_t v);
typedef bool (*V2200FifoPeekFn)(void *opaque, uint32_t *out);

/* A pixel-engine register write, handed out rather than acted on here. */
typedef void (*V2200PixFn)(void *opaque, unsigned index, uint32_t value);
/* A draw, step or fill instruction, likewise. */
typedef void (*V2200EngineFn)(void *opaque, unsigned op, unsigned d,
                              unsigned s2, unsigned s1);

typedef struct V2200Core {
    uint32_t reg[V2200_NREG];
    uint32_t pc;
    uint32_t pix[256];          /* last value written to each pixel reg   */

    /*
     * Branches are delayed and non-annulling: the instruction after the
     * branch always runs, and the target is relative to that delay slot.
     * branch_pc holds where to go once the delay slot has retired.
     */
    bool     branch_pending;
    uint32_t branch_pc;

    uint64_t icount;
    uint64_t stalls;            /* cycles spent waiting on a FIFO          */
    uint64_t pix_writes;
    uint64_t engine_ops;

    V2200ReadFn     read;
    V2200WriteFn    write;
    V2200FifoInFn   fifo_in;
    V2200FifoPeekFn fifo_peek;
    V2200FifoOutFn  fifo_out;
    V2200PixFn      pix_write;
    V2200EngineFn   engine;
    void           *opaque;

    const char *halt_reason;
    uint32_t    halt_pc;
    uint32_t    halt_word;
    bool        halted;
} V2200Core;

void v2200_reset(V2200Core *c, uint32_t entry);
bool v2200_step(V2200Core *c);
uint32_t v2200_reg_read(V2200Core *c, unsigned index);
void v2200_reg_write(V2200Core *c, unsigned index, uint32_t v);
const char *v2200_reg_name(unsigned index);

#endif /* STB_V2200_H */
