/*
 * Rendition Verite V2200 Pixel and Fill Engines.  See stb-v2200-pix.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "stb-v2200-pix.h"

/*
 * Stride, spec 4.3.  Two shift distances are summed, and three of the
 * eight encodings are themselves a sum of two shifts - which is how a
 * power-of-two shifter reaches 800 and 1600 byte scanlines.
 *
 *   Address = Base + (Y << dist0) + (Y << dist1) + X * bytes_per_pixel
 */
static uint32_t stride_term(uint32_t y, unsigned v0, unsigned v1)
{
    static const int d0a[8] = { -1, 8, 9, 10, 2, 8, 9, 10 };
    static const int d0b[8] = { -1, -1, -1, -1, -1, 9, 6, 7 };
    static const int d1[8]  = { -1, 4, 5, 6, 7, 10, 11, 12 };
    uint32_t a = 0;

    if (d0a[v0] >= 0) {
        a += y << d0a[v0];
    }
    if (d0b[v0] >= 0) {
        a += y << d0b[v0];
    }
    if (d1[v1] >= 0) {
        a += y << d1[v1];
    }
    return a;
}

unsigned v2200_pix_bpp(const V2200Pix *p)
{
    switch (p->reg[PIX_DSTFMT] & 0xF) {
    case 1: case 2: case 3: return 1;
    case 4: case 5: case 6: case 7: return 2;
    case 0xC: case 0xD: return 4;
    default: return 1;
    }
}

static uint32_t dst_addr(const V2200Pix *p, uint32_t x, uint32_t y)
{
    uint32_t s = p->reg[PIX_STRIDE];

    return p->reg[PIX_DSTBASE] +
           stride_term(y, (s >> 8) & 7, (s >> 12) & 7) +
           x * v2200_pix_bpp(p);
}

static uint32_t src_addr(const V2200Pix *p, uint32_t u, uint32_t v)
{
    uint32_t s = p->reg[PIX_STRIDE];

    return p->reg[PIX_SRCBASE] +
           stride_term(v, s & 7, (s >> 4) & 7) +
           u * v2200_pix_bpp(p);
}

/* The sixteen raster ops of DstMode b3:b0 (spec 4.4). */
static uint32_t alu_mode(unsigned mode, uint32_t s, uint32_t d)
{
    switch (mode) {
    case 0x0: return 0;
    case 0x1: return ~(s | d);
    case 0x2: return ~s & d;
    case 0x3: return ~s;
    case 0x4: return s & ~d;
    case 0x5: return ~d;
    case 0x6: return s ^ d;
    case 0x7: return ~(s & d);
    case 0x8: return s & d;
    case 0x9: return ~(s ^ d);
    case 0xA: return d;
    case 0xB: return s | ~d;
    case 0xC: return s;                 /* Replace */
    case 0xD: return ~s | d;
    case 0xE: return s | d;
    default:  return 0xFFFFFFFFu;
    }
}

static uint32_t rd(V2200Pix *p, uint32_t a, unsigned n)
{
    uint8_t b[4];
    uint32_t v = 0;
    unsigned i;

    if (!p->read || !p->read(p->opaque, a, b, n)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        v = (v << 8) | b[i];
    }
    return v;
}

static void wr(V2200Pix *p, uint32_t a, unsigned n, uint32_t v)
{
    uint8_t b[4];
    unsigned i;

    for (i = 0; i < n; i++) {
        b[i] = (uint8_t)(v >> ((n - 1 - i) * 8));
    }
    if (p->write) {
        p->write(p->opaque, a, b, n);
    }
}

/*
 * One pixel.  PMask is the plane mask and the ALU mode comes from
 * DstMode; scissoring uses ScissorX/ScissorY, which the microcode
 * programs from CMD_SETCLIPPING.
 */
