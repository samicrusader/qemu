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

static unsigned fmt_bpp(unsigned fmt);

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
    unsigned sfmt = p->reg[PIX_SRCMODE] & 0xF;

    return p->reg[PIX_SRCBASE] +
           stride_term(v, s & 7, (s >> 4) & 7) +
           u * (sfmt ? fmt_bpp(sfmt) : v2200_pix_bpp(p));
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
 * Pixel formats, spec table 4.5.  Everything inside the engine is 8888
 * ARGB and the format conversion happens at the edges, which is what lets
 * dither, fog and blending see real colour components.  A format that
 * round-trips (332 to 332, 565 to 565) is bit-exact through here, so the
 * plain BitBlt path is unaffected.
 */
static unsigned fmt_bpp(unsigned fmt)
{
    switch (fmt & 0xF) {
    case 1: case 2: case 3: return 1;
    case 4: case 5: case 6: case 7: return 2;
    case 0xC: case 0xD: return 4;
    default: return 1;
    }
}

static uint32_t unpack(unsigned fmt, uint32_t v)
{
    unsigned r, g, b, a = 255;

    switch (fmt & 0xF) {
    case 1:                                     /* 8-bit 332        */
        r = ((v >> 5) & 7) * 255 / 7;
        g = ((v >> 2) & 7) * 255 / 7;
        b = (v & 3) * 255 / 3;
        break;
    case 2:                                     /* 8-bit intensity  */
        r = g = b = v & 0xFF;
        break;
    case 3:                                     /* 8-bit alpha      */
        r = g = b = 255;
        a = v & 0xFF;
        break;
    case 4:                                     /* 16-bit 565       */
        r = ((v >> 11) & 0x1F) * 255 / 31;
        g = ((v >> 5) & 0x3F) * 255 / 63;
        b = (v & 0x1F) * 255 / 31;
        break;
    case 5:                                     /* 16-bit 4444      */
        a = ((v >> 12) & 0xF) * 17;
        r = ((v >> 8) & 0xF) * 17;
        g = ((v >> 4) & 0xF) * 17;
        b = (v & 0xF) * 17;
        break;
    case 6:                                     /* 16-bit 1555      */
        a = (v & 0x8000) ? 255 : 0;
        r = ((v >> 10) & 0x1F) * 255 / 31;
        g = ((v >> 5) & 0x1F) * 255 / 31;
        b = (v & 0x1F) * 255 / 31;
        break;
    default:                                    /* 32-bit 8888      */
        return v;
    }
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | b;
}

/*
 * The 4x4 perfect-magic-square dither of spec 4.8.  DitherOffset carries
 * two-bit X and Y offsets (b1:b0 and b5:b4) that are added to the low two
 * bits of the integer coordinates, so a dithered image is bit-identical
 * wherever the window sits.
 */
static const uint8_t dither_matrix[4][4] = {
    {  0, 12,  3, 15 },
    {  7, 11,  4,  8 },
    { 13,  1, 14,  2 },
    { 10,  6,  9,  5 },
};

static unsigned dither_bias(const V2200Pix *p, uint32_t x, uint32_t y,
                            unsigned keep)
{
    uint32_t off = p->reg[PIX_DITHEROFF];
    unsigned dx = (x + (off & 3)) & 3;
    unsigned dy = (y + ((off >> 4) & 3)) & 3;
    unsigned drop = 8 - keep;

    if (!(p->reg[PIX_DSTMODE] & DSTMODE_DITHER) || drop == 0 || drop > 8) {
        return 0;
    }
    /* Spread the 4-bit matrix value across the bits being discarded. */
    return ((unsigned)dither_matrix[dy][dx] << drop) >> 4;
}

static unsigned clamp8(unsigned v)
{
    return v > 255 ? 255 : v;
}

static uint32_t pack(const V2200Pix *p, unsigned fmt, uint32_t argb,
                     uint32_t x, uint32_t y)
{
    unsigned a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF;
    unsigned g = (argb >> 8) & 0xFF, b = argb & 0xFF;

    switch (fmt & 0xF) {
    case 1:
        r = clamp8(r + dither_bias(p, x, y, 3));
        g = clamp8(g + dither_bias(p, x, y, 3));
        b = clamp8(b + dither_bias(p, x, y, 2));
        return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);
    case 2:
        return (r * 77 + g * 151 + b * 28) >> 8;
    case 3:
        return a;
    case 4:
        r = clamp8(r + dither_bias(p, x, y, 5));
        g = clamp8(g + dither_bias(p, x, y, 6));
        b = clamp8(b + dither_bias(p, x, y, 5));
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    case 5:
        return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) |
               (b >> 4);
    case 6:
        return (a >= 128 ? 0x8000u : 0) | ((r >> 3) << 10) |
               ((g >> 3) << 5) | (b >> 3);
    default:
        return argb;
    }
}

