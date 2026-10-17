/*
 * STB Systems "Tulsa" video/graphics card.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/range.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/sockets.h"
#include "io/net-listener.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/core/cpu.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "stb-tulsa.h"

/* ================================================================== *
 * Helpers
 * ================================================================== */

static const char *stb_mu_regname(hwaddr addr)
{
    switch (addr) {
    case STB_MU_IMR0: return "IMR0";
    case STB_MU_IMR1: return "IMR1";
    case STB_MU_OMR0: return "OMR0";
    case STB_MU_OMR1: return "OMR1";
    case STB_MU_IDR:  return "IDR";
    case STB_MU_IISR: return "IISR";
    case STB_MU_IIMR: return "IIMR";
    case STB_MU_ODR:  return "ODR";
    case STB_MU_OISR: return "OISR";
    case STB_MU_OIMR: return "OIMR";
    default:          return "?";
    }
}

static const char *stb_status_name(uint8_t st)
{
    switch (st) {
    case STB_BOARD_RESET:   return "reset";
    case STB_BOARD_INIT:    return "init";
    case STB_BOARD_RUNNING: return "running";
    case STB_BOARD_FAULT:   return "fault";
    default:                return "?";
    }
}

/*
 * Verite microcode entry points, recovered from the symbol table of
 * STBT2200.uc.  A display-list slot's first dword is a jump address into
 * this space, so naming them turns an otherwise opaque trace into something
 * readable.  .text runs 0x1000-0x5000; anything outside that is not a
 * handler address.
 */
static const struct {
    uint32_t addr;
    const char *name;
} stb_v2200_handlers[] = {
    { 0x2280, "FlickerFilter" },
    { 0x2df0, "OverlayInit" },
    { 0x2e2c, "InterruptHandler" },
    { 0x30a0, "VideoScaleHandler" },
    { 0x3124, "OverlayScaleYUVHandler" },
    { 0x3148, "OverlayScaleHandler" },
    { 0x3224, "VerticalScrollHandler" },
    { 0x3420, "HorzScrollHandler" },
    { 0x36e0, "DstCKHandler" },
    { 0x3764, "BeamInit" },
    { 0x3784, "BeamTest" },
    { 0x38a0, "VideoScaleDstCKYUVHandler" },
    { 0x38f8, "OverlayScaleDstCKYUVHandler" },
    { 0x391c, "OverlayScaleDstCKHandler" },
    { 0x39c0, "VideoScaleSrcCKYUVHandler" },
    { 0x3a18, "OverlayScaleSrcCKYUVHandler" },
    { 0x3a3c, "OverlayScaleSrcCKHandler" },
    { 0x3c3c, "SetFrameBase" },
    { 0x3cac, "FillHandler" },
    { 0x3e34, "CalcAveBurstLevel" },
    { 0x4360, "CopyHandler" },
    { 0x4440, "SrcCKHandler" },
};

static const char *stb_v2200_handler_name(uint32_t addr)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(stb_v2200_handlers); i++) {
        if (stb_v2200_handlers[i].addr == addr) {
            return stb_v2200_handlers[i].name;
        }
    }
    if (addr >= 0x1000 && addr < 0x5000) {
        return "<unnamed microcode entry>";
    }
    return "<not a handler address>";
}

/* ================================================================== *
 * ATU - i960RP Address Translation Unit, function 1
 * ================================================================== */

static void stb_atu_update_irq(STBTulsaATU *s)
{
    int level = (s->oisr & ~s->oimr) != 0;

    trace_stb_tulsa_irq(s->oisr, s->oimr, level);
    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * Publish TULSA_TEST_STATUS.
 *
 * The firmware runs the board diagnostics as part of its own start-up and
 * leaves the results here; nothing in this model executes that code, so the
 * results are posted directly.  All-pass is the honest answer for a board
 * whose peripherals are themselves models: there is no failure to report.
 * A real card can report individual failures, and the sample app's dialog
 * renders each one, so this is a place where a model and real hardware can
 * legitimately differ.
 */
static void stb_atu_publish_selftest(STBTulsaATU *s, bool finished)
{
    int i;

    stl_le_p(s->shm + STB_SHM_READY, finished ? 1 : 0);
    stl_le_p(s->shm + STB_SHM_TEST_ENCODER_ID,
             finished ? STB_TEST_ENCODER_ID : 0);

    for (i = 0; i < STB_SHM_TEST_COUNT; i++) {
        stl_le_p(s->shm + STB_SHM_TEST_FIRST + i * 4, finished ? 1 : 0);
    }
}

/*
 * The NTSC field clock.
 *
 * Section 8 flagged this years of notes ago: "GetBoardVertical has to be a
 * real 59.94 Hz counter because scroll timing and CallOnBoardVertical hang
 * off it."  Section 25 then measured a real card scrolling 1 pixel per 2
 * verticals, dead flat over 15 seconds -- so the card advances its own
 * scroll off this clock and the host only reads the count back.
 *
 * Period is 1001/60000 s exactly, not 1/60: NTSC's field rate is 59.94 Hz,
 * and a 1/60 clock would drift against anything the host times from it.
 */
/*
 * Rearm a field timer on an absolute schedule.
 *
 * "now + period" looks right and is not: the callback is entered late by
 * however long the last one took, and that lateness is folded into the next
 * deadline, so the clock runs slow by exactly the cost of its own work.  With
 * the display list replayed every field (four full-frame blits) that cost is
 * not small -- measured, the scroll ran 24.5 px/s where section 25's preset
 * demands 29.97.  Counting from the previous deadline makes the period exact
 * and the error non-cumulative; if the host falls so far behind that whole
 * fields are missed, resynchronise rather than queue a burst.
 */
static int64_t stb_field_next(int64_t *deadline)
{
    int64_t period = (int64_t)NANOSECONDS_PER_SECOND *
                     STB_FIELD_NUM / STB_FIELD_DEN;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    *deadline += period;
    if (*deadline <= now - period * 8) {
        *deadline = now + period;
    }
    return *deadline;
}

static void stb_atu_ring(STBTulsaATU *s, uint32_t bits);

static void stb_atu_field_tick(void *opaque)
{
    STBTulsaATU *s = opaque;

    s->vertical++;
    stl_le_p(s->shm + STB_SHM_VERTICAL, s->vertical);

    /*
     * Section 68.7.  ISTBTBoardControl::CallOnBoardVertical only records a
     * call type and an ISTBTVerticalCallback object on the host side - it
     * sends the board nothing - so the firmware cannot know whether anyone is
     * listening and rings the vertical bits on every field regardless.
     * STBTCtl's ODR decoder then decides, and calls VerticalOccurred() with
     * the count it reads back from SHM + 0x158.
     */
    if (s->vertical_doorbell) {
        stb_atu_ring(s, STB_ODR_VERTICAL | STB_ODR_FIELDTICK);
    }

    timer_mod(s->field_timer, stb_field_next(&s->field_deadline));
}

static void stb_atu_field_start(STBTulsaATU *s, bool run)
{
    if (!s->field_timer) {
        return;
    }
    if (run) {
        s->field_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(s->field_timer, stb_field_next(&s->field_deadline));
    } else {
        timer_del(s->field_timer);
        s->vertical = 0;
        stl_le_p(s->shm + STB_SHM_VERTICAL, 0);
    }
}

static void stb_atu_set_status(STBTulsaATU *s, STBBoardStatus st)
{
    if (s->status == st) {
        return;
    }
    trace_stb_tulsa_status(stb_status_name(s->status), stb_status_name(st));
    s->status = st;

    stb_atu_publish_selftest(s, st == STB_BOARD_RUNNING);
    stb_atu_field_start(s, st == STB_BOARD_RUNNING);
}

/* Post a reply to the host and raise the matching outbound interrupt. */
static void stb_atu_reply(STBTulsaATU *s, uint32_t omr0, uint32_t omr1)
{
    trace_stb_tulsa_reply(omr0, omr1);
    s->omr[0] = omr0;
    s->omr[1] = omr1;
    s->oisr |= STB_OISR_OMR0 | STB_OISR_OMR1;
    stb_atu_update_irq(s);
}


/*
 * The device-context commands.
 *
 * Section 77: the firmware handlers for 11-16, 18-20 and 23 all begin
 * `call sub_A02E4EF0`, which returns the constant 0xA00004C0 - the device
 * context - and then read their parameter out of it at a fixed offset.  The
 * mailbox carries nothing for them.  So handling one is reading the same
 * words the i960 would read and latching them; the board-side effects
 * (sub_A02F22A8, sub_A02F232C, sub_A02E4C9C and the sub_A02F1498 re-apply)
 * drive analog hardware the model does not have.
 */
static uint32_t stb_ctx_dword(STBTulsaATU *s, unsigned off)
{
    if (STB_SHM_DEVCTX + off + 4 > s->dram_size) {
        return 0;
    }
    return ldl_le_p(s->shm + STB_SHM_DEVCTX + off);
}

static void stb_ctx_scalar(STBTulsaATU *s, uint32_t cmd, unsigned off,
                           uint32_t *dst)
{
    *dst = stb_ctx_dword(s, off);
    trace_stb_tulsa_ctx_scalar(cmd, off, *dst);
}

static void stb_ctx_triple(STBTulsaATU *s, uint32_t cmd, unsigned off,
                           uint32_t dst[3])
{
    int i;

    for (i = 0; i < 3; i++) {
        dst[i] = stb_ctx_dword(s, off + 4 * i);
    }
    trace_stb_tulsa_ctx_triple(cmd, off, dst[0], dst[1], dst[2]);
}

/* True if `cmd` was fully handled here. */
static bool stb_atu_ctx_cmd(STBTulsaATU *s, uint32_t cmd)
{
    switch (cmd) {
    case 11:
        stb_ctx_scalar(s, cmd, STB_CTX_VIDEO_MODE, &s->ctx.video_mode);
        stb_ctx_scalar(s, cmd, STB_CTX_DISABLE_GENLOCK,
                       &s->ctx.disable_genlock);
        return true;
    case 12:    /* with the TMC scalar at cfg+0xc30, which nothing reads yet */
        stb_ctx_scalar(s, cmd, STB_CTX_VIDEO_MODE, &s->ctx.video_mode);
        return true;
    case 13:
    case 20:    /* shares 13's handler */
        stb_ctx_scalar(s, cmd, STB_CTX_VIDEO_MODE, &s->ctx.video_mode);
        stb_ctx_scalar(s, cmd, STB_CTX_GENLOCK_SOURCE,
                       &s->ctx.genlock_source);
        return true;
    case 14:
        stb_ctx_scalar(s, cmd, STB_CTX_AUDIO_DELAY, &s->ctx.audio_delay);
        return true;
    case 15:
        stb_ctx_scalar(s, cmd, STB_CTX_AUDIO_SOURCE, &s->ctx.audio_source);
        return true;
    case 16:
        stb_ctx_triple(s, cmd, STB_CTX_AUDIO_VOLUME, s->ctx.audio_volume);
        return true;
    case 17:    /* EnableBypass - the only one whose parameter is in `arg` */
        return false;
    case 18:    /* the host still sends it; this firmware ignores it */
        stb_ctx_scalar(s, cmd, STB_CTX_LINE21, &s->ctx.line21_passthru);
        return true;
    case 19:
        stb_ctx_triple(s, cmd, STB_CTX_VIDEO_SOURCE, s->ctx.video_source);
        return true;
    case 23:
        stb_ctx_triple(s, cmd, STB_CTX_AUDIO_PHASE,
                       (uint32_t *)s->ctx.audio_phase);
        return true;
    default:
        return false;
    }
}

static void stb_atu_initboard(STBTulsaATU *s, const uint8_t *payload)
{
    STBTulsaConfig cfg;
    int i;

    memcpy(&cfg, payload, sizeof(cfg));

    /* The block is little-endian on the wire, like the rest of the host ABI. */
    cfg.disk_buffer_count = le32_to_cpu(cfg.disk_buffer_count);
    cfg.disk_buffer_size  = le32_to_cpu(cfg.disk_buffer_size);
    cfg.audio_delay       = le32_to_cpu(cfg.audio_delay);
    cfg.audio_source      = le32_to_cpu(cfg.audio_source);
    cfg.line21_passthru   = le32_to_cpu(cfg.line21_passthru);
    cfg.video_mode        = le32_to_cpu(cfg.video_mode);
    cfg.genlock_source    = le32_to_cpu(cfg.genlock_source);
    cfg.acc_enable        = le32_to_cpu(cfg.acc_enable);
    cfg.thread_priority   = le32_to_cpu(cfg.thread_priority);
    for (i = 0; i < 3; i++) {
        cfg.audio_volume[i] = le32_to_cpu(cfg.audio_volume[i]);
        cfg.video_source[i] = le32_to_cpu(cfg.video_source[i]);
        cfg.audio_phase[i]  = le32_to_cpu(cfg.audio_phase[i]);
    }

    s->config = cfg;
    s->configured = true;

    trace_stb_tulsa_initboard(cfg.video_mode, cfg.audio_source,
                              cfg.genlock_source, cfg.audio_delay);

    /*
     * Real InitBoard resets, programs and starts the card, and explicitly
     * does not touch the graphics overlay hardware.  Nothing here needs that
     * distinction yet, so go straight to running.
     */
    stb_atu_set_status(s, STB_BOARD_RUNNING);
}

/*
 * The host has rung the command doorbell.  Read the mailbox and act on it.
 *
 * Only the dispatch envelope is established: commands 2..26 are valid, a
 * channel above 2 wedges the real firmware, and InitBoard carries a
 * 0x50-byte TULSA_CONFIG.  The individual command numbers are not yet known,
 * so anything else is traced with its payload length for identification
 * against a real driver run rather than guessed at.
 */
/*
 * Ring the outbound doorbell.
 *
 * Section 47.2 mapped the bits from both sides independently: `1 << chan` is
 * the stream state machine, `64 << chan` and `256 << chan` are two other
 * per-channel events, bit 10 is a notification sink and bit 31 a watchdog.
 * The driver's DPC (section 22.8) ORs ODR into an accumulator and clears it
 * write-1-to-clear, and STBTCtl.dll's status thread asks for bit 2 of OISR
 * only -- which is why section 32.7 saw OIMR = 0xfb.
 */
static void stb_atu_ring(STBTulsaATU *s, uint32_t bits)
{
    s->odr |= bits;
    s->oisr |= STB_OISR_DOORBELL;
    trace_stb_tulsa_ring(bits, s->odr);
    stb_atu_update_irq(s);
}

/*
 * Report a stream state change on a channel.
 *
 * Writes the code and the handle into the channel's block in shared memory
 * and rings `1 << chan`.  This is the whole of what the host needs to see a
 * stream progress; nothing here decodes anything, and the decoder itself
 * (section 66) is a later and much larger piece of work.
 */
static void stb_atu_chan_arm(STBTulsaChannel *c)
{
    STBTulsaATU *s = c->atu;

    if (!c->notify_timer || !s->notify_delay_ms) {
        return;                 /* 0 disables the notification entirely */
    }
    if (c->notify_head == c->notify_tail) {
        return;                 /* nothing queued */
    }
    timer_mod(c->notify_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)s->notify_delay_ms * 1000 * 1000);
}

static void stb_atu_chan_notify(void *opaque)
{
    STBTulsaChannel *c = opaque;
    STBTulsaATU *s = c->atu;
    hwaddr blk = STB_SHM_CHANSTATE + (hwaddr)c->index * STB_SHM_CHANSTATE_SZ;
    uint32_t code;

    if (c->notify_head == c->notify_tail) {
        return;
    }
    code = c->notify_q[c->notify_head % STB_CHAN_NOTIFY_QDEPTH];
    c->notify_head++;

    stl_le_p(s->shm + blk + STB_SHM_CHAN_CODE, code);
    stl_le_p(s->shm + blk + STB_SHM_CHAN_HANDLE, c->handle);
    trace_stb_tulsa_chan_notify(c->index, code, c->handle);
    stb_atu_ring(s, STB_ODR_CHAN(c->index));

    /*
     * One notification per doorbell: the host reads the block from the ODR
     * handler, so a second code written before that runs would be lost.
     */
    stb_atu_chan_arm(c);
}

/* Queue a stream notification for one channel; see STB_CHAN_NOTIFY_DELAY_NS. */
static void stb_atu_chan_post(STBTulsaATU *s, uint32_t chan, uint32_t code)
{
    STBTulsaChannel *c = &s->chan[chan];
    bool idle = c->notify_head == c->notify_tail;

    if (c->notify_tail - c->notify_head >= STB_CHAN_NOTIFY_QDEPTH) {
        return;
    }
    c->notify_q[c->notify_tail % STB_CHAN_NOTIFY_QDEPTH] = code;
    c->notify_tail++;

    if (idle) {
        stb_atu_chan_arm(c);
    }
}

