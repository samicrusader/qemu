/*
 * Rendition Verite V2200 Pixel and Fill Engines.
 *
 * Spec chapter 4 for the register set, 2.6 to 2.11 for the instructions.
 * The RISC core hands every spr write and every step/draw/fill instruction
 * here; this is where they turn into pixels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef STB_V2200_PIX_H
#define STB_V2200_PIX_H

#include "stb-v2200.h"

/* Pixel Engine register indices that matter here (spec table 4). */
#define PIX_SRCBASE   0
#define PIX_SRCMODE   1
#define PIX_SRCMASK   2
#define PIX_STRIDE    3
#define PIX_DSTBASE   4
#define PIX_DSTMODE   5
#define PIX_DSTFMT    6
#define PIX_PMASK     7
#define PIX_PATTERN   8
#define PIX_IPAT     10
#define PIX_PATMODE  12
#define PIX_PATOFFSET 13
#define PIX_SCISSORX 14
#define PIX_SCISSORY 15
#define PIX_FGCOLOR  19
#define PIX_BGCOLOR  20
#define PIX_SYNC     31

typedef struct V2200Pix {
    uint32_t reg[256];          /* the raw spr values */

    /* Shadow state the step instructions maintain (spec 2.6). */
    uint32_t xshadow;           /* 16.16 */
    uint32_t y;                 /* 16.16 */
    uint32_t cnt;               /* pixels, from b31:b16 of step cnt */
    uint32_t u, v;              /* 16.16 texture coordinates */
    int      xdir, udir, vdir;

    V2200ReadFn  read;
    V2200WriteFn write;
    void        *opaque;

    uint64_t pixels;            /* how many have been written */
    uint64_t spans;
    uint64_t fills;
    uint64_t tris;
} V2200Pix;

void v2200_pix_reset(V2200Pix *p);
void v2200_pix_spr(V2200Pix *p, unsigned index, uint32_t value);
void v2200_pix_engine(V2200Pix *p, V2200Core *c, unsigned op, unsigned d,
                      unsigned s2, unsigned s1);
unsigned v2200_pix_bpp(const V2200Pix *p);

#endif