/*
 * Fog, spec 4.11: NewColor = f * (SrcColor - FogColor) + FogColor, with
 * f = 0 full fog and f = 255 none.
 */
static uint32_t apply_fog(const V2200Pix *p, uint32_t argb)
{
    uint32_t fc = p->reg[PIX_FOGCOLOR];
    unsigned f = p->fog & 0xFF;
    unsigned i, out = argb & 0xFF000000u;

    if (!(p->reg[PIX_DSTMODE] & DSTMODE_FOG)) {
        return argb;
    }
    for (i = 0; i < 3; i++) {
        int sc = (int)((argb >> (i * 8)) & 0xFF);
        int fo = (int)((fc >> (i * 8)) & 0xFF);
        int nv = (f * (sc - fo)) / 255 + fo;

        if (nv < 0) {
            nv = 0;
        }
        if (nv > 255) {
            nv = 255;
        }
        out |= (uint32_t)nv << (i * 8);
    }
    return out;
}

/*
 * The Z buffer, spec 4.4.  ZBufMode picks the comparison, ZBufWrMode says
 * whether a passing pixel writes it back, and the buffer is 16-bit,
 * half-word aligned, addressed with the Stride register's own ZStride
 * pair.  Nothing in the Tulsa microcode programs ZBase or ZStride, so
 * this path is written from the specification and is not exercised by
 * anything on this card - it is here so that it exists, and it is
 * deliberately inert until a caller sets ZBufMode to something other
 * than "replace always".
 */
static bool z_test(V2200Pix *p, uint32_t x, uint32_t y)
{
    unsigned mode = (p->reg[PIX_DSTMODE] & DSTMODE_ZBUFMODE) >> 12;
    uint32_t s = p->reg[PIX_STRIDE];
    uint32_t addr, oldz, newz = (p->z >> 11) & 0xFFFF;
    bool pass;

    if (mode == 0) {
        return true;                            /* no Z read at all */
    }
    if (mode == 7) {
        return false;                           /* replace never    */
    }
    addr = p->reg[PIX_ZBASE] +
           stride_term(y, (s >> 16) & 7, (s >> 20) & 7) + x * 2;
    oldz = rd(p, addr, 2);
    switch (mode) {
    case 1: pass = newz <  oldz; break;
    case 2: pass = newz == oldz; break;
    case 3: pass = newz <= oldz; break;
    case 4: pass = newz >  oldz; break;
    case 6: pass = newz >= oldz; break;
    default: pass = true; break;
    }
    if (!pass) {
        p->zrejects++;
        return false;
    }
    if (p->reg[PIX_DSTMODE] & DSTMODE_ZBUFWR) {
        wr(p, addr, 2, newz);
    }
    return true;
}

/*
 * One pixel.  PMask is the plane mask and the ALU mode comes from
 * DstMode; scissoring uses ScissorX/ScissorY, which the microcode
 * programs from CMD_SETCLIPPING.
 */
static void put_pixel(V2200Pix *p, uint32_t x, uint32_t y, uint32_t argb)
{
    unsigned fmt = p->reg[PIX_DSTFMT] & 0xF;
    unsigned bpp = v2200_pix_bpp(p);
    uint32_t sx = p->reg[PIX_SCISSORX], sy = p->reg[PIX_SCISSORY];
    uint32_t a, d, r, mask, src;

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
    if (!z_test(p, x, y)) {
        return;
    }
    src = pack(p, fmt, apply_fog(p, argb), x, y);
    a = dst_addr(p, x, y);
    d = (p->reg[PIX_DSTMODE] & DSTMODE_DSTRDDIS)
      ? pack(p, fmt, p->reg[PIX_DSTCOLOR], x, y)
      : rd(p, a, bpp);
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

/*
 * SrcMask wraps the texture, SwapUV exchanges the two coordinates, and
 * SrcFmt - SrcMode b3:b0 - says how to unpack what comes back.  The
 * result is 8888 ARGB like everything else inside the engine.
 */
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
    {
        unsigned sfmt = p->reg[PIX_SRCMODE] & 0xF;
        unsigned sbpp = sfmt ? fmt_bpp(sfmt) : bpp;

        return unpack(sfmt ? sfmt : (p->reg[PIX_DSTFMT] & 0xF),
                      rd(p, src_addr(p, ui, vi), sbpp));
    }
}