/* Drop anything still queued - a new command supersedes it. */
static void stb_atu_chan_flush(STBTulsaChannel *c)
{
    c->notify_head = c->notify_tail = 0;
    if (c->notify_timer) {
        timer_del(c->notify_timer);
    }
}

static void stb_atu_doorbell(STBTulsaATU *s)
{
    const uint8_t *mbox = s->shm + STB_SHM_MAILBOX;
    uint32_t cmd  = ldl_le_p(mbox + STB_MBOX_CMD  * 4);
    uint32_t chan = ldl_le_p(mbox + STB_MBOX_CHAN * 4);
    uint32_t arg  = ldl_le_p(mbox + STB_MBOX_ARG  * 4);
    const uint8_t *payload = mbox + STB_MBOX_PAYLOAD * 4;

    s->commands_seen++;
    s->last_command = cmd;
    trace_stb_tulsa_doorbell(cmd, chan, arg);

    if (chan > STB_TULSA_CHANNELS) {
        /*
         * sub_A02E3290 is "b .", an unconditional branch to itself: a bad
         * channel number hangs the i960 until the card is reset.  Latching a
         * fault is more useful than reproducing the hang.
         */
        trace_stb_tulsa_cmd_chan(chan);
        stb_atu_set_status(s, STB_BOARD_FAULT);
        return;
    }

    if (cmd < STB_CMD_MIN || cmd > STB_CMD_MAX) {
        trace_stb_tulsa_cmd_range(cmd);
        stb_atu_reply(s, 0, ~0u);
        return;
    }

    /*
     * Identify InitBoard by its payload rather than by a guessed command
     * number: it is the only command carrying a 0x50-byte block, and arg
     * carries the length.
     */
    if (arg == STB_CMD_INITBOARD_LEN) {
        stb_atu_initboard(s, payload);
        stb_atu_reply(s, 0, 0);
        return;
    }

    /*
     * The stream channel, faked.
     *
     * Section 65.2: Hollywood validates the board by loading and playing
     * `Mpeg Test File`, waits `Load Timeout` 500 ms for the decoder to
     * notify, retries once, and then declares the device dead and escalates
     * to a machine reboot.  Nothing answered because commands 2-5 fell
     * through to "unknown" here.
     *
     * Commands 2-5 are the stream state machine.  Section 68.8: both the
     * host's state word and the board's notification are values of the SDK's
     * DECODE_STATE enum, so each command sets a *_REQUESTED state and the
     * board answers with the settled one - PLAY_REQUESTED/PLAYING,
     * STOP_REQUESTED/STOPPED, PAUSE_REQUESTED/PAUSED,
     * RESUME_REQUESTED/PLAYING.  No bytes have to be decoded, and none are.
     * The command's `arg` is the stream handle, which the notify has to echo
     * or sub_1000359F ignores it.
     *
     * This is deliberately a stub.  It makes the decoder *protocol* work so
     * that the rest of Hollywood can be reached; the descriptor ring at
     * +0x8000 and the actual MPEG-2 program streams of section 66 are
     * untouched, and anything that measures real decode progress will see
     * nothing.
     */
    switch (cmd) {
    case STB_CMD_STREAM_PLAY:           /* host PLAY_REQUESTED -> PLAYING  */
        stb_atu_chan_flush(&s->chan[chan]);
        s->chan[chan].handle = arg;
        s->chan[chan].active = true;
        s->chan[chan].state = STB_DEC_PLAYING;
        stb_atu_reply(s, 0, 0);
        stb_atu_chan_post(s, chan, STB_DEC_PLAYING);
        return;

    case STB_CMD_STREAM_STOP:           /* host STOP_REQUESTED -> STOPPED  */
        stb_atu_chan_flush(&s->chan[chan]);
        s->chan[chan].state = STB_DEC_STOPPED;
        s->chan[chan].active = false;
        stb_atu_reply(s, 0, 0);
        stb_atu_chan_post(s, chan, STB_DEC_STOPPED);
        return;

    case STB_CMD_STREAM_PAUSE:          /* host PAUSE_REQUESTED -> PAUSED  */
        s->chan[chan].state = STB_DEC_PAUSED;
        stb_atu_reply(s, 0, 0);
        stb_atu_chan_post(s, chan, STB_DEC_PAUSED);
        return;

    case STB_CMD_STREAM_RESUME:         /* host RESUME_REQUESTED -> PLAYING */
        s->chan[chan].state = STB_DEC_PLAYING;
        stb_atu_reply(s, 0, 0);
        stb_atu_chan_post(s, chan, STB_DEC_PLAYING);
        return;

    default:
        break;
    }

    if (stb_atu_ctx_cmd(s, cmd)) {
        stb_atu_reply(s, 0, 0);
        return;
    }

    /*
     * The remaining commands do carry their parameters in the mailbox:
     * 9 is {dword, byte, byte}, 10 is {byte, byte}, 21 one dword and 22 two.
     * Trace the words rather than discarding them - nothing in the model
     * drives the analog encoder they configure, but a silent ACK makes a
     * misconfiguration invisible.
     */
    switch (cmd) {
    case 17:
        /*
         * EnableBypass(BOOL).  Section 68.3: the arg that looked like a
         * stream handle is the boolean.  The firmware drives a relay through
         * sub_A02E49A4(0x04, ...); here it is a latch, so that "the card is
         * in bypass" is at least visible rather than silently accepted.
         */
        s->ctx.bypass = arg != 0;
        trace_stb_tulsa_ctx_scalar(cmd, 0xff, arg);
        stb_atu_reply(s, 0, 0);
        return;
    case 9: case 10: case 21: case 22:
        trace_stb_tulsa_cmd_payload(cmd, ldl_le_p(payload),
                                    ldl_le_p(payload + 4),
                                    ldl_le_p(payload + 8));
        stb_atu_reply(s, 0, 0);
        return;
    default:
        break;
    }

    trace_stb_tulsa_cmd_unknown(cmd, arg);
    stb_atu_reply(s, 0, 0);
}

static uint64_t stb_atu_mu_read(void *opaque, hwaddr addr, unsigned size)
{
    STBTulsaATU *s = opaque;
    uint32_t val;

    switch (addr) {
    case STB_MU_IMR0: val = s->imr[0]; break;
    case STB_MU_IMR1: val = s->imr[1]; break;

    case STB_MU_OMR0:
        /* Reading an outbound message retires its status bit. */
        val = s->omr[0];
        s->oisr &= ~STB_OISR_OMR0;
        stb_atu_update_irq(s);
        break;
    case STB_MU_OMR1:
        val = s->omr[1];
        s->oisr &= ~STB_OISR_OMR1;
        stb_atu_update_irq(s);
        break;

    case STB_MU_IDR:  val = s->idr;  break;
    case STB_MU_IISR: val = s->iisr; break;
    case STB_MU_IIMR: val = s->iimr; break;
    case STB_MU_ODR:  val = s->odr;  break;
    case STB_MU_OISR: val = s->oisr; break;
    case STB_MU_OIMR: val = s->oimr; break;

    default:
        trace_stb_tulsa_mu_unknown(addr, 0, "rd");
        return 0;
    }

    trace_stb_tulsa_mu_read(stb_mu_regname(addr), val);
    return val;
}

static void stb_atu_mu_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    STBTulsaATU *s = opaque;

    switch (addr) {
    case STB_MU_IMR0:
        if (s->fw_started) {
            /*
             * Once the firmware is running, IMR0 stops being the window
             * register and becomes the mailbox doorbell.  The production
             * sequence is: write the command at +0x450, its channel at
             * +0x454 and its argument at +0x458, store a non-zero token in
             * IMR0, then poll IMR0 until the i960 zeroes it.  Leaving it
             * set is what parks STBTCtl.dll in a read loop on IMR0 with the
             * board otherwise alive.
             */
            s->imr[0] = val;
            trace_stb_tulsa_mu_write("IMR0 (doorbell)", val);
            stb_atu_doorbell(s);
            s->imr[0] = 0;                 /* command consumed */
            break;
        }
        /*
         * Window select, not a doorbell.  STBTCtl.dll's sub_100016FE writes a
         * base in the i960's local address space here, then spins up to ten
         * times waiting for OMR1 to read back the same value; the wrappers
         * around it pass 0xA0000000 and 0xE0000000, and the reset path passes
         * 0 and then restores whatever was there before.  Everything the host
         * reads or writes above the MU registers is relative to this base.
         *
         * The acknowledge is the i960 storing the value into OMR1, which by
         * the Messaging Unit's definition also raises OISR bit 1.
         */
        s->imr[0] = val;
        s->window = val;
        s->window_valid = true;
        trace_stb_tulsa_window(val);
        s->omr[1] = val;
        s->oisr |= STB_OISR_OMR1;
        stb_atu_update_irq(s);
        break;

    case STB_MU_IMR1:
        /*
         * The firmware entry point, written once the S-record download has
         * finished: STBT960.hex's S7 record gives 0xa02e8c80 and the host
         * writes 0xa02e8cc0.  From here on the board is executing its own
         * code, which changes what IMR0 means.
         */
        trace_stb_tulsa_mu_write("IMR1", val);
        s->imr[1] = val;
        if (val >= STB_I960_DRAM_BASE) {
            s->fw_started = true;
            /*
             * Do NOT post anything at 0xa0000058 here.  It looked like a
             * firmware-alive flag the host was spinning on; it is not.
             * CSurfaceControl::MonitorOverlayStatus reads it once every 30 ms
             * and passes it straight to SetVerticalLineCount as the flag that
             * subtracts 0x100000 from the programmed line count, so a
             * non-zero value silently changes the video timing.  Zero is the
             * correct idle value.  See TULSA.md section 46.2.
             */
        }
        break;

    case STB_MU_IDR:
        /*
         * The i960RP's inbound doorbell.  This is the natural command trigger
         * now that IMR0 is known to be the window register; nothing observed
         * has rung it yet, so the mailbox dispatch lives here until something
         * does.
         */
        trace_stb_tulsa_mu_write("IDR", val);
        s->idr = val;
        if (val) {
            stb_atu_doorbell(s);
            s->idr = 0;
        }
        break;
    case STB_MU_IISR:
        trace_stb_tulsa_mu_write("IISR", val);
        s->iisr = val;
        break;
    case STB_MU_IIMR:
        trace_stb_tulsa_mu_write("IIMR", val);
        s->iimr = val;
        break;

    case STB_MU_ODR:
        /* Outbound doorbell is write-1-to-clear. */
        trace_stb_tulsa_mu_write("ODR", val);
        s->odr &= ~(uint32_t)val;
        if (!s->odr) {
            s->oisr &= ~STB_OISR_DOORBELL;
        }
        stb_atu_update_irq(s);
        break;

    case STB_MU_OISR:
        /*
         * Bits 0-2 retire by reading OMR0/OMR1 and clearing ODR, so writes
         * to them do nothing; the upper error bits are write-1-to-clear.
         * TulsaDPC writes 0 and then 0xFF, which is consistent with both.
         */
        trace_stb_tulsa_mu_write("OISR", val);
        s->oisr &= ~((uint32_t)val & 0xf8);
        stb_atu_update_irq(s);
        break;

    case STB_MU_OIMR:
        trace_stb_tulsa_mu_write("OIMR", val);
        s->oimr = val;
        stb_atu_update_irq(s);
        break;

    case STB_MU_OMR0:
    case STB_MU_OMR1:
        /* Written by the i960, not the host. */
        break;

    default:
        trace_stb_tulsa_mu_unknown(addr, val, "wr");
        break;
    }
}

static const MemoryRegionOps stb_atu_mu_ops = {
    .read = stb_atu_mu_read,
    .write = stb_atu_mu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Debug shim over the shared-memory aperture.  The BAR is normally plain RAM
 * so the host's reads and writes never trap and cannot be seen; this overlay
 * forwards them to the same backing store and traces each one, which is the
 * only way to find out which offsets the production driver actually polls.
 */
/*
 * The BAR is a 4 MiB window onto board DRAM.  IMR0 selects which part of the
 * i960's local address space it looks at (§34), so an access at BAR offset
 * `addr` reaches DRAM at `window - 0xa0000000 + addr`.
 *
 * The aperture has to be this wide: the firmware downloader writes the whole
 * of STBT960.hex (0xa02d8000..0xa0380000) through it without ever sliding the
 * window, so a 64 KiB BAR puts the very first S3 record 0x2d8000 past the end
 * of the mapping.  An earlier experiment widened the BAR and concluded it
 * made things worse; that was wrong, and §44 says why.
 */
/*
 * The guest PC behind an aperture access.  There is no symbol information on
 * this side of the PCI bus, so the only way to tell which module is polling a
 * given shared-memory word is to record where the access came from and match
 * it against the module list afterwards.  Valid under both TCG and WHPX:
 * whpx-all.c syncs the register state before dispatching the MMIO exit.
 */
static uint64_t stb_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs && cs->cc && cs->cc->get_pc) {
        return cs->cc->get_pc(cs);
    }
    return 0;
}

static hwaddr stb_atu_window_offset(STBTulsaATU *s, hwaddr addr)
{
    hwaddr base = 0;

    if (s->window_valid && s->window >= STB_I960_DRAM_BASE) {
        base = s->window - STB_I960_DRAM_BASE;
    }
    if (base + addr + 4 > s->dram_size) {
        return 0;               /* off the end of DRAM: fold back to the header */
    }
    return base + addr;
}

/*
 * Dump the instruction bytes around a guest PC, once per distinct PC.
 *
 * NT 4.0 only sends DbgKdLoadSymbols for kernel drivers, so a user-mode PC
 * cannot be turned into a module name from the debug stream, and the HMP
 * memory commands read through whichever process happens to be current.
 * Reading here is the one point where the right address space is guaranteed
 * to be live: byte-match the result against the SDK DLLs to identify the
 * caller.
 */
static void stb_dump_guest_code(STBTulsaATU *s, uint64_t pc)
{
    CPUState *cs = current_cpu;
    uint8_t code[16];
    char hex[sizeof(code) * 3 + 1];
    int i;

    if (!cs || !pc || pc == s->last_code_pc) {
        return;
    }
    s->last_code_pc = pc;

    if (cpu_memory_rw_debug(cs, pc - 8, code, sizeof(code), false) != 0) {
        return;
    }
    for (i = 0; i < (int)sizeof(code); i++) {
        snprintf(hex + i * 3, 4, "%02x ", code[i]);
    }
    trace_stb_tulsa_guest_code(pc - 8, hex);
}

static uint64_t stb_atu_shm_read(void *opaque, hwaddr addr, unsigned size)
{
    STBTulsaATU *s = opaque;
    uint32_t val = ldn_le_p(s->shm + stb_atu_window_offset(s, addr), size);

    if (addr < STB_TULSA_ATU_TRACE_LIMIT) {
        uint64_t pc = stb_guest_pc();
        trace_stb_tulsa_atu_access("rd", addr, size, val, s->window, pc);
        stb_dump_guest_code(s, pc);
    }
    return val;
}

static void stb_atu_shm_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    STBTulsaATU *s = opaque;

    stn_le_p(s->shm + stb_atu_window_offset(s, addr), size, val);
    if (addr < STB_TULSA_ATU_TRACE_LIMIT) {
        trace_stb_tulsa_atu_access("wr", addr, size, val, s->window,
                                   stb_guest_pc());
    }

    /*
     * The host hands the board over by clearing the ready word after it has
     * filled in the device context at +0x4c0 and the TMC images at +0xf080.
     * On real hardware the i960 firmware notices, finishes bringing the card
     * up and puts the word back with the running bit set; the SDK sample
     * waits on exactly that and otherwise sits forever at "Reseting,
     * programming, and starting card 0".  Nothing here needs the interval, so
     * complete the handover immediately.
     */
    if (addr == STB_SHM_READY && val == 0) {
        s->status = STB_BOARD_RESET;          /* force the transition to fire */
        stb_atu_set_status(s, STB_BOARD_RUNNING);
        stb_atu_reply(s, STB_READY_PRESENT | STB_READY_RUNNING, s->window);
    }
}

