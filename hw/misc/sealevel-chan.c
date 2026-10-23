/*
 * Sealevel SeaIO digital I/O - per-port character-device channels
 *
 * See include/hw/misc/sealevel-chan.h for the wire format.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "hw/misc/sealevel-chan.h"
#include "trace.h"

static uint8_t wire(SeaIOChan *ch, uint8_t v)
{
    return ch->invert ? ~v : v;
}

static void chan_send(SeaIOChan *ch, int val)
{
    char buf[4];

    ch->last_sent = val;
    if (val < 0 || !qemu_chr_fe_backend_connected(&ch->chr)) {
        return;
    }
    snprintf(buf, sizeof(buf), "%02x\n", wire(ch, val));
    trace_sealevel_chan_tx(ch->name, wire(ch, val));
    /* three bytes, best effort: a reconnect resyncs anyway */
    qemu_chr_fe_write(&ch->chr, (const uint8_t *)buf, 3);
}

void sealevel_chan_update(SeaIOChan *ch)
{
    int val;

    if (!ch->ops) {
        return;
    }
    val = ch->ops->get_output(ch->opaque, ch->port);
    if (val != ch->last_sent) {
        chan_send(ch, val);
    }
}

static void chan_bad(SeaIOChan *ch, const char *why)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sealevel %s: %s: \"%s\"\n",
                  ch->name, why, ch->line);
}

static void chan_line(SeaIOChan *ch)
{
    const char *s = ch->line;
    uint64_t v;
    uint8_t cur;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return;
    }
    if (*s == '?') {
        chan_send(ch, ch->ops->get_output(ch->opaque, ch->port));
        return;
    }
    if (!ch->ops->set_input) {
        chan_bad(ch, "output-only port, input ignored");
        return;
    }

    cur = wire(ch, ch->ops->get_input(ch->opaque, ch->port));
    if (*s == '+' || *s == '-') {
        if (s[1] < '0' || s[1] > '7' || s[2]) {
            chan_bad(ch, "bit number must be 0-7");
            return;
        }
        if (*s == '+') {
            cur |= 1u << (s[1] - '0');
        } else {
            cur &= ~(1u << (s[1] - '0'));
        }
    } else {
        if (qemu_strtou64(s, NULL, 16, &v) < 0 || v > 0xff) {
            chan_bad(ch, "expected one hex byte");
            return;
        }
        cur = v;
    }
    trace_sealevel_chan_rx(ch->name, cur);
    ch->ops->set_input(ch->opaque, ch->port, wire(ch, cur));
}

static int chan_can_receive(void *opaque)
{
    return 64;
}

static void chan_receive(void *opaque, const uint8_t *buf, int size)
{
    SeaIOChan *ch = opaque;

    for (int i = 0; i < size; i++) {
        char c = buf[i];

        if (c == '\n' || c == '\r') {
            ch->line[ch->len] = '\0';
            chan_line(ch);
            ch->len = 0;
        } else if (ch->len < sizeof(ch->line) - 1) {
            ch->line[ch->len++] = c;
        } else {
            /* runaway line: drop it and resync at the next newline */
            ch->line[ch->len] = '\0';
            chan_bad(ch, "line too long");
            ch->len = 0;
        }
    }
}

static void chan_event(void *opaque, QEMUChrEvent event)
{
    SeaIOChan *ch = opaque;

    if (event == CHR_EVENT_OPENED) {
        trace_sealevel_chan_connect(ch->name);
        ch->len = 0;
        chan_send(ch, ch->ops->get_output(ch->opaque, ch->port));
    }
}

void sealevel_chan_realize(SeaIOChan *ch, const SeaIOChanOps *ops,
                           void *opaque, unsigned port, const char *name)
{
    ch->ops = ops;
    ch->opaque = opaque;
    ch->port = port;
    ch->name = name;
    ch->last_sent = -1;
    ch->len = 0;
    if (qemu_chr_fe_backend_connected(&ch->chr)) {
        qemu_chr_fe_set_handlers(&ch->chr, chan_can_receive, chan_receive,
                                 chan_event, NULL, ch, NULL, true);
    }
}