static void put_pixel(V2200Pix *p, uint32_t x, uint32_t y, uint32_t src)
{
    unsigned bpp = v2200_pix_bpp(p);
    uint32_t sx = p->reg[PIX_SCISSORX], sy = p->reg[PIX_SCISSORY];
    uint32_t a, d, r, mask;

    if (sx) {
        uint32_t l = (sx >> 16) & 0xFFF, rt = sx & 0xFFF;

        if (rt && (x < l || x >= rt)) {
            return;
        }
    }
    if (sy) {
        uint32_t t = (sy >> 16) & 0xFFF, b = sy & 0xFFF;

        if (b && (y < t || y >= b)) {
            return;
        }
    }
    a = dst_addr(p, x, y);
    d = rd(p, a, bpp);
    r = alu_mode(p->reg[PIX_DSTMODE] & 0xF, src, d);
    mask = p->reg[PIX_PMASK];
    if (bpp < 4) {
        mask &= (1u << (bpp * 8)) - 1;
    }
    if (mask) {
        r = (r & mask) | (d & ~mask);
    }
    wr(p, a, bpp, r);
    p->pixels++;
}

/*
 * The pattern register, spec 4.7.  The current bit is ~PatOffset & 0x1F of
 * Pat - bit 31 is the leftmost - and PatOffset advances after every pixel
 * even when the pixel is scissored away, wrapping at PatLengthM1.  A clear
 * bit selects the background, which is not drawn at all unless PatOpaque.
 */
#define PATMODE_ENABLE  (1u << 16)
#define PATMODE_OPAQUE  (1u << 17)

static bool pattern_pixel(V2200Pix *p, uint32_t *colour)
{
    uint32_t mode = p->reg[PIX_PATMODE];
    uint32_t off, bit;

    if (!(mode & PATMODE_ENABLE)) {
        return true;
    }
    off = p->reg[PIX_PATOFFSET] & 0x1F;
    bit = (p->reg[PIX_PATTERN] >> (~off & 0x1F)) & 1;
    p->reg[PIX_PATOFFSET] = (off != (mode & 0x1F)) ? off + 1 : 0;
    if (bit) {
        return true;
    }
    if (!(mode & PATMODE_OPAQUE)) {
        return false;                   /* transparent background */
    }
    *colour = p->reg[PIX_BGCOLOR];
    return true;
}

/* SrcMask wraps the texture; SwapUV exchanges the two coordinates. */
static uint32_t texel(V2200Pix *p, uint32_t u, uint32_t v, unsigned bpp)
{
    uint32_t mask = p->reg[PIX_SRCMASK];
    uint32_t umask = mask & 0x7FF, vmask = (mask >> 16) & 0x7FF;
    uint32_t ui = u >> 16, vi = v >> 16;

    if (p->reg[PIX_SRCMODE] & (1u << 10)) {     /* SwapUV */
        uint32_t t = ui; ui = vi; vi = t;
        t = umask; umask = vmask; vmask = t;
    }
    if (umask) {
        ui &= umask;
    }
    if (vmask) {
        vi &= vmask;
    }
    return rd(p, src_addr(p, ui, vi), bpp);
}

/* A run of Cnt pixels along a scanline, source either solid or a blit. */
static void span(V2200Pix *p, bool textured, bool keep_x)
{
    uint32_t x = p->xshadow >> 16;
    uint32_t y = p->y >> 16;
    uint32_t u = p->u, v = p->v;
    unsigned bpp = v2200_pix_bpp(p);
    uint32_t n = p->cnt;
    uint32_t i;

    p->spans++;
    for (i = 0; i < n && i < 8192; i++) {
        uint32_t s = textured ? texel(p, u, v, bpp) : p->reg[PIX_FGCOLOR];

        if (pattern_pixel(p, &s)) {
            put_pixel(p, x, y, s);
        }
        x += (uint32_t)p->xdir;
        u += (uint32_t)(p->xdir << 16);
    }
    /*
     * drawp and rdrawp advance XShadow and U; the resetting drawpxy and
     * rdrawpxy restart from the shadow every time, which is what makes a
     * rectangle out of one instruction in a delay slot.
     */
    if (keep_x) {
        p->xshadow = (p->xshadow & 0xFFFF) | (x << 16);
        p->u = u;
    }
    (void)v;
}