static const MemoryRegionOps stb_atu_shm_ops = {
    .read = stb_atu_shm_read,
    .write = stb_atu_shm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

static void stb_atu_reset_shmem(STBTulsaATU *s)
{
    stb_atu_field_start(s, false);
    memset(s->shm, 0, s->dram_size);
    stl_le_p(s->shm + STB_SHM_READY, STB_READY_PRESENT);
    stl_le_p(s->shm + STB_SHM_DRAMSIZE, s->dram_size);
}

static void stb_atu_realize(PCIDevice *pdev, Error **errp)
{
    STBTulsaATU *s = STB_TULSA_ATU(pdev);

    pci_config_set_interrupt_pin(pdev->config, 1); /* INTA, level, shared */

    for (int i = 0; i < STB_TULSA_CHANNELS; i++) {
        s->chan[i].atu = s;
        s->chan[i].index = i;
        s->chan[i].notify_timer =
            timer_new_ns(QEMU_CLOCK_VIRTUAL, stb_atu_chan_notify, &s->chan[i]);
    }

    s->field_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stb_atu_field_tick, s);

    /*
     * One 4 MiB BAR, built as a container with the messaging unit overlaid
     * at offset 0 at higher priority.
     *
     * Section 22.11 originally wanted the aperture backed by plain RAM so
     * the mailbox, per-channel state and descriptors would cost no VM
     * exits.  Sections 34, 41 and 44 then established that the aperture is
     * a *sliding window* into i960 local space, re-aimed by writing IMR0,
     * so a given BAR offset does not map to a fixed backing address and RAM
     * cannot express it.  The exits are structural, not instrumentation.
     * What is instrumentation is the trace call itself, and that is already
     * bounded to the low aperture by STB_TULSA_ATU_TRACE_LIMIT.
     */
    memory_region_init(&s->bar0, OBJECT(s), "stb-tulsa-atu",
                       STB_TULSA_ATU_BAR_SIZE);
    memory_region_init_io(&s->shmtrace, OBJECT(s), &stb_atu_shm_ops, s,
                          "stb-tulsa-shmtrace", STB_TULSA_ATU_BAR_SIZE);
    memory_region_add_subregion_overlap(&s->bar0, 0, &s->shmtrace, 1);

    memory_region_init_io(&s->mu, OBJECT(s), &stb_atu_mu_ops, s,
                          "stb-tulsa-mu", STB_TULSA_MU_SIZE);
    memory_region_add_subregion_overlap(&s->bar0, 0, &s->mu, 2);

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    s->dram = g_malloc0(s->dram_size);
    s->shm = s->dram;
    stb_atu_reset_shmem(s);
    s->oimr = 0xff;  /* the driver's own reset value: all masked */
}

static void stb_atu_reset_hold(Object *obj, ResetType type)
{
    STBTulsaATU *s = STB_TULSA_ATU(obj);

    s->imr[0] = s->imr[1] = 0;
    s->omr[0] = s->omr[1] = 0;
    s->idr = s->iisr = s->odr = s->oisr = 0;
    s->iimr = s->oimr = 0xff;
    s->status = STB_BOARD_RESET;
    s->fw_started = false;
    s->window_valid = false;
    s->window = 0;
    s->configured = false;
    s->commands_seen = 0;
    s->last_command = 0;
    /*
     * Clear the channels' *state* without losing their wiring.  A plain
     * memset over the array also nulls `atu`, `index` and `notify_timer`,
     * which are set up once in realize -- and reset runs after realize, so
     * every deferred notification silently did nothing.
     */
    for (int i = 0; i < STB_TULSA_CHANNELS; i++) {
        STBTulsaChannel *c = &s->chan[i];

        if (c->notify_timer) {
            timer_del(c->notify_timer);
        }
        c->active = false;
        c->state = 0;
        c->handle = 0;
        c->notify_head = 0;
        c->notify_tail = 0;
        c->buffers_submitted = 0;
        c->buffers_retired = 0;
    }
    if (s->shm) {
        stb_atu_reset_shmem(s);
    }
    stb_atu_update_irq(s);
}

static const VMStateDescription vmstate_stb_atu = {
    .name = "stb-tulsa-atu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, STBTulsaATU),
        VMSTATE_UINT32_ARRAY(imr, STBTulsaATU, 2),
        VMSTATE_UINT32_ARRAY(omr, STBTulsaATU, 2),
        VMSTATE_UINT32(idr, STBTulsaATU),
        VMSTATE_UINT32(iisr, STBTulsaATU),
        VMSTATE_UINT32(iimr, STBTulsaATU),
        VMSTATE_UINT32(odr, STBTulsaATU),
        VMSTATE_UINT32(oisr, STBTulsaATU),
        VMSTATE_UINT32(oimr, STBTulsaATU),
        VMSTATE_UINT8(status, STBTulsaATU),
        VMSTATE_BOOL(configured, STBTulsaATU),
        VMSTATE_END_OF_LIST()
    }
};

static const Property stb_atu_properties[] = {
    DEFINE_PROP_UINT32("dram-size", STBTulsaATU, dram_size, 8 * MiB),
    DEFINE_PROP_UINT32("notify-delay-ms", STBTulsaATU, notify_delay_ms, 10),
    DEFINE_PROP_BOOL("vertical-doorbell", STBTulsaATU, vertical_doorbell, true),
};

static void stb_atu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize   = stb_atu_realize;
    k->vendor_id = PCI_VENDOR_ID_STB;
    k->device_id = PCI_DEVICE_ID_STB_TULSA_ATU;
    k->class_id  = PCI_CLASS_DISPLAY_OTHER;
    k->revision  = 0x01;

    rc->phases.hold = stb_atu_reset_hold;
    dc->vmsd = &vmstate_stb_atu;
    dc->desc = "STB Tulsa i960RP ATU";
    device_class_set_props(dc, stb_atu_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* ================================================================== *
 * Rendition Verite V2200
 * ================================================================== */

/*
 * Decode a submitted overlay command list.  The list is 514 dwords: two
 * header dwords followed by sixteen 32-dword slots, each of whose first
 * dword is a jump address into the Verite microcode.
 */
/*
 * The handlers that take section 21.3's blit payload: every OverlayScale and
 * VideoScale variant, across the plain / YUV / SrcCK / DstCK axes that
 * section 24.1 decomposed.
 */
static bool stb_v2200_is_blit_handler(uint32_t h)
{
    switch (h) {
    case 0x30a0: case 0x3124: case 0x3148:
    case 0x38a0: case 0x38f8: case 0x391c:
    case 0x39c0: case 0x3a18: case 0x3a3c:
        return true;
    default:
        return false;
    }
}

/*
 * Execute one overlay slot: composite the surface at payload +4 onto the
 * primary at payload +8.
 *
 * Field order per section 55.1, which corrects section 21.3: the high half
 * of each rect word is X or width, the low half is Y or height-halved, and
 * the pair (+0,+2) describes the *source* surface while (+1,+3) describes
 * the destination.  Slot 4 of a real list is the clincher -- 720x70 at
 * y=240, which is exactly the static timebar band section 25 measured on
 * hardware.
 *
 * Scaling, colour keying, alpha and the scrolling variants are not done
 * here; only 1:1 blits of non-scrolling slots, so that what appears can be
 * checked against section 24.7's preset before more is built on it.
 */
/*
 * Advance every live scroll by one field.
 *
 * Section 21.5 step 5: add lScrollPixelsPerStep / dwScrollVerticalsPerStep to
 * the position each field, honour the pause position, bump the notify
 * counter.  All of it is 16.16, so the step is a fraction of a pixel and the
 * position is sub-pixel -- the host sets a rate and a start, the board owns
 * the position from then on (section 21.4).
 *
 * Section 25's capture of a real card is the acceptance test: the preset runs
 * 1 pixel per 2 verticals, which is 0x8000 per field here, and measured a
 * dead-flat 30 px/s over fifteen seconds at NTSC's 59.94 Hz field rate.
 */
static void stb_v2200_run_list(STBTulsaV2200 *s, uint32_t addr, bool replay);


/* ---- host-time accounting (section 146) -------------------------- */

static int64_t stb_prof_begin(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static void stb_prof_end(STBTulsaV2200 *s, unsigned bucket, int64_t t0)
{
    uint64_t dt = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0);
    STBTulsaProf *p = &s->prof[bucket];

    p->ns += dt;
    p->calls++;
    p->ns_total += dt;
    p->calls_total++;
    if (dt > p->max_ns) {
        p->max_ns = dt;
    }
}

/*
 * Report once a second from the field tick, which is the only thing here
 * that runs on its own schedule.  The window is reported rather than the
 * running total so a stall shows up as a spike instead of being averaged
 * away over an hour of uptime.
 */
static void stb_prof_report(STBTulsaV2200 *s)
{
    static const char *const names[STB_PROF_MAX] = {
        "list", "field", "composite", "reg"
    };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t window;
    unsigned i;

    if (!s->prof_window) {
        s->prof_window = now;
        return;
    }
    window = (uint64_t)(now - s->prof_window);
    if (window < 1000000000ULL) {
        return;
    }
    for (i = 0; i < STB_PROF_MAX; i++) {
        STBTulsaProf *p = &s->prof[i];

        if (p->calls) {
            trace_stb_tulsa_prof(s->role ?: "?", names[i], p->calls,
                                 p->ns / 1000, p->max_ns / 1000,
                                 (unsigned)(p->ns * 100 / window));
        }
        p->ns = p->max_ns = p->calls = 0;
    }
    s->prof_window = now;
}

static void stb_v2200_scroll_tick(void *opaque)
{
    STBTulsaV2200 *s = opaque;
    int64_t _t0 = stb_prof_begin();
    int i;

    for (i = 0; i < s->n_scroll; i++) {
        uint32_t base = s->scroll[i].addr;
        uint32_t extent = s->scroll[i].extent;
        int32_t pixels, verticals, step;
        uint32_t pos, prev, limit, pause, notify_pos;
        int32_t  notify;
        bool wrapped = false;

        if (!base || base + 0x24 > STB_V2200_MEM_SIZE || !extent) {
            continue;
        }
        pause     = ldl_le_p(s->vram + base + STB_SS_PAUSE);
        pixels    = (int32_t)ldl_le_p(s->vram + base + STB_SS_PIXELS) >> 16;
        verticals = (int32_t)ldl_le_p(s->vram + base + STB_SS_VERTICALS) >> 16;
        if (!verticals) {
            continue;
        }
        step = (pixels << 16) / verticals;
        prev = ldl_le_p(s->vram + base + STB_SS_POSITION);
        pos = prev + step;

        limit = extent << 16;
        if (limit) {                    /* circular buffer, section 25 */
            while (pos >= limit) {
                pos -= limit;
                wrapped = true;
            }
        }

        /*
         * dwScrollPausePosition -- the refill barrier, and the reason the
         * pulldown looks right.
         *
         * vscroll.cpp recomputes it on every refill as
         *     StopPoint = NextUpdatePosition - windowHeight
         * and hands it down with SetSurfaceDescExt, so it is not a fixed park
         * at all: it is a moving barrier that tracks how far the host has
         * drawn.  The board may never scroll past what exists.  In steady
         * state it never catches up -- a refill advances the barrier by
         * UPDATE_SIZE_VS (100 px) while a notify period is only 70 px -- so
         * nothing parks and section 25's capture shows unbroken motion.
         *
         * The preset's pulldown is the one place it is used deliberately.
         * vscroll.cpp:644 stops servicing the surface and spins on
         *     GetSurfaceDescExt(); PixelsFromPausePos = pause - position;
         *     while (PixelsFromPausePos > lScrollPixelsPerStep)
         * so the scroll runs into the barrier, parks, and only then does the
         * 49-line walk run.  Without this the grid keeps moving underneath a
         * window that is itself moving, which duplicates listing rows against
         * the timebar -- exactly what a real card does not do.
         *
         * Out of range means never pause, which is how the non-preset paths
         * disable it (0xffffffff).
         */
        if ((pause >> 16) < extent) {
            uint32_t park = pause;
            bool crossed = wrapped ? (park > prev || park <= pos)
                                   : (park > prev && park <= pos);

            if (prev == park) {
                pos = park;             /* already parked: stay put */
            } else if (crossed) {
                pos = park;
                /*
                 * Tell the host on the way in rather than only on the
                 * countdown.  Parked means "I have run out of drawn source";
                 * the host's worker is blocked on hScrollEvent and the thing
                 * that unparks this is the refill it does when woken.
                 */
                stl_le_p(s->vram + base + STB_SS_NOTIFY_FLAG, 1);
                trace_stb_v2200_scroll_park(s->role ?: "?", i, base,
                                            pos >> 16);
            }
        }

        stl_le_p(s->vram + base + STB_SS_POSITION, pos);

        /*
         * The *other* notification trigger: dwScrollNotifPosition at +0x20.
         * SDE_SCROLL_NOTIFY arms both - the SDK header says the application
         * is notified "at each scroll notification position and/or
         * increment" - and Hollywood uses the position one.
         *
         * Section 83 concluded that Hollywood never armed a notification at
         * all, because the countdown at +0x14 and its reload at +0x18 both
         * sit at the -1.0 "never" sentinel.  They do, and that is
         * deliberate: dwScrollNotifIncrement is 0xffff precisely to disable
         * the increment trigger.  The arming lives at +0x20, which this
         * model defined a constant for and then never read.  Measured off
         * the running guide: the listings block carries NOTIFY_POS = 277.0
         * against a 588-line source, the time bar 24.0 against 60 - both
         * well in range, both armed, and both ignored until now.  So the
         * host armed a handshake, nothing ever fired it, and Hollywood sat
         * on hScrollEvent until it gave up.
         *
         * Out of range means never, the same convention the pause barrier
         * uses for 0xffffffff.
         */
        notify_pos = ldl_le_p(s->vram + base + STB_SS_NOTIFY_POS);
        if ((notify_pos >> 16) < extent && pos != prev) {
            bool crossed = wrapped ? (notify_pos > prev || notify_pos <= pos)
                                   : (notify_pos > prev && notify_pos <= pos);

            if (crossed) {
                stl_le_p(s->vram + base + STB_SS_NOTIFY_FLAG, 1);
                trace_stb_v2200_scroll_notify(s->role ?: "?", i, base,
                                              pos >> 16, notify_pos >> 16);
            }
        }

        /*
         * The notify handshake (section 6, section 21.4).
         *
         * Without this the picture scrolls but the source is never refilled:
         * the host draws one window's worth, hands the surface over and waits
         * on hScrollEvent for ever, so the crawl runs off the end of what was
         * drawn and the grid wraps onto stale pixels.  That is the whole of
         * the "bitmap is cut off" symptom.
         *
         * The board side is a countdown in whole steps.  One field is
         * 1/verticals of a step, so decrement by that and the handshake lands
         * on a step boundary regardless of the rate.  On expiry, reload from
         * the host's parameter at +0x18 and raise the flag at +0x1c;
         * CSurfaceControl::MonitorOverlayStatus reads the block every 30 ms,
         * and on a non-zero flag clears it and signals the surface's event.
         */
        /*
         * The scroll-notification countdown, section 81.3.
         *
         * It is **signed** 16.16, and a negative value is the same "never"
         * sentinel the pause field uses: both the time bar and a quiet
         * listings surface sit at 0xffff0000, which is -1.0, not a countdown
         * of 65536 steps.  Reading it unsigned decrements that sentinel on
         * every field, which is how a disabled counter was observed to have
         * drifted from 0xffff0000 down to 0xff42ab67 - a disabled
         * notification slowly turning into a live one that fires at an
         * arbitrary position.  Hollywood draws where the board tells it the
         * scroll has reached, so a notification that fires at the wrong
         * moment makes it draw in the wrong place, and the result is a new
         * bar written over one that has not scrolled clear yet.
         *
         * A negative reload means one-shot: fire once, then stay disabled.
         */
        notify = (int32_t)ldl_le_p(s->vram + base + STB_SS_NOTIFY_CNT);
        if ((int32_t)notify > 0 && pos != prev) {
            int32_t dec = (int32_t)(0x10000 / verticals);

            if (dec <= 0) {
                dec = 1;
            }
            if ((int32_t)notify > dec) {
                stl_le_p(s->vram + base + STB_SS_NOTIFY_CNT,
                         (uint32_t)((int32_t)notify - dec));
            } else {
                int32_t reload =
                    (int32_t)ldl_le_p(s->vram + base + STB_SS_NOTIFY_INC);

                stl_le_p(s->vram + base + STB_SS_NOTIFY_CNT, (uint32_t)reload);
                stl_le_p(s->vram + base + STB_SS_NOTIFY_FLAG, 1);
                trace_stb_v2200_scroll_notify(s->role ?: "?", i, base,
                                              pos >> 16, reload >> 16);
            }
        }

        trace_stb_v2200_scroll_pos(s->role ?: "?", i, base, pos >> 16,
                                   ldl_le_p(s->vram + base + STB_SS_PAUSE) >> 16);
    }

    /*
     * Re-execute the display list.  On the real board the microcode runs the
     * list the host handed it once per vertical, for ever, and the host only
     * submits again when the *content* changes -- which is why section 56's
     * capture stops after five submissions and the picture still moves on
     * hardware.  Blitting only at submission time froze the frame and made
     * the scroll invisible; the list is the board's program, not a one-shot.
     *
     * This is a replay, so it does not bump the submission counter, touch
     * the retire pointer or re-trace: at 59.94 Hz the trace would be the
     * only thing in the log.
     */
    if (s->cmd_current) {
        stb_v2200_run_list(s, s->cmd_current, true);
    }

    timer_mod(s->field_timer, stb_field_next(&s->field_deadline));
    stb_prof_end(s, STB_PROF_FIELD, _t0);
    stb_prof_report(s);
}

/*
 * Per-pixel source-over-destination blend.
 *
 * DirectDraw's DDOVER_ALPHASRC, which is what UpdateOverlay() is called with
 * when a surface has m_bBlend set.  The alpha is the source pixel's own byte,
 * not a constant: the sample app paints it into the surface when it draws
 * (DrawOverlayBitmap ORs `PixColor | (alpha << 24)`), which is why payload
 * +13 reads 0xffffffff -- "no constant alpha" -- on every slot in the preset.
 *
 * `invert` is DDOVER_ALPHASRCNEG.  `keep_dst_alpha` is bPreserveDestAlpha, and
 * it is load-bearing rather than cosmetic: section 2's mixer keys live video
 * off the RGB processor's alpha channel per pixel, so a blend that overwrote
 * the destination alpha would punch the video window out from under itself.
 */
static void stb_v2200_blend_row(uint8_t *dst, const uint8_t *src, uint32_t n,
                                bool invert, bool keep_dst_alpha)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t sp = ldl_le_p(src + i * 4);
        uint32_t dp = ldl_le_p(dst + i * 4);
        uint32_t a = sp >> 24;
        uint32_t ia, out;

        if (invert) {
            a = 255 - a;
        }
        ia = 255 - a;

        out = ((((sp >> 16) & 0xff) * a + ((dp >> 16) & 0xff) * ia) / 255) << 16;
        out |= ((((sp >> 8) & 0xff) * a + ((dp >> 8) & 0xff) * ia) / 255) << 8;
        out |= (((sp & 0xff) * a + (dp & 0xff) * ia) / 255);
        out |= (keep_dst_alpha ? (dp & 0xff000000u) : (sp & 0xff000000u));
        stl_le_p(dst + i * 4, out);
    }
}

