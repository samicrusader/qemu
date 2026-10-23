/*
 * Sealevel SeaIO digital I/O - per-port character-device channels
 *
 * Each 8-bit port of a sealevel-* card can be attached to a chardev, so a
 * TCP socket, pipe or serial port carries that port's lines without going
 * through QMP.  The wire format is symmetric, so two cards in two VMs can be
 * wired straight to each other:
 *
 *   "xx\n"   one byte, 1-2 hex digits (optional 0x).  From the card: the
 *            value it drives on an output port, sent on every change and
 *            once on connect.  To the card: the levels on an input port.
 *   "+n\n"   set input bit n (0-7)      } typed by hand; a card never
 *   "-n\n"   clear input bit n          } sends these
 *   "?\n"    resend the current output value
 *
 * Bytes on the wire are XORed with 0xff in both directions when the
 * channel's invert property is set, for relays wired to pull a line low.
 * Input received while a port is an output is kept and takes effect when
 * the port turns back into an input.  Lines are held across disconnects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SEALEVEL_CHAN_H
#define HW_MISC_SEALEVEL_CHAN_H

#include "chardev/char-fe.h"

typedef struct SeaIOChanOps {
    /* value the card drives on @port, or -1 while the port is an input */
    int (*get_output)(void *opaque, unsigned port);
    /* current external levels on @port */
    uint8_t (*get_input)(void *opaque, unsigned port);
    /* new external levels on @port; NULL for an output-only port */
    void (*set_input)(void *opaque, unsigned port, uint8_t val);
} SeaIOChanOps;

typedef struct SeaIOChan {
    /* properties */
    CharFrontend chr;
    bool invert;

    /* runtime */
    const SeaIOChanOps *ops;
    void *opaque;
    const char *name;           /* for traces and logs, e.g. "8001.B" */
    unsigned port;
    int last_sent;              /* -1 = nothing sent since last change */
    char line[16];
    unsigned len;
} SeaIOChan;

void sealevel_chan_realize(SeaIOChan *ch, const SeaIOChanOps *ops,
                           void *opaque, unsigned port, const char *name);
/* call after anything that may change what get_output() returns */
void sealevel_chan_update(SeaIOChan *ch);

#endif