void v2200_pix_reset(V2200Pix *p)
{
    uint32_t **keep = NULL;

    (void)keep;
    memset(p->reg, 0, sizeof(p->reg));
    p->xshadow = p->y = p->u = p->v = 0;
    p->cnt = 0;
    p->xdir = 1;
    p->udir = p->vdir = 1;
    p->pixels = p->spans = p->fills = 0;
}

void v2200_pix_spr(V2200Pix *p, unsigned index, uint32_t value)
{
    p->reg[index & 0xFF] = value;
    /* Bit-field spr's fold into their parent register (spec table 4). */
    switch (index) {
    case 58: p->reg[PIX_STRIDE] = (p->reg[PIX_STRIDE] & ~0xFFu) |
                                  (value & 0xFF); break;
    case 59: p->reg[PIX_STRIDE] = (p->reg[PIX_STRIDE] & ~0xFF00u) |
                                  ((value & 0xFF) << 8); break;
    case 64: p->reg[PIX_DSTMODE] = (p->reg[PIX_DSTMODE] & ~0xFu) |
                                   (value & 0xF); break;
    default: break;
    }
}

void v2200_pix_engine(V2200Pix *p, V2200Core *c, unsigned op, unsigned d,
                      unsigned s2, unsigned s1)
{
    uint32_t s2a = v2200_src(c, s2 & ~1u), s2b = v2200_src(c, s2 | 1);
    uint32_t s1a = v2200_src(c, s1 & ~1u);

    switch (op) {
    /* ---- step: load one Pixel Engine interpolator (spec 2.6) ---- */
    case 0xD0:                                  /* step x / step y     */
        if (s2 & 1) {
            p->y = s2b;
        } else {
            p->xshadow = s2a;
            p->xdir = ((int32_t)s1a < 0) ? -1 : 1;
        }
        return;
    case 0xD2:                                  /* step r / step v     */
        if (s2 & 1) {
            p->v = s2b;
            p->vdir = ((int32_t)s1a < 0) ? -1 : 1;
        }
        return;
    case 0xD3:                                  /* step i / step u     */
        if (s2 & 1) {
            p->u = s2b;
            p->udir = ((int32_t)s1a < 0) ? -1 : 1;
        }
        return;
    case 0xD7:                                  /* step cnt            */
        p->cnt = s2a >> 16;
        return;
    case 0xD8:                                  /* stepv xy            */
        p->xshadow = s2a;
        p->y = s2b;
        p->xdir = ((int32_t)s1a < 0) ? -1 : 1;
        return;
    case 0xDF:                                  /* stepv cnt           */
        p->cnt = s2a >> 16;
        return;

    /* ---- parallel draws (spec 2.8) ------------------------------- */
    case 0xE0:                                  /* drawp               */
    case 0xE1:                                  /* rdrawp              */
        span(p, (p->reg[PIX_SRCMODE] & 0xF) != 0, true);
        return;
    case 0xE4:                                  /* drawpxy             */
    case 0xE5: {                                /* rdrawpxy            */
        /*
         * The resetting forms take their Y from the odd half of the S2
         * pair and restart the span at XShadow.  FillRectSolid's whole
         * body is one of these in a branch delay slot, with S1 = %allfs
         * so that the pair delta is (-1, +1.0) - the row counter and the
         * next scanline, straight out of the constant register file.
         */
        p->y = s2b;
        span(p, (p->reg[PIX_SRCMODE] & 0xF) != 0, false);
        return;
    }

    /* ---- the Fill Engine (spec 2.11) ----------------------------- */
    case 0xE8: {                                /* fill                */
        uint32_t addr = s2a, len = s2b;
        unsigned bpp = (p->reg[PIX_DSTFMT] & 0xF) >= 0xC ? 4
                     : (p->reg[PIX_DSTFMT] & 0xF) >= 4 ? 2 : 1;
        uint32_t colour = c->reg[130];          /* a2 */
        uint32_t i;

        p->fills++;
        for (i = 0; i < len && i < 8192; i++) {
            wr(p, addr + i * bpp, bpp, colour);
            p->pixels++;
        }
        return;
    }
    default:
        break;
    }
    (void)d;
}