/*
 * Map a frame row to an offset, given the base pair a slot names and a line
 * pitch.  Three layouts occur and the pair itself says which:
 *
 *   b0 == b1            progressive - the graphics surfaces, which name the
 *                       same address twice
 *   |b0 - b1| == pitch  interleaved - the display surfaces, where the two
 *                       field origins are one line apart (section 53.1)
 *   otherwise           two separate field blocks - the SAA7146's capture
 *                       buffers, whose bases sit 240 lines of pitch apart
 *
 * The third is new in section 85 and is why the capture cannot simply be
 * written as one progressive raster: the part fills two blocks, and the
 * display list reads them back as a frame.
 */
static uint64_t stb_v2200_row_off(uint32_t b0, uint32_t b1, uint32_t pitch,
                                  uint32_t row)
{
    uint32_t lo = MIN(b0, b1), hi = MAX(b0, b1);

    if (!b1 || b0 == b1) {
        return (uint64_t)b0 + (uint64_t)row * pitch;
    }
    if (hi - lo == pitch) {
        return (uint64_t)lo + (uint64_t)row * pitch;
    }
    return (uint64_t)((row & 1) ? hi : lo) + (uint64_t)(row / 2) * pitch;
}

/*
 * FillHandler, 0x3cac.  `CSurfaceControl::BuildFillCommand` lays the slot out
 * as position at +0, extent at +1, the destination base pair at +2/+3, the
 * pitch code at +4, the pixel format at +5 and - the part that matters - the
 * address of an 18-dword _FILLSURFACECMD block in Verite memory at +8.  The
 * colour is in that block, not in the slot.
 *
 * Section 6 calls FillSurface a hardware gradient generator: R/G/B/A each get
 * a 16.16 start plus dx, dy and d/vertical deltas.  Read off the running card
 * the block is
 *
 *     ffffffff ffffffff 00000000 00800000 00800000 00 00 ...
 *
 * - three channel starts of 0.0, 128.0 and 128.0 with every delta zero, which
 * as UYVY is Y=0, U=V=128: black, and byte for byte the 0x00800080 the YUV
 * display surface reads when nothing has drawn over it.  So the common case
 * is a flat fill, and that is what this does; a real gradient would need the
 * deltas, and nothing has been seen using them.
 *
 * 16 bpp only.  The RGB processor's fills have never been executed and its
 * picture is correct without them, so leaving that path alone avoids trading
 * a known-good grid for an unverified one.
 */
/*
 * Section 87.7.  Everything that writes a *displayed* framebuffer records
 * what it touched, so the console update can recompose that much and no
 * more.  A write to an offscreen surface - the border buffers the fills
 * build, for instance - changes nothing on screen until a later blit moves
 * it, and must not mark the console dirty.
 */
static bool stb_v2200_is_front(STBTulsaV2200 *s, uint32_t d0, uint32_t d1)
{
    return (d0 && (d0 == s->frame_base[0] || d0 == s->frame_base[1])) ||
           (d1 && (d1 == s->frame_base[0] || d1 == s->frame_base[1]));
}

static void stb_v2200_mark_dirty(STBTulsaV2200 *s, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h)
{
    uint32_t x1, y1;

    if (!w || !h || x >= STB_V2200_DISP_W || y >= STB_V2200_DISP_H) {
        return;
    }
    x1 = MIN(x + w, (uint32_t)STB_V2200_DISP_W);
    y1 = MIN(y + h, (uint32_t)STB_V2200_DISP_H);

    if (s->dirty_x1 <= s->dirty_x0) {           /* currently empty */
        s->dirty_x0 = x;  s->dirty_y0 = y;
        s->dirty_x1 = x1; s->dirty_y1 = y1;
        return;
    }
    s->dirty_x0 = MIN(s->dirty_x0, x);
    s->dirty_y0 = MIN(s->dirty_y0, y);
    s->dirty_x1 = MAX(s->dirty_x1, x1);
    s->dirty_y1 = MAX(s->dirty_y1, y1);
}

static void stb_v2200_fill(STBTulsaV2200 *s, const uint8_t *p)
{
    uint32_t a0 = ldl_le_p(p), a1 = ldl_le_p(p + 4);
    uint32_t dpair0 = ldl_le_p(p + 8), dpair1 = ldl_le_p(p + 12);
    uint32_t fmt = ldl_le_p(p + 20);
    uint32_t cmd = ldl_le_p(p + 32);
    uint32_t x = a0 >> 16, y = a0 & 0xffff;
    uint32_t w, h, stride, row, col;
    uint8_t pat[4];

    /*
     * The format word at +5 cannot be used to tell the two apart: this branch
     * of BuildFillCommand writes 12 unconditionally, and only the *other*
     * branch picks 4 or 12 from the surface's bit count.  The processor is
     * the honest discriminator - the YUV one's surfaces are 16 bpp - and
     * testing the format instead rejected exactly the fill this is for.
     */
    if (!s->saa || s != s->saa->yuv || !cmd ||
        cmd + 18 * 4 > STB_V2200_MEM_SIZE) {
        return;
    }
    (void)fmt;
    /*
     * This branch of BuildFillCommand stores both extents halved - the
     * hardware counts 16-bpp pixels in 32-bit units - where the other stores
     * only the height that way.
     */
    w = (a1 >> 16) * 2;
    h = (a1 & 0xffff) * 2;
    if (!w || !h || !dpair0 || !dpair1 || dpair0 == dpair1) {
        return;
    }
    stride = MAX(dpair0, dpair1) - MIN(dpair0, dpair1);
    if (!stride) {
        return;
    }
    {
        uint32_t c0 = ldl_le_p(s->vram + cmd + 8) >> 16;    /* Y */
        uint32_t c1 = ldl_le_p(s->vram + cmd + 12) >> 16;   /* U */
        uint32_t c2 = ldl_le_p(s->vram + cmd + 16) >> 16;   /* V */

        pat[0] = c1; pat[1] = c0; pat[2] = c2; pat[3] = c0;
    }
    w = MIN(w, STB_V2200_DISP_W - MIN(x, STB_V2200_DISP_W));
    h = MIN(h, STB_V2200_DISP_H - MIN(y, STB_V2200_DISP_H));

    for (row = 0; row < h; row++) {
        uint64_t off = stb_v2200_row_off(dpair0, dpair1, stride, y + row)
                     + (uint64_t)x * 2;

        if (off + (uint64_t)w * 2 > STB_V2200_MEM_SIZE) {
            return;
        }
        for (col = 0; col < w; col += 2) {
            memcpy(s->vram + off + (size_t)col * 2, pat, 4);
        }
    }
    if (stb_v2200_is_front(s, dpair0, dpair1)) {
        stb_v2200_mark_dirty(s, x, y, w, h);
    }
}

static void stb_v2200_blit(STBTulsaV2200 *s, const uint8_t *p, bool replay)
{
    uint32_t a0 = ldl_le_p(p),       a1 = ldl_le_p(p + 4);
    uint32_t a2 = ldl_le_p(p + 8),   a3 = ldl_le_p(p + 12);
    uint32_t sbase = ldl_le_p(p + 16);          /* +4  source surface   */
    uint32_t fmt = ldl_le_p(p + 40);            /* +10 12 = 32 bpp      */
    uint32_t scroll = ldl_le_p(p + 64);         /* +16 0 = none         */
    uint32_t aflags = ldl_le_p(p + 48);         /* +12 blend selector   */
    uint32_t amask = ldl_le_p(p + 60);          /* +15 dest write mask  */
    /*
     * Section 21.3: the alpha bits appear as 0x80000 plus either 0x230 or
     * 0x320, chosen by bInvertAlpha.  Only 0x320 has been observed, on the
     * one surface the preset blends (the grid) with bInvBlend FALSE, so
     * 0x320 is taken as normal and 0x230 as inverted.  That pairing is
     * inference; the 0x80000 gate and the 0x00ffffff mask are observed.
     */
    bool blend = (aflags & STB_V2200_BLT_ALPHA) != 0;
    bool invert = (aflags & STB_V2200_BLT_ALPHA) &&
                  (aflags & 0xff0) == STB_V2200_BLT_ALPHA_NEG;
    bool keep_dst_alpha = (amask & 0xff000000u) == 0;
    uint32_t sx = a0 >> 16, sy = a0 & 0xffff;
    uint32_t sw = a2 >> 16, sh = (a2 & 0xffff) * 2;
    uint32_t dx = a1 >> 16, dy = a1 & 0xffff;
    uint32_t dw = a3 >> 16, dh = (a3 & 0xffff) * 2;
    uint32_t spair0 = ldl_le_p(p + 16), spair1 = ldl_le_p(p + 20);
    uint32_t dpair0 = ldl_le_p(p + 28), dpair1 = ldl_le_p(p + 32);
    uint32_t dstride;
    uint32_t sstride, w, h, row, pos, bpp;
    bool cap_slot = false;

    /*
     * Section 85: a slot whose source is the SAA7146's capture buffer is the
     * video window, and it is the only thing that knows how big the scaled
     * picture is meant to be.  Tell the capture side, so it can size its
     * output to what will actually be read.
     */
    if (s->saa && s == s->saa->yuv && sbase &&
        (sbase == (s->saa->regs[STB_SAA7146_BASE_ODD / 4]
                   & (STB_V2200_MEM_SIZE - 1)) ||
         sbase == (s->saa->regs[STB_SAA7146_BASE_EVEN / 4]
                   & (STB_V2200_MEM_SIZE - 1)))) {
        s->saa->cap_w = sw;
        s->saa->cap_h = sh;
        cap_slot = true;
    }

    /*
     * 32 bpp is the RGB processor's graphics; 16 bpp is the YUV processor's
     * video, and skipping it is what kept every video-port slot from ever
     * executing.  Alpha lives in the 32 bpp pixel, so a 16 bpp slot is a
     * straight copy and the blend path does not apply to it.
     */
    if (fmt == STB_V2200_FMT_32BPP) {
        bpp = 4;
    } else if (fmt == STB_V2200_FMT_16BPP) {
        bpp = 2;
        blend = false;
    } else {
        return;
    }
    /*
     * Section 63.3/64.5: SetFrameBase and the overlay payload disagree by one
     * line about the destination origin.  The payload is the one the command
     * that uses it carries, so prefer it and keep SetFrameBase as the
     * fallback for slots that do not name a pair.
     */
    if (!dpair0 && !dpair1) {
        dpair0 = s->frame_base[0];      /* a slot that names no pair */
        dpair1 = s->frame_base[1];
    }
    /*
     * And the destination stride with it.  `shown_stride` is only ever set by
     * the console update callback, which runs for the processor that owns a
     * QemuConsole - the RGB one.  The YUV processor's stayed zero for ever,
     * so every video slot bailed here; the 32-bpp-only test above used to
     * reject them first, which is why it never showed.  The slot names both
     * field origins, and the gap between them is the stride, exactly as
     * SetFrameBase's pair gives it in section 53.1.
     */
    dstride = (dpair0 && dpair1 && dpair0 != dpair1)
            ? MAX(dpair0, dpair1) - MIN(dpair0, dpair1)
            : s->shown_stride;
    if (!dstride) {
        return;
    }
    if (!scroll && (sw != dw || sh != dh)) {
        return;                         /* true scaling: later stage */
    }
    /*
     * Surface pitch.  Section 80: the code at +9 *is* a stride after all -
     * two halves, the destination's in the high word and the source's in the
     * low - encoded as
     *
     *     pitch = ceil(stride_bytes / 32) + 3
     *
     * which holds for every surface measured: the 148-wide clock gives 22
     * against a 148-pixel stride, and the 720-wide grid, time bar and
     * primary all give 99 against the 768-pixel stride that SetFrameBase's
     * 0xc00 field gap proves and that the gaps between their base addresses
     * confirm.
     *
     * The ceiling loses the low five bits, so the field alone only brackets
     * the stride to ((p-4)*32, (p-3)*32].  The surface's own width settles
     * it: a width inside the bracket means the allocator packed it tight,
     * and a width below means it padded to the top of the bracket.  Both
     * measured cases fall out of that - 592 is inside 577..608 so the clock
     * is tight at 148 pixels, and 2880 is below 3041..3072 so the 720-wide
     * surfaces are padded to 768.
     *
     * Assuming 64-pixel alignment for everything, as this did before, gives
     * the 720-wide surfaces the right answer by luck and the clock 192
     * pixels against its real 148 - a 44-pixel shear per row, which is what
     * turned the on-screen clock into diagonal hash.
     */
    {
        uint32_t pitch = ldl_le_p(p + 36);          /* +9 */
        uint32_t code = pitch & 0xffff;             /* low half: the source */
        uint32_t wbytes = sw * bpp;

        sstride = 0;
        /*
         * Section 106.  `CSurfaceControl::BuildOverlayCommand` (STBTdde.dll)
         * builds this word as `f(dest lPitch) << 16 | f(source lPitch)` -
         * both halves through the *same* encoder - and writes the destination
         * base pair as `base + lPitch` and `base`, which is where dstride
         * above comes from.  So equal halves mean equal lPitch, and dstride
         * already is that pitch exactly, with nothing to decode.
         *
         * This matters because the encoder is per processor: section 80's
         * `ceil(bytes/32) + 3` is the *graphics* one and fits the RGB plane
         * (3072 -> 99, 592 -> 22), but the video processor's is a different
         * function (2048 -> 96, 1536 -> 82) and a bracket derived from the
         * graphics one is simply wrong for it.  The YUV border slots carry
         * 0x00600060 and were being given 2976 bytes of source stride against
         * a real 2048 - a 464-pixel shear per row, invisible only because the
         * surface they read has been blank (section 99.5).
         *
         * Restricted to 16 bpp deliberately: the graphics bracket already
         * gets the RGB plane right, so applying this there would change
         * working behaviour for nothing.
         */
        if (bpp == 2 && code && (pitch >> 16) == code) {
            sstride = dstride;
        } else if (code > 3) {
            uint32_t hi = (code - 3) * 32;
            uint32_t lo = (code - 4) * 32 + 1;

            sstride = (wbytes >= lo && wbytes <= hi) ? wbytes : hi;
        }
        if (sstride < wbytes) {
            sstride = ROUND_UP(sw, 64) * bpp;       /* nonsense code: fall back */
        }
        /*
         * For the capture buffer the part itself was told the pitch, and a
         * measured register beats a bracketed code: section 80's encoding is
         * only established for the 32-bpp graphics surfaces, and the two
         * 16-bpp points now in hand do not fit it.
         */
        if (cap_slot && s->saa->regs[STB_SAA7146_PITCH / 4]) {
            sstride = s->saa->regs[STB_SAA7146_PITCH / 4];
        }
    }

    w = MIN(dw, STB_V2200_DISP_W - MIN(dx, STB_V2200_DISP_W));
    h = MIN(dh, STB_V2200_DISP_H - MIN(dy, STB_V2200_DISP_H));

    /*
     * A scrolling slot is not a scale: the source is larger than the window
     * and the board walks through it, wrapping at the end -- section 25 saw
     * the seam pass through the frame with no discontinuity in motion, so it
     * is a modulo-source circular buffer, not a reset.
     */
    pos = 0;
    if (scroll) {
        uint32_t sstate = ldl_le_p(p + 68);     /* +17 */

        if (sstate && sstate + 4 <= STB_V2200_MEM_SIZE) {
            pos = ldl_le_p(s->vram + sstate + STB_SS_POSITION) >> 16;
            /*
             * The position is in frame rows, not the halved field lines the
             * slot stores its vertical extents in.  Section 81: the time bar
             * settles it - a 24-row bar in a 24-row window at position 24
             * selects the second bar exactly, while halving it to 12 splits
             * the window across both.  The barrier arithmetic that argued
             * for halving was reading a symptom: the position really does
             * run past where the host has drawn, but because the host is
             * drawing in the wrong place, not because the units are wrong.
             */
        }
    }

    for (row = 0; row < h; row++) {
        uint32_t srow = sy + row, scol = sx;
        uint64_t doff = stb_v2200_row_off(dpair0, dpair1, dstride, dy + row)
                      + dx * bpp;
        uint64_t soff;
        uint32_t run;

        if (scroll == STB_V2200_SCROLL_VERT && sh) {
            srow = (sy + pos + row) % sh;
        } else if (scroll == STB_V2200_SCROLL_HORZ && sw) {
            scol = (sx + pos) % sw;
        }
        soff = stb_v2200_row_off(spair0, spair1, sstride, srow)
             + (uint64_t)scol * bpp;

        /* a horizontal scroll can straddle the wrap: copy it in two runs */
        run = w;
        if (scroll == STB_V2200_SCROLL_HORZ && sw && scol + w > sw) {
            run = sw - scol;
        }
        if (soff + (uint64_t)run * bpp > STB_V2200_MEM_SIZE ||
            doff + (uint64_t)w * bpp > STB_V2200_MEM_SIZE) {
            return;
        }
        if (blend) {
            stb_v2200_blend_row(s->vram + doff, s->vram + soff, run,
                                invert, keep_dst_alpha);
        } else {
            memcpy(s->vram + doff, s->vram + soff, (size_t)run * bpp);
        }
        if (run < w) {
            uint64_t woff = stb_v2200_row_off(spair0, spair1, sstride, srow);

            if (woff + (uint64_t)(w - run) * bpp > STB_V2200_MEM_SIZE) {
                return;
            }
            if (blend) {
                stb_v2200_blend_row(s->vram + doff + (size_t)run * bpp,
                                    s->vram + woff, w - run,
                                    invert, keep_dst_alpha);
            } else {
                memcpy(s->vram + doff + (size_t)run * bpp, s->vram + woff,
                       (size_t)(w - run) * bpp);
            }
        }
    }
    if (stb_v2200_is_front(s, dpair0, dpair1)) {
        stb_v2200_mark_dirty(s, dx, dy, w, h);
    }
    if (!replay) {
        trace_stb_v2200_blit(s->role ?: "?", sbase, sw, sh, dx, dy, w, h);
    }
}