/*
 * Whether a draw takes its colour from a source surface is SrcFunc, bits
 * 7:4 of SrcMode (spr 49) - 1 is Replace, the BitBlt case - and not
 * SrcFmt in bits 3:0.  Setup leaves SrcMode at 0x0010, so testing the low
 * nibble makes every blit draw FGColor instead of copying, which looks
 * exactly like a correct blit of the wrong data.
 */
static bool src_in_use(const V2200Pix *p)
{
    return ((p->reg[PIX_SRCMODE] >> 4) & 0xF) != 0;
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
    p->pixels = p->spans = p->fills = p->tris = p->zrejects = 0;
    p->fog = 255;
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
    case 0xD6:                                  /* step z / step a     */
        if (s2 & 1) {
            p->reg[PIX_FGCOLOR] = (p->reg[PIX_FGCOLOR] & 0x00FFFFFFu) |
                                  ((s2b >> 16) << 24);
        } else {
            p->z = s2a;
        }
        return;
    case 0xD4:                                  /* step f              */
        p->fog = (s2b >> 16) & 0xFF;
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
        /*
         * "Explicit drawing parameters are written to the Pixel Engine as
         * they are read into S2" - so a parallel draw carries U in the odd
         * half of its S2 pair rather than needing a step u before it,
         * which is why the microcode's blit loop issues step v and step x
         * but never a step u.
         */
        p->u = s2b;
        span(p, src_in_use(p), true);
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
        span(p, src_in_use(p), false);
        return;
    }

    /* ---- the Triangle Engine (spec 2.10) ------------------------- */
    case 0xFE: {
        /*
         * tri takes no operands worth the name: the Triangle Engine reads
         * a fixed block of the register file (spec table 2.10d), %128 for
         * the start values and %144/%160 for the two gradient banks, which
         * is why the microcode issues `tri a12, %zero, %zero`.  DrawCtl
         * b3:0 = 0xB enables the shadowing that keeps those registers and
         * the engine in step; b4 to b11 say which parameters iterate.
         */
        const uint32_t *r = c->reg;
        int32_t xtop = (int32_t)r[128], ytop = (int32_t)r[129];
        int32_t xmid = (int32_t)r[144], ymid = (int32_t)r[145];
        int32_t ybot = (int32_t)r[161];
        int32_t dxlong = (int32_t)r[142];       /* a14 dXLong/dy        */
        int32_t dxtop  = (int32_t)r[158];       /* a30 dXTop/dy         */
        int32_t dxbot  = (int32_t)r[174];       /* q14 dXBot/dy         */
        uint32_t dx    = r[160];                /* q0, b31 = direction  */
        bool long_right = (dx & 0x80000000u) != 0;
        uint32_t ctl = c->reg[V2200_R_DRAWCTL];
        bool do_uv = (ctl & ((1u << 6) | (1u << 7))) != 0;
        bool do_z = (ctl & (1u << 4)) != 0;
        int32_t z = (int32_t)r[130], dzdx = (int32_t)r[146];
        int32_t dzdy = (int32_t)r[162];
        int32_t u = (int32_t)r[140], v = (int32_t)r[141];
        int32_t dudx = (int32_t)r[156], dvdx = (int32_t)r[157];
        int32_t dudy = (int32_t)r[172], dvdy = (int32_t)r[173];
        int32_t xa = xtop, xb = xtop;
        int32_t y, ylast = ybot >> 16;
        unsigned bpp = v2200_pix_bpp(p);
        bool textured = src_in_use(p);

        p->tris++;
        for (y = ytop >> 16; y < ylast && y - (ytop >> 16) < 4096; y++) {
            int32_t left = long_right ? xb : xa;
            int32_t right = long_right ? xa : xb;
            int32_t xi, x0 = left >> 16, x1 = right >> 16;
            int32_t su = u, sv = v, sz = z;

            if (y == (ymid >> 16)) {
                xb = xmid;
                right = long_right ? xa : xb;
                left = long_right ? xb : xa;
                x0 = left >> 16;
                x1 = right >> 16;
            }
            for (xi = x0; xi < x1 && xi - x0 < 8192; xi++) {
                uint32_t col = textured
                             ? texel(p, (uint32_t)su, (uint32_t)sv, bpp)
                             : p->reg[PIX_FGCOLOR];

                if (do_z) {
                    p->z = (uint32_t)sz;
                }
                put_pixel(p, (uint32_t)xi, (uint32_t)y, col);
                if (do_uv) {
                    su += dudx;
                    sv += dvdx;
                }
                if (do_z) {
                    sz += dzdx;
                }
            }
            xa += dxlong;
            xb += (y < (ymid >> 16)) ? dxtop : dxbot;
            if (do_uv) {
                u += dudy;
                v += dvdy;
            }
            if (do_z) {
                z += dzdy;
            }
        }
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