static void stb_v2200_run_list(STBTulsaV2200 *s, uint32_t addr, bool replay)
{
    const uint8_t *list;
    uint32_t seq;
    int i;

    if (addr + STB_V2200_CMDLIST_SZ > STB_V2200_MEM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stb-tulsa: v2200(%s) list address 0x%x out of range\n",
                      s->role ?: "?", addr);
        return;
    }

    list = s->vram + addr;
    seq = ldl_le_p(list);
    s->n_scroll = 0;
    if (!replay) {
        trace_stb_v2200_list_submit(s->role ?: "?", addr, seq);
    }

    for (i = 0; i < STB_V2200_SLOTS; i++) {
        const uint8_t *slot = list + 8 + i * STB_V2200_SLOT_DWORDS * 4;
        uint32_t handler = ldl_le_p(slot);

        if (!handler) {
            continue;
        }
        if (!replay) {
            char dw[18 * 9 + 1];
            int j;

            trace_stb_v2200_slot(s->role ?: "?", i, handler,
                                 stb_v2200_handler_name(handler));
            /*
             * The payload layout differs per handler and the named offsets
             * below are only known for the blit.  Dump the raw 18 dwords so
             * a handler whose layout is still being worked out - the fill,
             * section 86.6 - can be read straight out of the trace.
             */
            for (j = 0; j < 18; j++) {
                snprintf(dw + j * 9, 10, "%08x ",
                         ldl_le_p(slot + 16 + j * 4));
            }
            trace_stb_v2200_payload(s->role ?: "?", i, handler, dw);
        }

        /*
         * Slot layout is [0] handler, [1] BUILD_CRTC_DBG_VALUE, [2] 0,
         * [3] 0, [4..] payload (section 21.2).  SetFrameBase carries the
         * display bases; section 24.1 names it as setting the odd and even
         * base, which is the interlace showing through again.
         */
        /*
         * Overlay/video blits.  Section 21.3 gives the 18-dword payload that
         * follows the 4-dword slot header; every extent is stored halved
         * because the hardware works in fields.  Dump the real numbers before
         * writing a blitter against them -- assuming the layout is how the
         * framebuffer stride got guessed wrong in section 53.1.
         */
        if (stb_v2200_is_blit_handler(handler)) {
            const uint8_t *p = slot + 16;
            uint32_t p0 = ldl_le_p(p), p1 = ldl_le_p(p + 4);
            uint32_t p2 = ldl_le_p(p + 8), p3 = ldl_le_p(p + 12);

            if (!replay) {
                trace_stb_v2200_overlay_rect(s->role ?: "?", i,
                                             p0 & 0xffff, p0 >> 16,
                                             (p2 & 0xffff) * 2, p2 >> 16,
                                             p1 & 0xffff, p1 >> 16,
                                             (p3 & 0xffff) * 2, p3 >> 16);
                trace_stb_v2200_overlay_alpha(s->role ?: "?", i,
                                              ldl_le_p(p + 48),   /* +12 flags  */
                                              ldl_le_p(p + 52),   /* +13 const  */
                                              ldl_le_p(p + 56),   /* +14 mode   */
                                              ldl_le_p(p + 60));  /* +15 mask   */
                trace_stb_v2200_overlay_src(s->role ?: "?", i,
                                        ldl_le_p(p + 32),   /* +8  source   */
                                        ldl_le_p(p + 16),   /* +4  dst base */
                                        ldl_le_p(p + 36),   /* +9  pitches  */
                                        ldl_le_p(p + 40),   /* +10 format   */
                                        ldl_le_p(p + 56),   /* +14 mode     */
                                        ldl_le_p(p + 64),   /* +16 scroll   */
                                        ldl_le_p(p + 68));  /* +17 SCROLLSTATE */
            }
            /*
             * Remember the scroll blocks this list uses, so the field clock
             * knows what to advance.  Rebuilt per list: a surface that stops
             * scrolling drops out on the next one.
             */
            if (ldl_le_p(p + 64) && ldl_le_p(p + 68) &&
                s->n_scroll < STB_V2200_MAX_SCROLL) {
                uint32_t a2x = ldl_le_p(p + 8);
                bool vert = ldl_le_p(p + 64) == STB_V2200_SCROLL_VERT;

                s->scroll[s->n_scroll].addr = ldl_le_p(p + 68);
                s->scroll[s->n_scroll].extent =
                    vert ? (a2x & 0xffff) * 2 : a2x >> 16;
                s->n_scroll++;
            }

            stb_v2200_blit(s, p, replay);
        }

        if (handler == STB_V2200_UC_FILL) {
            stb_v2200_fill(s, slot + 16);
        }

        if (handler == STB_V2200_UC_SETFRAMEBASE) {
            s->frame_base[0] = ldl_le_p(slot + 16);
            s->frame_base[1] = ldl_le_p(slot + 20);
            if (!replay) {
                trace_stb_v2200_framebase(s->role ?: "?",
                                          s->frame_base[0], s->frame_base[1]);
            }
        }
    }

    if (replay) {
        return;
    }
    s->lists_submitted++;
    /*
     * With no microcode running, retire the list immediately: the host polls
     * both pointers to pick a buffer that is neither current nor active, and
     * would otherwise run out after three submissions.
     */
    s->cmd_active = addr;
}


/* ------------------------------------------------------------------ */
/* The low-level cores                                                 */
/*                                                                     */
/* The V2200 interpreter and Pixel Engine reach Verite memory through   */
/* these callbacks, so they see exactly the same 16 MiB the HLE path    */
/* and the host's PCI window do.  The core does its own big-endian      */
/* assembly on top of these, because the RISC is big-endian (spec 2.4)  */
/* and the microcode image is a big-endian ELF uploaded byte for byte.  */
/*                                                                      */
/* The HLE path above reads the same words with ldl_le_p and agrees     */
/* with what the driver writes, which is not a contradiction: section   */
/* 20 found that CTulsaDirectDraw's ReadVeriteMemory/WriteVeriteMemory  */
/* are endian-aware, taking LockEndianControlAccess and consulting      */
/* GetCurrentEndianness(), and that in mode 3 each dword is 16-bit      */
/* word-swapped on the way through.  The swap is the aperture's, under  */
/* driver control, not the core's.  Which means these callbacks are     */
/* right only while the board sits in whatever mode makes host dwords   */
/* land in RISC order - and the endian control register's location is   */
/* still section 20's open item 4.  Everything funnels through this one */
/* pair so that when the register turns up there is a single place to   */
/* honour it.                                                           */
/* ------------------------------------------------------------------ */

static bool stb_v2200_core_read(void *opaque, uint32_t addr, void *buf,
                                unsigned len)
{
    STBTulsaV2200 *s = opaque;

    if ((uint64_t)addr + len > STB_V2200_MEM_SIZE) {
        return false;
    }
    memcpy(buf, s->vram + addr, len);
    return true;
}

static bool stb_v2200_core_write(void *opaque, uint32_t addr, const void *buf,
                                 unsigned len)
{
    STBTulsaV2200 *s = opaque;

    if ((uint64_t)addr + len > STB_V2200_MEM_SIZE) {
        return false;
    }
    memcpy(s->vram + addr, buf, len);
    return true;
}

static void stb_v2200_pix_spr_cb(void *opaque, unsigned index, uint32_t val)
{
    STBTulsaV2200 *s = opaque;

    v2200_pix_spr(&s->pix, index, val);
}

static void stb_v2200_engine_cb(void *opaque, struct V2200Core *c,
                                unsigned op, unsigned d, unsigned s2,
                                unsigned s1)
{
    STBTulsaV2200 *s = opaque;

    v2200_pix_engine(&s->pix, c, op, d, s2, s1);
}

/*
 * Bring the interpreter up on the microcode the driver has already
 * uploaded into Verite memory through the RISC bootstrap.  Returns false
 * when there is nothing there to run, which is the normal state until the
 * guest's driver has loaded.
 */
static bool stb_v2200_lle_start(STBTulsaV2200 *s)
{
    if (s->lle_running) {
        return true;
    }
    if (!s->vram) {
        return false;
    }
    v2200_pix_reset(&s->pix);
    s->pix.read = stb_v2200_core_read;
    s->pix.write = stb_v2200_core_write;
    s->pix.opaque = s;

    /*
     * No FIFO callbacks: DispatchLoop tests FIFOInRdy in Flag before it
     * ever reads, so a core with none simply waits instead of halting.
     * Commands still arrive through the display list; wiring the real
     * command FIFO aperture is the next step.
     */
    s->core.read = stb_v2200_core_read;
    s->core.write = stb_v2200_core_write;
    s->core.pix_write = stb_v2200_pix_spr_cb;
    s->core.engine = stb_v2200_engine_cb;
    s->core.opaque = s;
    v2200_reset(&s->core, STB_V2200_UCODE_ENTRY);

    s->lle_running = true;
    return true;
}

/*
 * Run the card's own microcode over a submitted list.  The interrupt tail
 * reads the list pointer out of Verite memory at 0x6cf8, which the host
 * normally writes through the cmdptr aperture - that aperture overlays
 * this address, so the value never reaches the backing store and has to
 * be published here before the microcode can see it.
 */
static void stb_v2200_lle_list(STBTulsaV2200 *s, uint32_t addr)
{
    uint64_t budget = STB_V2200_LLE_BUDGET;

    if (!stb_v2200_lle_start(s)) {
        return;
    }
    stl_be_p(s->vram + STB_V2200_CMD_CURRENT, addr);

    /* Arm the field interrupt DispatchLoop is waiting on. */
    s->core.reg[V2200_R_RISCINTR] |= V2200_INTR_VERT | V2200_INTR_RISCINTR;

    while (budget-- && !s->core.halted) {
        if (!v2200_step(&s->core)) {
            break;
        }
    }
    if (s->core.halted) {
        qemu_log_mask(LOG_UNIMP,
                      "stb-tulsa: v2200(%s) microcode halted at %08x: %s\n",
                      s->role ?: "?", s->core.halt_pc,
                      s->core.halt_reason ?: "?");
        s->lle_running = false;
    memset(s->prof, 0, sizeof(s->prof));
    s->prof_window = 0;
    }
}

static void stb_v2200_decode_list(STBTulsaV2200 *s, uint32_t addr)
{
    int64_t t0 = stb_prof_begin();

    if (s->lle) {
        stb_v2200_lle_list(s, addr);
    } else {
        stb_v2200_run_list(s, addr, false);
    }
    /*
     * This ran inside the vCPU thread, holding the BQL, with the guest
     * stopped on the store to the cmdptr aperture that got us here.  So
     * this bucket is not "work the card did" - it is guest stall.
     */
    stb_prof_end(s, STB_PROF_LIST, t0);
}

/*
 * Interpret one force-stepped instruction.  STBTdde.dll drives the Vérité's
 * RISC core by stopping it, planting a single instruction in the 0x20064 latch
 * and pulsing the step bit, so this is the whole execution engine the
 * bootstrap needs - writeRF materialises values with load-immediates, and
 * risc_readmem / risc_writemem reach board memory through r254 (address),
 * r255 (value) and r252 (result).
 */
static void stb_v2200_risc_exec(STBTulsaV2200 *s, uint32_t insn)
{
    uint8_t op = insn >> 24;
    uint8_t rd = (insn >> 16) & 0xff;
    uint8_t rs = (insn >> 8) & 0xff;
    uint8_t ra = insn & 0xff;
    uint32_t addr;
    unsigned sz;

    switch (op) {
    case STB_V2200_OP_LDI:
        s->rf[rd] = insn & 0xffff;
        break;

    case STB_V2200_OP_LDI_HI:
        s->rf[rd] = (s->rf[rd] & 0xffff) | ((insn & 0xffff) << 16);
        break;

    case STB_V2200_OP_LOAD8:
    case STB_V2200_OP_LOAD8 + 1:
    case STB_V2200_OP_LOAD8 + 2:
        sz = 1u << (op - STB_V2200_OP_LOAD8);
        addr = s->rf[ra];
        if (addr + sz <= STB_V2200_MEM_SIZE) {
            s->rf[rd] = ldn_le_p(s->vram + addr, sz);
        }
        break;

    case STB_V2200_OP_STORE8:
    case STB_V2200_OP_STORE8 + 1:
    case STB_V2200_OP_STORE8 + 2:
        sz = 1u << (op - STB_V2200_OP_STORE8);
        addr = s->rf[ra];
        if (addr + sz <= STB_V2200_MEM_SIZE) {
            stn_le_p(s->vram + addr, sz, s->rf[rs]);
        }
        break;

    case STB_V2200_OP_JUMP:
        s->risc_pc = (insn & 0x00ffffff) << 2;
        break;

    default:
        /* Arithmetic and the rest: nothing observed depends on it yet. */
        break;
    }
    s->risc_steps++;
    trace_stb_v2200_risc_step(s->role ?: "?", insn, op);
}

static uint64_t stb_v2200_reg_read_inner(void *opaque, hwaddr addr,
                                         unsigned size)
{
    STBTulsaV2200 *s = opaque;
    uint32_t val;

    if (addr + size > STB_V2200_REG_SIZE) {
        return 0;
    }

    switch (addr) {
    case STB_V2200_FIFO_STAT:
        /*
         * The command FIFO's free-slot counter, in the low five bits.
         * v2kinit_context spins on
         *
         *     while ((*(BYTE *)(base + 0x20040) & 0x1f) < 4)
         *         ;
         *
         * before pushing a four-dword packet, and v2ksuspend_context does the
         * same.  Reporting zero free slots is what wedged the SDK sample at
         * 99% CPU with no window.  Nothing here queues, so the FIFO is never
         * anything but empty: report it permanently wide open.
         */
        val = STB_V2200_FIFO_FREE;
        break;

    case STB_V2200_RISC_STAT:
        /*
         * We do not run the Vérité's RISC core, so report it permanently
         * ready and stopped: v2krisc_stop waits for bits 3:2 and then for
         * bits 1:0, and would otherwise time out ten times over.
         */
        val = STB_V2200_RISC_STOPPED;
        break;

    case STB_V2200_RISC_CTRL:
        /*
         * risc_forcestep sets bits 1 and 2 and spins until (ctrl & 6) == 2,
         * i.e. until the step it asked for has retired.  We retire it in the
         * write handler, so just report it done.
         */
        s->regfile[STB_V2200_RISC_CTRL] &= ~0x04;
        val = s->regfile[STB_V2200_RISC_CTRL];
        break;

    case STB_V2200_RISC_DATA:
        /*
         * readRF selects a register by latching reg|0x10000000 here while the
         * mode byte reads 0x80, checks that back, then flips the mode to 0x82
         * and reads this register again - and that second read must return the
         * register file, not the latch.
         */
        switch (s->regfile[STB_V2200_RISC_MODE]) {
        case STB_V2200_RISC_MODE_RF:
            val = s->rf[s->rf_sel];
            break;
        case STB_V2200_RISC_MODE_PC:
            /*
             * v1kload_uc_file writes 0x81 here and then spins until this
             * register reads the address the bootstrap was placed at, which
             * is where v1krisc_start's final jump sent the core.
             */
            val = s->risc_pc;
            break;
        default:
            val = ldn_le_p(s->regfile + addr, size);
            break;
        }
        break;

    default:
        val = ldn_le_p(s->regfile + addr, size);
        break;
    }

    trace_stb_v2200_reg_read(s->role ?: "?", addr, val);
    return val;
}

static void stb_v2200_reg_write_inner(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    STBTulsaV2200 *s = opaque;

    if (addr + size > STB_V2200_REG_SIZE) {
        return;
    }

    /* Everything latches, because the software checks it by reading back. */
    stn_le_p(s->regfile + addr, size, val);

    if (addr == STB_V2200_RISC_DATA && (val & STB_V2200_RF_SELECT)) {
        s->rf_sel = val & 0xff;
    }

    /* Setting both the stop and step bits retires one instruction. */
    if (addr == STB_V2200_RISC_CTRL && (val & 0x06) == 0x06) {
        stb_v2200_risc_exec(s, ldl_le_p(s->regfile + STB_V2200_RISC_DATA));
    }

    trace_stb_v2200_reg_write(s->role ?: "?", addr, val);
}

static uint64_t stb_v2200_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    STBTulsaV2200 *s = opaque;
    int64_t t0 = stb_prof_begin();
    uint64_t r = stb_v2200_reg_read_inner(opaque, addr, size);

    stb_prof_end(s, STB_PROF_REG, t0);
    return r;
}

static void stb_v2200_reg_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    STBTulsaV2200 *s = opaque;
    int64_t t0 = stb_prof_begin();

    stb_v2200_reg_write_inner(opaque, addr, val, size);
    stb_prof_end(s, STB_PROF_REG, t0);
}

static const MemoryRegionOps stb_v2200_reg_ops = {
    .read = stb_v2200_reg_read,
    .write = stb_v2200_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * The microcode loader kicks this aperture with a single byte store, so
     * 4-byte-only access would drop it on the floor.
     */
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * The submit and retire pointers live in Verite *memory*, not in the
 * register aperture, so they are caught with a small MMIO window overlaid on
 * the framebuffer rather than by trapping the whole 16 MiB.
 */
static uint64_t stb_v2200_cmdptr_read(void *opaque, hwaddr addr, unsigned size)
{
    STBTulsaV2200 *s = opaque;

    return addr == 0 ? s->cmd_current : s->cmd_active;
}

static void stb_v2200_cmdptr_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    STBTulsaV2200 *s = opaque;

    if (addr == 0) {
        s->cmd_current = val;
        stb_v2200_decode_list(s, val);
    } else {
        s->cmd_active = val;
    }
}

static const MemoryRegionOps stb_v2200_cmdptr_ops = {
    .read = stb_v2200_cmdptr_read,
    .write = stb_v2200_cmdptr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static void stb_uyvy_black(uint8_t *p, size_t bytes);

static void stb_v2200_reset_hold(Object *obj, ResetType type)
{
    STBTulsaV2200 *s = STB_TULSA_V2200(obj);

    memset(s->regfile, 0, sizeof(s->regfile));
    memset(s->rf, 0, sizeof(s->rf));
    s->risc_pc = 0;
    s->risc_steps = 0;
    s->rf_sel = 0;
    s->cmd_current = 0;
    s->cmd_active = 0;
    s->lists_submitted = 0;
    s->lle_running = false;
    if (s->vram) {
        /*
         * Zero is black in RGB but *saturated green* in UYVY, where black is
         * `80 00 80 00`.  Section 88.4: slots 3 and 4 blit a YUV surface at
         * 0x0a9000/0x25800 that nothing in the model ever writes, so they
         * were copying this memset straight to screen as a green bar across
         * the top of the window and a green column down its left edge.
         *
         * Who writes that surface on real hardware is still unanswered
         * (section 88.6 item 1).  But whatever the answer, uninitialised
         * video memory reading as green is wrong on its own terms - the card
         * does not power up green - so the YUV processor's memory starts as
         * UYVY black and the RGB processor's stays zeroed, which is black
         * there.
         */
        if (s->role && !strcmp(s->role, "yuv")) {
            stb_uyvy_black(s->vram, STB_V2200_MEM_SIZE);
        } else {
            memset(s->vram, 0, STB_V2200_MEM_SIZE);
        }
    }
}

/*
 * Tracing overlay over the microcode's control structures.
 *
 * Reads and writes are forwarded to the same backing RAM, so behaviour is
 * unchanged; the only effect is that the accesses become visible.  Section
 * 40.2 did exactly this for the ATU aperture, and for the same reason: an
 * untraced RAM region cannot be distinguished from one nobody touches, and
 * sections 46.4 and 48.1 both concluded "the main thread touches no MMIO"
 * when what they had actually established was that the Verite framebuffer
 * BAR is not instrumented.
 */
static uint64_t stb_v2200_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    STBTulsaV2200 *s = opaque;
    hwaddr off = STB_V2200_CTRL_BASE + addr;
    uint32_t val = ldn_le_p(s->vram + off, size);
    uint64_t pc = stb_guest_pc();

    /*
     * Retire a flicker-filter command on the first poll of its line count.
     *
     * CSurfaceControl::CheckSurfaceForCompleteFFCommand reads the 14-dword
     * block and completes when word 7 is zero; CCardDlg then leaves the
     * unbounded "do { GetFlickerFilterStatus(); Sleep(10); } while
     * (!bFinished)" in carddlg.cpp that otherwise hangs the main thread.  On
     * real hardware the FlickerFilter microcode handler decrements word 7
     * once per field.  Nothing here executes microcode, so the honest
     * stand-in is to complete it immediately -- the same policy the display
     * list already uses when it retires on submit.
     *
     * Keyed off word 0 of the same block being set, so this only fires on a
     * block the host has actually armed.
     */
    if (size == 4 && val != 0 &&
        addr >= STB_V2200_FFCMD_LINES &&
        (addr - STB_V2200_FFCMD_LINES) % sizeof(uint32_t) == 0) {
        hwaddr base = off - STB_V2200_FFCMD_LINES;

        if (ldl_le_p(s->vram + base + STB_V2200_FFCMD_ACTIVE) == 1) {
            stl_le_p(s->vram + off, 0);
            trace_stb_v2200_ff_retire(s->role ?: "?", base);
            val = 0;
        }
    }

    trace_stb_v2200_mem_access(s->role ?: "?", "rd", off, size, val, pc);
    return val;
}

static void stb_v2200_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    STBTulsaV2200 *s = opaque;
    hwaddr off = STB_V2200_CTRL_BASE + addr;

    trace_stb_v2200_mem_access(s->role ?: "?", "wr", off, size, val,
                               stb_guest_pc());
    stn_le_p(s->vram + off, size, val);
}

static const MemoryRegionOps stb_v2200_ctrl_ops = {
    .read = stb_v2200_ctrl_read,
    .write = stb_v2200_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Present the card's own framebuffer.
 *
 * This is deliberately the crudest possible view -- whatever SetFrameBase
 * last pointed at, read straight out of Verite memory -- because its job is
 * to be an instrument, not a renderer.  Section 24.7 gives an exact
 * reference for what should eventually appear (prevue.bmp, the real
 * 4 February 1999 Nashville grid) and section 25 is a capture of a real card
 * compositing it, so a wrong picture here is checkable against ground truth
 * in a way nothing else in this model is.
 *
 * Compositing the overlay slots, the scroll and the alpha modes come after;
 * see section 52.3 for the staging and the known hazards (field vs frame
 * above all).
 */
/*
 * One YUV 4:2:2 pixel out of the YUV processor's framebuffer.
 *
 * Byte order is **UYVY**: U, Y0, V, Y1.
 *
 * This was first read as YUYV, on the argument that a cleared framebuffer of
 * `00 80 00 80` is black at neutral chroma under YUYV and a mid-grey with
 * both chroma channels pinned to opposite extremes under UYVY -- "not a value
 * anything clears to."  The argument was wrong because the premise was: the
 * preset's generated pattern is not greyscale at all.  Decoding a dump of the
 * framebuffer both ways settles it in one look -- UYVY gives two clean flat
 * triangles of saturated green and cyan, YUYV gives a one-pixel-wide stripe
 * pattern.  Saturated colour means chroma extremes are exactly what belongs
 * in those bytes.
 *
 * Chroma is shared across each even/odd pair, so it comes from the pair's
 * base rather than from the pixel.
 */
static void stb_v2200_yuyv_pixel(const uint8_t *row, uint32_t x,
                                 int *r, int *g, int *b)
{
    const uint8_t *pair = row + (x & ~1u) * 2;
    int y = row[x * 2 + 1];
    int u = pair[0] - 128;
    int v = pair[2] - 128;
    int rr, gg, bb;

    /* BT.601, 16.16 fixed point. */
    rr = y + ((91881 * v) >> 16);
    gg = y - ((22554 * u + 46802 * v) >> 16);
    bb = y + ((116130 * u) >> 16);

    *r = CLAMP(rr, 0, 255);
    *g = CLAMP(gg, 0, 255);
    *b = CLAMP(bb, 0, 255);
}

static bool stb_v2200_gfx_update_inner(void *opaque)
{
    STBTulsaV2200 *s = opaque;
    STBTulsaV2200 *v = s->peer;
    uint32_t even = s->frame_base[0], odd = s->frame_base[1];
    uint32_t base = MIN(even, odd);
    uint32_t stride = even > odd ? even - odd : odd - even;
    uint32_t vbase = 0, vstride = 0;
    uint32_t dx0, dy0, dx1, dy1;
    bool full = false;
    size_t need;
    int y;

    /*
     * The two bases SetFrameBase publishes are the first line of each field,
     * so the frame is interleaved and the gap between them is one line.
     * Observed: even=0x027000 odd=0x026400, a 0xc00 = 3072-byte stride --
     * 768 pixels at 32bpp for a 720-pixel raster, i.e. padded, which is why
     * assuming width*bpp gives a sheared picture.
     */
    /*
     * Always return true.  ui/console.c reads a false return as "this device
     * will finish the update asynchronously and call
     * qemu_console_hw_update_done() itself"; this one is synchronous, and
     * returning false for "nothing to show yet" hangs screendump on the
     * console's dump queue forever.
     */
    if (!stride) {
        return true;            /* SetFrameBase has not run yet */
    }
    need = (size_t)stride * STB_V2200_DISP_H;
    if (base + need > STB_V2200_MEM_SIZE) {
        return true;            /* not pointed anywhere sane yet */
    }
    if (s->shown_base != base || s->shown_stride != stride) {
        full = true;            /* every pixel's source just moved */
    }
    s->shown_base = base;
    s->shown_stride = stride;

    /*
     * The mixer.  Section 2: "the mixer is controlled by the RGB processor's
     * alpha channel.  Per-pixel.  That is how the guide sits over live
     * video."  Section 5(e) adds the part that makes the polarity read
     * backwards until you have it:
     *
     *     sde.bInvertAlpha = TRUE;   // note: primary runs inverted alpha
     *
     * so on the primary an alpha of 0xff means *show video* and 0x00 means
     * *show graphics*.  Measured in the running card's framebuffer, the
     * title overlay's magenta background is 0xff and the listings grid is
     * 0x00 -- the magenta is a placeholder under a fully transparent region,
     * never a colour anyone was meant to see, and the grid is solid
     * graphics.  Showing Verite memory raw, as the console did before this,
     * is showing one mixer input rather than the card's output.
     */
    if (!s->mix) {
        DisplaySurface *ds;

        full = true;
        s->mix = g_malloc0((size_t)STB_V2200_DISP_W * STB_V2200_DISP_H * 4);
        ds = qemu_create_displaysurface_from(STB_V2200_DISP_W,
                                             STB_V2200_DISP_H,
                                             PIXMAN_x8r8g8b8,
                                             STB_V2200_DISP_W * 4,
                                             s->mix);
        qemu_console_set_surface(s->con, ds);
    }

    /* The other mixer input, if the YUV processor has published a frame. */
    if (v && v->vram) {
        uint32_t ve = v->frame_base[0], vo = v->frame_base[1];

        vstride = ve > vo ? ve - vo : vo - ve;
        vbase = MIN(ve, vo);
        /*
         * Section 99.16.  The YUV plane's origin is not the first *visible*
         * pixel - it is the first pixel of the line, blanking included - while
         * the RGB plane's base is the visible origin.  Three successive
         * startup rectangles for the video slot prove it: Hollywood shrinks
         * the picture about a fixed centre, and that centre is x = 442.5 in
         * all three, against 360 for a 720-pixel raster.  The boot rectangle
         * then pins the origin without any fitting - "full screen" is
         * programmed as 82..802, so subtracting 82 puts it at 0..720.
         *
         *   seq 2   dst x  82  w 721   centre 442.5   <- full screen
         *   seq 3   dst x 226  w 433   centre 442.5
         *   seq 4   dst x 182  w 521   centre 442.5   <- steady state
         *
         * Vertically the same argument gives 26, which is an NTSC VBI's worth
         * of lines and lands the steady-state picture inside the window the
         * RGB plane keys out instead of overhanging it.
         *
         * Correcting it here rather than in the blit matters: the RGB
         * processor's own slots legitimately sit at dst (0,0) and must not
         * move.  Only the plane whose base includes blanking gets shifted.
         */
        vbase += (size_t)STB_V2200_BLANK_Y * vstride
               + (size_t)STB_V2200_BLANK_X * 2;        /* 16 bpp */
        if (!vstride ||
            vbase + (size_t)vstride * STB_V2200_DISP_H
                  + (size_t)STB_V2200_DISP_W * 2 > STB_V2200_MEM_SIZE) {
            vstride = 0;
        }
    }

    /*
     * Section 87.7.  Recompose what moved, not the whole frame.  The two
     * processors each record the destination rectangles their blits and
     * fills wrote to a displayed framebuffer; the union is the damage.  A
     * geometry change or a fresh surface forces the full walk, because then
     * nothing about the previous contents can be trusted.
     *
     * With live video the YUV side is dirty on every field, so the win here
     * is spatial rather than a skipped frame: the video window is a fraction
     * of the raster, and the guide around it is static between redraws.
     */
    if (full) {
        dx0 = 0;
        dy0 = 0;
        dx1 = STB_V2200_DISP_W;
        dy1 = STB_V2200_DISP_H;
    } else {
        dx0 = STB_V2200_DISP_W;
        dy0 = STB_V2200_DISP_H;
        dx1 = 0;
        dy1 = 0;
        if (s->dirty_x1 > s->dirty_x0) {
            dx0 = MIN(dx0, s->dirty_x0);
            dy0 = MIN(dy0, s->dirty_y0);
            dx1 = MAX(dx1, s->dirty_x1);
            dy1 = MAX(dy1, s->dirty_y1);
        }
        if (v && v->dirty_x1 > v->dirty_x0) {
            dx0 = MIN(dx0, v->dirty_x0);
            dy0 = MIN(dy0, v->dirty_y0);
            dx1 = MAX(dx1, v->dirty_x1);
            dy1 = MAX(dy1, v->dirty_y1);
        }
        if (dx1 <= dx0 || dy1 <= dy0) {
            return true;        /* nothing moved; the last frame still stands */
        }
    }
    s->dirty_x0 = s->dirty_y0 = s->dirty_x1 = s->dirty_y1 = 0;
    if (v) {
        v->dirty_x0 = v->dirty_y0 = v->dirty_x1 = v->dirty_y1 = 0;
    }

    for (y = dy0; y < (int)dy1; y++) {
        const uint8_t *gr = s->vram + base + (size_t)y * stride;
        const uint8_t *vr = vstride ? v->vram + vbase + (size_t)y * vstride
                                    : NULL;
        uint8_t *out = s->mix + (size_t)y * STB_V2200_DISP_W * 4;
        int x;

        for (x = dx0; x < (int)dx1; x++) {
            uint32_t gp = ldl_le_p(gr + x * 4);
            /*
             * `a` is always the *video* weight.  With the primary running
             * inverted alpha that is the stored byte; otherwise it is its
             * complement, so an opaque guide pixel (0xff) keys to graphics.
             */
            uint32_t a = s->alpha_invert ? (gp >> 24) : 255 - (gp >> 24);
            int vrd = 0, vgn = 0, vbl = 0;
            uint32_t px;

            if (a == 0) {                   /* graphics only: the common case */
                stl_le_p(out + x * 4, gp & 0x00ffffffu);
                continue;
            }
            if (vr) {
                stb_v2200_yuyv_pixel(vr, x, &vrd, &vgn, &vbl);
            }
            if (a == 255) {                 /* video only */
                px = ((uint32_t)vrd << 16) | ((uint32_t)vgn << 8) | vbl;
            } else {
                uint32_t ia = 255 - a;

                px = ((((gp >> 16) & 0xff) * ia + (uint32_t)vrd * a) / 255) << 16;
                px |= ((((gp >> 8) & 0xff) * ia + (uint32_t)vgn * a) / 255) << 8;
                px |= (((gp & 0xff) * ia + (uint32_t)vbl * a) / 255);
            }
            stl_le_p(out + x * 4, px);
        }
    }

    qemu_console_update(s->con, dx0, dy0, dx1 - dx0, dy1 - dy0);
    return true;
}

/*
 * These three all have several returns, so they are wrapped rather than
 * instrumented in place - one timing pair each, no missed exits.
 */
static bool stb_v2200_gfx_update(void *opaque)
{
    STBTulsaV2200 *s = opaque;
    int64_t t0 = stb_prof_begin();
    bool r = stb_v2200_gfx_update_inner(opaque);

    stb_prof_end(s, STB_PROF_COMPOSITE, t0);
    return r;
}

static const GraphicHwOps stb_v2200_gfx_ops = {
    .gfx_update = stb_v2200_gfx_update,
};

static void stb_v2200_realize(PCIDevice *pdev, Error **errp)
{
    STBTulsaV2200 *s = STB_TULSA_V2200(pdev);
    g_autofree char *memname = g_strdup_printf("stb-v2200-%s-mem",
                                               s->role ?: "x");
    g_autofree char *regname = g_strdup_printf("stb-v2200-%s-reg",
                                               s->role ?: "x");
    g_autofree char *ptrname = g_strdup_printf("stb-v2200-%s-cmdptr",
                                               s->role ?: "x");

    pci_config_set_interrupt_pin(pdev->config, 1);

    memory_region_init_ram(&s->mem, OBJECT(s), memname,
                           STB_V2200_MEM_SIZE, &error_fatal);
    s->vram = memory_region_get_ram_ptr(&s->mem);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mem);

    memory_region_init_io(&s->reg, OBJECT(s), &stb_v2200_reg_ops, s,
                          regname, STB_V2200_REG_SIZE);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->reg);

    /*
     * Trap just the eight bytes holding OverlayCmdPtrCurrent and
     * OverlayCmdPtrActive, overlaid on the framebuffer.  Everything else in
     * the 16 MiB aperture stays plain RAM, so building a display list costs
     * nothing and only the submit doorbell exits.
     */
    /*
     * Unlike the ATU aperture, the Verite's is not windowed, so this overlay
     * is pure instrumentation and can be switched off when a run cares more
     * about speed than visibility.  It stays on by default: it is what found
     * the flicker-filter stall in section 49, and the traffic it carries --
     * one 14-dword read per monitor pass -- is cheap.
     */
    if (s->mmio_trace) {
        memory_region_init_io(&s->ctrltrace, OBJECT(s), &stb_v2200_ctrl_ops, s,
                              "stb-v2200-ctrltrace", STB_V2200_CTRL_SIZE);
        memory_region_add_subregion_overlap(&s->mem, STB_V2200_CTRL_BASE,
                                            &s->ctrltrace, 1);
    }

    /*
     * Only the RGB processor composites the guide; the YUV side is the video
     * framebuffer and gets no console of its own for now.
     */
    if (s->role && !strcmp(s->role, "rgb")) {
        s->con = qemu_graphic_console_create(DEVICE(s), 0, &stb_v2200_gfx_ops, s);
        qemu_console_resize(s->con, STB_V2200_DISP_W, STB_V2200_DISP_H);
    }

    s->field_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stb_v2200_scroll_tick, s);
    s->field_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->field_timer, stb_field_next(&s->field_deadline));

    /*
     * The running totals, so a session can read them live over QMP
     * without waiting for a report window:
     *   qom-get path=/machine/peripheral/tulsa-rgb property=prof-list-ns
     */
    {
        static const char *const pname[STB_PROF_MAX] = {
            "prof-list-ns", "prof-field-ns", "prof-composite-ns",
            "prof-reg-ns"
        };
        static const char *const cname[STB_PROF_MAX] = {
            "prof-list-calls", "prof-field-calls", "prof-composite-calls",
            "prof-reg-calls"
        };
        unsigned i;

        for (i = 0; i < STB_PROF_MAX; i++) {
            object_property_add_uint64_ptr(OBJECT(s), pname[i],
                                           &s->prof[i].ns_total,
                                           OBJ_PROP_FLAG_READ);
            object_property_add_uint64_ptr(OBJECT(s), cname[i],
                                           &s->prof[i].calls_total,
                                           OBJ_PROP_FLAG_READ);
        }
    }

    memory_region_init_io(&s->cmdptr, OBJECT(s), &stb_v2200_cmdptr_ops, s,
                          ptrname, 8);
    memory_region_add_subregion_overlap(&s->mem, STB_V2200_CMD_CURRENT,
                                        &s->cmdptr, 2);
}

static const Property stb_v2200_properties[] = {
    DEFINE_PROP_STRING("role", STBTulsaV2200, role),
    DEFINE_PROP_BOOL("mmio-trace", STBTulsaV2200, mmio_trace, true),
    DEFINE_PROP_BOOL("alpha-invert", STBTulsaV2200, alpha_invert, false),
    DEFINE_PROP_BOOL("lle", STBTulsaV2200, lle, false),
};

static void stb_v2200_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = stb_v2200_reset_hold;
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = stb_v2200_realize;
    k->vendor_id = PCI_VENDOR_ID_RENDITION;
    k->device_id = PCI_DEVICE_ID_RENDITION_V2200;
    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    k->revision  = 0x01;

    device_class_set_props(dc, stb_v2200_properties);
    dc->desc = "Rendition Verite V2200 (STB Tulsa)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* ================================================================== *
 * Philips SAA7146
 * ================================================================== */


/*
 * The analog capture path: frames in, Verite memory out.
 *
 * Section 85.  The part digitises the BNC input, *scales* it, and DMAs the
 * result into a capture buffer whose address it is given explicitly - it does
 * not write the display surface.  The Verite's own display list then blits
 * that buffer into the window with a 0x30a0 VideoScaleHandler slot, which is
 * why no slot ever carries mismatched extents: the scaling already happened
 * here.
 *
 * `STBTdde.dll!Enable7146VideoPortYUVProc` programs six dwords at 0x00 and
 * eight at 0x5c and nothing else, and every value in a register dump off the
 * running card is accounted for by it - including the two field bases, which
 * match the source pair of the display-list slot that reads them byte for
 * byte.  So the honest model is to put the picture where those registers say
 * and let the list do the rest.
 *
 * A frame arrives every other field, which is 29.97 a second against the
 * 59.94 field clock - broadcast NTSC, and what an MPEG-2 feed decodes to.
 */
/*
 * UYVY black is `80 00 80 00`, not zero.  All-zero UYVY is saturated green,
 * which is exactly what section 99 found sitting in the capture buffer's
 * margin and being blitted to screen as a green bar.  Anything in this
 * buffer that is not picture has to be painted, not left.
 */
static void stb_uyvy_black(uint8_t *p, size_t bytes)
{
    size_t i;

    for (i = 0; i + 3 < bytes; i += 4) {
        p[i + 0] = 0x80;        /* U  */
        p[i + 1] = 0x00;        /* Y0 */
        p[i + 2] = 0x80;        /* V  */
        p[i + 3] = 0x00;        /* Y1 */
    }
    for (; i < bytes; i++) {
        p[i] = (i & 1) ? 0x00 : 0x80;
    }
}

static void stb_saa7146_deliver(STBTulsaSAA7146 *s)
{
    STBTulsaV2200 *v = s->yuv;
    uint32_t odd, even, pitch, w, h, y;

    if (!v || !v->vram) {
        return;
    }
    /*
     * Nothing is captured until the host enables it.  MC1 bit 0x40 is the
     * gate: Enable7146VideoPortYUVProc ends by setting it and
     * Disable7146VideoPort does nothing but clear it.  Without this the model
     * paints video from the moment the card is up, which is why the emulated
     * unit showed a picture before the border rather than after it.
     */
    if (!(s->mc1 & STB_SAA7146_MC1_CAPTURE)) {
        return;
    }

    /* The bases are PCI addresses; the Verite's BAR is 16 MiB and aligned. */
    odd   = s->regs[STB_SAA7146_BASE_ODD  / 4] & (STB_V2200_MEM_SIZE - 1);
    even  = s->regs[STB_SAA7146_BASE_EVEN / 4] & (STB_V2200_MEM_SIZE - 1);
    pitch = s->regs[STB_SAA7146_PITCH     / 4];

    /*
     * How big the scaled picture is cannot be read back out of the scaler
     * registers alone - they encode the ratio between a source crop and a
     * destination size, and the source crop lives in a DDVIDEOPORTINFO the
     * card never sees.  The consumer knows, though: the display-list slot
     * that reads these very bases states its own source extent, and
     * stb_v2200_blit records it here.  That couples the two the same way the
     * hardware couples them - the part writes where the list reads - and it
     * follows the host automatically when the window changes.
     */
    w = s->cap_w;
    h = s->cap_h;
    if (!w || !h || !pitch || w * 2 > pitch) {
        return;                 /* no consumer yet, or nonsense geometry */
    }
    if ((uint64_t)MAX(odd, even) + (uint64_t)(h / 2) * pitch > STB_V2200_MEM_SIZE) {
        return;
    }

    /*
     * Section 99.  The picture is `w` pixels wide; the buffer row is `pitch`
     * bytes, and here that is 768 px against a 521-px picture.  The loop
     * below writes only the picture, so on a geometry change the rest of the
     * row keeps whatever the previous, wider geometry left there, and the
     * part no geometry ever reached stays all-zero - green.  Repaint the
     * whole extent black whenever the geometry moves, so the only thing in
     * the buffer afterwards is this frame's picture and black.
     */
    if (w != s->last_w || h != s->last_h || pitch != s->last_pitch ||
        odd != s->last_odd || even != s->last_even) {
        size_t span = (size_t)(h / 2) * pitch;

        stb_uyvy_black(v->vram + even, span);
        stb_uyvy_black(v->vram + odd, span);
        s->last_w     = w;
        s->last_h     = h;
        s->last_pitch = pitch;
        s->last_odd   = odd;
        s->last_even  = even;
    }

    /*
     * Scale the incoming raster into the capture buffer, nearest neighbour,
     * and split it into the two field buffers.  They are not interleaved the
     * way the display surface is - the bases are 240 lines of pitch apart,
     * so each field is a contiguous block of its own.
     */
    for (y = 0; y < h; y++) {
        uint32_t base = (y & 1) ? odd : even;
        const uint8_t *src = s->frame +
            (size_t)(y * STB_V2200_DISP_H / h) * STB_VIDEO_BPL;
        uint8_t *dst = v->vram + base + (size_t)(y / 2) * pitch;
        uint32_t x;

        /*
         * And the margin past the picture, every frame - the display list
         * reads the full row, so a stale or unwritten tail is visible.
         */
        if ((size_t)w * 2 < pitch) {
            stb_uyvy_black(dst + (size_t)w * 2, pitch - (size_t)w * 2);
        }

        for (x = 0; x < w; x += 2) {
            /*
             * UYVY pairs share chroma, so step two pixels at a time and take
             * both luma samples from their own source columns.
             */
            uint32_t sx0 = (x * STB_V2200_DISP_W / w) & ~1u;
            uint32_t sx1 = ((x + 1) * STB_V2200_DISP_W / w);

            dst[x * 2 + 0] = src[sx0 * 2 + 0];          /* U  */
            dst[x * 2 + 1] = src[sx0 * 2 + 1];          /* Y0 */
            dst[x * 2 + 2] = src[sx0 * 2 + 2];          /* V  */
            dst[x * 2 + 3] = src[(sx1 | 1) * 2 + 1];    /* Y1 */
        }
    }
    s->frames_in++;
    trace_stb_saa7146_frame(even, pitch, s->frames_in);
}

static void stb_saa7146_accept(STBTulsaSAA7146 *s);
static void stb_saa7146_open_source(STBTulsaSAA7146 *s, Error **errp);

/*
 * The last socket error, normalised.  `recv` here is the platform call, not
 * a QEMU wrapper, so on Windows the error is in WSAGetLastError() and errno
 * is untouched; getting this wrong is what made a reset producer look like
 * a would-block and hang the input for good.
 */
static int stb_sock_err(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();

    switch (e) {
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAEINTR:
        return EINTR;
    default:
        return e ? e : EIO;
    }
#else
    return errno;
#endif
}

/*
 * Closing a socket is not closing a file descriptor on Windows, where the
 * value in video_fd is a SOCKET rather than a CRT handle.  There is no
 * portable QEMU wrapper for this (`closesocket` is winsock's own and does
 * not exist on POSIX), which is what stopped the Linux/KVM build dead -
 * section 115.
 */
static void stb_sock_close(int fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static void stb_saa7146_tick(void *opaque)
{
    STBTulsaSAA7146 *s = opaque;

    stb_saa7146_accept(s);

    if (s->video_fd >= 0) {
        /*
         * One frame's worth at most per tick, non-blocking, carrying a
         * partial frame across ticks.  A producer that cannot keep up leaves
         * the last frame on screen; it never blocks the CPU thread.
         */
        while (s->fill < STB_VIDEO_BYTES) {
            /*
             * A socket is not a CRT file descriptor on Windows, so the two
             * sources need different calls.  Getting this wrong fails only at
             * run time, and silently - read() on a SOCKET returns -1/EBADF.
             */
            ssize_t n = s->video_is_socket
                ? recv(s->video_fd, (char *)s->frame + s->fill,
                       STB_VIDEO_BYTES - s->fill, 0)
                : read(s->video_fd, s->frame + s->fill,
                       STB_VIDEO_BYTES - s->fill);

            if (n > 0) {
                s->fill += n;
                continue;
            }
            if (n == 0 && !s->video_is_pipe) {
                lseek(s->video_fd, 0, SEEK_SET);   /* a file loops */
                continue;
            }
            /*
             * A producer that is killed rather than closed sends RST, and
             * then recv returns -1/ECONNRESET instead of 0.  Handling only
             * the graceful case left `video_is_socket` set for ever: the
             * accept path returns early while a producer is held, so the
             * listener stopped accepting, its backlog filled, and every
             * later connection was refused until QEMU was restarted.  That
             * is what "can't pipe video in" looked like.
             */
            if (n == 0 || (n < 0 && s->video_is_socket &&
                           stb_sock_err() != EAGAIN &&
                           stb_sock_err() != EINTR)) {
                if (!s->video_is_socket) {
                    break;      /* a pipe at end of stream */
                }
                /*
                 * Producer gone.  Fall back to the standby file if there is
                 * one, otherwise hold the last frame and wait for the next
                 * producer - which is what an input losing its feed looks
                 * like on the real card.
                 */
                stb_sock_close(s->video_fd);
                s->video_fd = -1;
                s->video_is_socket = false;
                s->fill = 0;
                trace_stb_saa7146_source("producer gone");
                stb_saa7146_open_source(s, NULL);
            }
            break;              /* EAGAIN, or a pipe at end of stream */
        }
        if (s->fill == STB_VIDEO_BYTES) {
            s->fill = 0;
            stb_saa7146_deliver(s);
        }
    }

    /* Two fields to a frame: 59.94 / 2 = 29.97. */
    stb_field_next(&s->frame_deadline);
    timer_mod(s->frame_timer, stb_field_next(&s->frame_deadline));
}

/*
 * A listening socket for the frame source, so a decode can be piped straight
 * in without a raw intermediate:
 *
 *   ffmpeg -re -i feed.m2v -pix_fmt uyvy422 -f rawvideo tcp://127.0.0.1:4457
 *
 * A file is simpler but a minute of 720x480 UYVY is 1.2 GB, and writing one
 * across a mounted volume is slower than decoding it.  One producer at a
 * time; when it disconnects the last frame stays up and the listener waits
 * for the next, which is what a real input losing its feed looks like.
 */
static void stb_saa7146_accept(STBTulsaSAA7146 *s)
{
    int fd;

    /*
     * A live producer outranks `video-file`.  The file is a loop, so it never
     * ends and would hold the input for ever; without this the two options
     * are mutually exclusive and the socket can only be reached by
     * configuring no file at all.  With it, the file is the standby picture
     * (bars, a slate) and a feed takes over the moment it connects.
     */
    if (s->video_listen_fd < 0 || s->video_is_socket) {
        return;
    }
    fd = qemu_accept(s->video_listen_fd, NULL, NULL);
    if (fd < 0) {
        return;                     /* nobody waiting */
    }
    if (s->video_fd >= 0) {
        close(s->video_fd);         /* the standby file, displaced */
    }
    qemu_set_blocking(fd, false, NULL);
    s->video_fd = fd;
    s->video_is_pipe = true;
    s->video_is_socket = true;
    s->fill = 0;
    trace_stb_saa7146_source("producer connected");
}

static void stb_saa7146_open_listen(STBTulsaSAA7146 *s, Error **errp)
{
    SocketAddress addr = { .type = SOCKET_ADDRESS_TYPE_INET };
    char *host = NULL, *colon;

    s->video_listen_fd = -1;
    if (!s->video_listen || !s->video_listen[0]) {
        return;
    }
    host = g_strdup(s->video_listen);
    colon = strrchr(host, ':');
    if (!colon) {
        error_setg(errp, "stb-tulsa: video-listen wants host:port");
        g_free(host);
        return;
    }
    *colon = '\0';
    addr.u.inet.host = host;
    addr.u.inet.port = colon + 1;
    s->video_listen_fd = socket_listen(&addr, 1, errp);
    g_free(host);
    if (s->video_listen_fd >= 0) {
        qemu_set_blocking(s->video_listen_fd, false, NULL);
        trace_stb_saa7146_source(s->video_listen);
    }
}

static void stb_saa7146_open_source(STBTulsaSAA7146 *s, Error **errp)
{
    struct stat st;

    s->video_fd = -1;
    if (!s->video_file || !s->video_file[0]) {
        return;
    }
    /*
     * O_BINARY matters: the build host is Windows, and without it the CRT
     * translates CR LF inside the pixel data.  O_NONBLOCK does not exist
     * there at all, and a regular file never blocks anyway - a live feed on
     * Windows wants a socket chardev rather than a FIFO, which is the next
     * step rather than this one.
     */
    s->video_fd = qemu_open_old(s->video_file, O_RDONLY | O_BINARY
#ifdef O_NONBLOCK
                                | O_NONBLOCK
#endif
                                );
    if (s->video_fd < 0) {
        error_setg_errno(errp, errno, "stb-tulsa: cannot open video-file %s",
                         s->video_file);
        return;
    }
    s->video_is_pipe = fstat(s->video_fd, &st) == 0 && !S_ISREG(st.st_mode);
    s->video_is_socket = false;
}

static uint64_t stb_saa7146_read(void *opaque, hwaddr addr, unsigned size)
{
    STBTulsaSAA7146 *s = opaque;
    uint32_t val;

    if (addr == STB_SAA7146_BOARDID) {
        /*
         * stbtulsa.sys computes (~reg & 0x18) >> 3 to get a 0-3 card select,
         * so present the strap active-low in bits 4:3.  The remaining bits
         * of whatever register this really is are not known.
         */
        val = ~((uint32_t)s->board_id << 3) & 0x18;
        trace_stb_saa7146_boardid(s->board_id, val);
        return val;
    }

    val = s->regs[addr / 4];
    trace_stb_saa7146_read(addr, val);
    return val;
}

static void stb_saa7146_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    STBTulsaSAA7146 *s = opaque;

    trace_stb_saa7146_write(addr, val);
    s->regs[addr / 4] = val;

    /*
     * MC1 and MC2 are write-mask registers rather than plain words: the high
     * half says which bits the write applies to and the low half carries
     * their values.  Both call sites in STBTdde.dll only make sense that way
     * - enabling writes 0x04400440 (set 0x0440) and disabling writes
     * 0x00400000 (clear 0x0040) - so a model that stored the raw word would
     * read the disable as "capture on" plus garbage.
     */
    if (addr == STB_SAA7146_MC1) {
        uint32_t mask = (val >> 16) & 0xffff;
        uint32_t bits = val & 0xffff;
        uint32_t was = s->mc1;

        s->mc1 = (s->mc1 & ~mask) | (bits & mask);
        if ((was ^ s->mc1) & STB_SAA7146_MC1_CAPTURE) {
            trace_stb_saa7146_source((s->mc1 & STB_SAA7146_MC1_CAPTURE)
                                     ? "capture enabled" : "capture disabled");
            if (!(s->mc1 & STB_SAA7146_MC1_CAPTURE)) {
                s->cap_w = s->cap_h = 0;
            }
        }
    }
}

static const MemoryRegionOps stb_saa7146_ops = {
    .read = stb_saa7146_read,
    .write = stb_saa7146_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static void stb_saa7146_realize(PCIDevice *pdev, Error **errp)
{
    STBTulsaSAA7146 *s = STB_TULSA_SAA7146(pdev);

    pci_config_set_interrupt_pin(pdev->config, 1);
    memory_region_init_io(&s->reg, OBJECT(s), &stb_saa7146_ops, s,
                          "stb-saa7146-reg", STB_SAA7146_REG_SIZE);

    s->frame = g_malloc0(STB_VIDEO_BYTES);
    stb_saa7146_open_source(s, errp);
    stb_saa7146_open_listen(s, errp);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stb_saa7146_tick, s);
    s->frame_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->frame_timer, stb_field_next(&s->frame_deadline));
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->reg);
}

static const Property stb_saa7146_properties[] = {
    DEFINE_PROP_STRING("video-file", STBTulsaSAA7146, video_file),
    DEFINE_PROP_STRING("video-listen", STBTulsaSAA7146, video_listen),
    DEFINE_PROP_UINT8("board-id", STBTulsaSAA7146, board_id, 0),
};

static void stb_saa7146_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = stb_saa7146_realize;
    k->vendor_id = PCI_VENDOR_ID_PHILIPS_SEMI;
    k->device_id = PCI_DEVICE_ID_PHILIPS_SAA7146;
    k->class_id  = PCI_CLASS_MULTIMEDIA_VIDEO;
    k->revision  = 0x01;

    device_class_set_props(dc, stb_saa7146_properties);
    dc->desc = "Philips SAA7146 (STB Tulsa)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* ================================================================== *
 * The card: bridge at function 0, and everything it carries
 * ================================================================== */

static void stb_tulsa_config_write(PCIDevice *dev, uint32_t addr,
                                   uint32_t val, int len)
{
    uint32_t old = pci_default_read_config(dev, STB_TULSA_EBCR, 4);

    pci_bridge_write_config(dev, addr, val, len);

    if (ranges_overlap(addr, len, STB_TULSA_EBCR, 4)) {
        uint32_t now = pci_default_read_config(dev, STB_TULSA_EBCR, 4);

        if ((now & STB_TULSA_EBCR_RESET) && !(old & STB_TULSA_EBCR_RESET)) {
            STBTulsa *s = STB_TULSA(dev);

            /*
             * EBCR bit 5 resets the whole card, not just the i960: the
             * Vérités come back with an empty register file and a stopped
             * RISC core.  Resetting only the ATU left their debug-port state
             * behind, so a second run of the same application found the core
             * half-configured and its control object failed where the first
             * run had succeeded.
             */
            trace_stb_tulsa_core_reset();
            device_cold_reset(DEVICE(&s->atu));
            device_cold_reset(DEVICE(&s->saa));
            device_cold_reset(DEVICE(&s->rgb));
            device_cold_reset(DEVICE(&s->yuv));
            /* Self-clearing; the driver never clears it itself. */
            pci_set_long(dev->config + STB_TULSA_EBCR,
                         now & ~STB_TULSA_EBCR_RESET);
        }
    }
}

static void stb_tulsa_realize(PCIDevice *dev, Error **errp)
{
    STBTulsa *s = STB_TULSA(dev);
    PCIBus *primary = pci_get_bus(dev);
    PCIBus *sec;

    if (s->board_id >= STB_TULSA_MAX_CARDS) {
        error_setg(errp, "board-id must be 0-%d", STB_TULSA_MAX_CARDS - 1);
        return;
    }

    /*
     * The ATU is hardwired to the function above the bridge, so the card as a
     * whole has to sit on function 0 of its slot.
     */
    if (PCI_FUNC(dev->devfn) != 0) {
        error_setg(errp, "stb-tulsa must be placed on function 0 "
                   "(it claims function 1 for the ATU)");
        return;
    }

    /*
     * pci_bridge_initfn preserves the multifunction bit that
     * pci_init_multifunction already put in the header-type byte during
     * instance_init; see stb_tulsa_instance_init for why it has to be set
     * that early.
     */
    pci_bridge_initfn(dev, TYPE_PCI_BUS);
    sec = pci_bridge_get_sec_bus(PCI_BRIDGE(dev));

    qdev_prop_set_int32(DEVICE(&s->atu), "addr", dev->devfn + 1);
    qdev_prop_set_uint32(DEVICE(&s->atu), "dram-size", s->dram_size);
    qdev_prop_set_bit(DEVICE(&s->atu), "vertical-doorbell",
                       s->vertical_doorbell);
    qdev_prop_set_uint32(DEVICE(&s->atu), "notify-delay-ms",
                         s->notify_delay_ms);
    if (!qdev_realize(DEVICE(&s->atu), BUS(primary), errp)) {
        return;
    }

    qdev_prop_set_int32(DEVICE(&s->saa), "addr", STB_TULSA_DEVFN_SAA7146);
    qdev_prop_set_uint8(DEVICE(&s->saa), "board-id", s->board_id);
    if (s->video_file) {
        qdev_prop_set_string(DEVICE(&s->saa), "video-file", s->video_file);
    }
    if (s->video_listen) {
        qdev_prop_set_string(DEVICE(&s->saa), "video-listen", s->video_listen);
    }
    if (!qdev_realize(DEVICE(&s->saa), BUS(sec), errp)) {
        return;
    }

    /*
     * Give the processors qdev ids.  QMP screendump resolves its `device`
     * argument with qdev_find_recursive against DeviceState.id, so without
     * one the card's own console can only be reached by clicking through the
     * QEMU UI -- there is no way to ask for it programmatically.
     */
    qdev_prop_set_int32(DEVICE(&s->rgb), "addr", STB_TULSA_DEVFN_V2200RGB);
    qdev_prop_set_string(DEVICE(&s->rgb), "role", "rgb");
    qdev_prop_set_bit(DEVICE(&s->rgb), "lle", s->lle);
    DEVICE(&s->rgb)->id = g_strdup("tulsa-rgb");
    if (!qdev_realize(DEVICE(&s->rgb), BUS(sec), errp)) {
        return;
    }

    qdev_prop_set_int32(DEVICE(&s->yuv), "addr", STB_TULSA_DEVFN_V2200YUV);
    qdev_prop_set_string(DEVICE(&s->yuv), "role", "yuv");
    qdev_prop_set_bit(DEVICE(&s->yuv), "lle", s->lle);
    DEVICE(&s->yuv)->id = g_strdup("tulsa-yuv");
    if (!qdev_realize(DEVICE(&s->yuv), BUS(sec), errp)) {
        return;
    }

    /* Section 60.3: the RGB console composites against the YUV framebuffer. */
    s->rgb.peer = &s->yuv;
    s->yuv.peer = &s->rgb;

    /*
     * Section 82: the SAA7146 digitises the BNC input into the YUV
     * processor's framebuffer, so it needs to know which processor that is.
     * Wired here rather than at its own realize because the YUV side does
     * not exist yet then; until it does, delivery is a no-op.
     */
    s->saa.yuv = &s->yuv;
    /*
     * And the reverse, so a display-list slot reading the capture buffer can
     * report back how big a picture the host expects to find there.
     */
    s->rgb.saa = &s->saa;
    s->yuv.saa = &s->saa;
}

static void stb_tulsa_exit(PCIDevice *dev)
{
    pci_bridge_exitfn(dev);
}

static void stb_tulsa_instance_init(Object *obj)
{
    STBTulsa *s = STB_TULSA(obj);

    /*
     * Function 1 can only be populated if function 0 advertises
     * QEMU_PCI_CAP_MULTIFUNCTION, and the bit has to be in cap_present before
     * do_pci_register_device() runs - it is pci_init_multifunction(), called
     * from there, that copies it into the header-type byte.  Setting it in
     * realize() is too late: the child realizes fine, because that check only
     * looks at cap_present, but the header-type byte keeps bit 7 clear and
     * firmware never probes past function 0.  So the ATU would exist, and
     * never get a BAR.
     */
    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;

    object_initialize_child(obj, "atu", &s->atu, TYPE_STB_TULSA_ATU);
    object_initialize_child(obj, "saa7146", &s->saa, TYPE_STB_TULSA_SAA7146);
    object_initialize_child(obj, "v2200-rgb", &s->rgb, TYPE_STB_TULSA_V2200);
    object_initialize_child(obj, "v2200-yuv", &s->yuv, TYPE_STB_TULSA_V2200);
}

static const Property stb_tulsa_properties[] = {
    DEFINE_PROP_UINT8("board-id", STBTulsa, board_id, 0),
    DEFINE_PROP_UINT32("dram-size", STBTulsa, dram_size, 8 * MiB),
    DEFINE_PROP_UINT32("notify-delay-ms", STBTulsa, notify_delay_ms, 10),
    DEFINE_PROP_BOOL("vertical-doorbell", STBTulsa, vertical_doorbell, true),
    /*
     * Run the card's own microcode on the V2200 interpreter instead of
     * the HLE display-list reader.  Off by default: the HLE path is what
     * puts a picture on the console today, and the LLE work exists to
     * make that path redundant rather than to replace it before it can.
     */
    DEFINE_PROP_BOOL("lle", STBTulsa, lle, false),
    DEFINE_PROP_STRING("video-file", STBTulsa, video_file),
    DEFINE_PROP_STRING("video-listen", STBTulsa, video_listen),
};

static void stb_tulsa_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize      = stb_tulsa_realize;
    k->exit         = stb_tulsa_exit;
    k->config_write = stb_tulsa_config_write;
    k->vendor_id    = PCI_VENDOR_ID_STB;
    k->device_id    = PCI_DEVICE_ID_STB_TULSA_BRIDGE;
    k->class_id     = PCI_CLASS_BRIDGE_PCI;
    k->revision     = 0x01;

    device_class_set_props(dc, stb_tulsa_properties);
    dc->desc = "STB Systems Tulsa video card";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo stb_tulsa_types[] = {
    {
        .name          = TYPE_STB_TULSA,
        .parent        = TYPE_PCI_BRIDGE,
        .instance_size = sizeof(STBTulsa),
        .instance_init = stb_tulsa_instance_init,
        .class_init    = stb_tulsa_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }, {
        .name          = TYPE_STB_TULSA_ATU,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(STBTulsaATU),
        .class_init    = stb_atu_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }, {
        .name          = TYPE_STB_TULSA_V2200,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(STBTulsaV2200),
        .class_init    = stb_v2200_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }, {
        .name          = TYPE_STB_TULSA_SAA7146,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(STBTulsaSAA7146),
        .class_init    = stb_saa7146_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(stb_tulsa_types)
