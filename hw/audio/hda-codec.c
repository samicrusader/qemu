/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * written by Gerd Hoffmann <kraxel@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "intel-hda.h"
#include "migration/vmstate.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "intel-hda-defs.h"
#include "qemu/audio.h"
#include "trace.h"
#include "qom/object.h"

/* -------------------------------------------------------------------------- */

typedef struct desc_param {
    uint32_t id;
    uint32_t val;
} desc_param;

typedef struct desc_node {
    uint32_t nid;
    const char *name;
    const desc_param *params;
    uint32_t nparams;
    uint32_t config;
    uint32_t pinctl;
    uint32_t *conn;
    uint32_t stindex;
} desc_node;

/* converter with no QEMU audio voice behind it (e.g. S/PDIF) */
#define HDA_NO_STREAM  0xff
/* highest node id + 1 and max input amps per node, stateful codecs only */
#define HDA_MAX_NID    0x80
#define HDA_MAX_CONN   16

/* power-on register values for one node of a stateful codec */
typedef struct HDANodeInit {
    uint32_t nid;
    uint32_t conn_sel;
    uint8_t amp_out[2];
    uint8_t amp_in[HDA_MAX_CONN][2];
} HDANodeInit;

typedef struct desc_codec {
    const char *name;
    uint32_t iid;
    const desc_node *nodes;
    uint32_t nnodes;
    /*
     * Stateful codecs model every widget register (amps on any node,
     * connection selects, pin controls, power, EAPD, GPIO, digital
     * converters) instead of folding everything onto the converter
     * streams, and may run several output converters off one DMA stream
     * (multichannel playback).  Needed for codecs whose Linux driver is
     * a vendor patch rather than the generic parser.
     */
    bool stateful;
    const HDANodeInit *init;
    uint32_t ninit;
} desc_codec;

static const desc_param* hda_codec_find_param(const desc_node *node, uint32_t id)
{
    int i;

    for (i = 0; i < node->nparams; i++) {
        if (node->params[i].id == id) {
            return &node->params[i];
        }
    }
    return NULL;
}

static const desc_node* hda_codec_find_node(const desc_codec *codec, uint32_t nid)
{
    int i;

    for (i = 0; i < codec->nnodes; i++) {
        if (codec->nodes[i].nid == nid) {
            return &codec->nodes[i];
        }
    }
    return NULL;
}

static void hda_codec_parse_fmt(uint32_t format, struct audsettings *as)
{
    if (format & AC_FMT_TYPE_NON_PCM) {
        return;
    }

    as->freq = (format & AC_FMT_BASE_44K) ? 44100 : 48000;

    switch ((format & AC_FMT_MULT_MASK) >> AC_FMT_MULT_SHIFT) {
    case 1: as->freq *= 2; break;
    case 2: as->freq *= 3; break;
    case 3: as->freq *= 4; break;
    }

    switch ((format & AC_FMT_DIV_MASK) >> AC_FMT_DIV_SHIFT) {
    case 1: as->freq /= 2; break;
    case 2: as->freq /= 3; break;
    case 3: as->freq /= 4; break;
    case 4: as->freq /= 5; break;
    case 5: as->freq /= 6; break;
    case 6: as->freq /= 7; break;
    case 7: as->freq /= 8; break;
    }

    switch (format & AC_FMT_BITS_MASK) {
    case AC_FMT_BITS_8:  as->fmt = AUDIO_FORMAT_S8;  break;
    case AC_FMT_BITS_16: as->fmt = AUDIO_FORMAT_S16; break;
    case AC_FMT_BITS_32: as->fmt = AUDIO_FORMAT_S32; break;
    }

    as->nchannels = ((format & AC_FMT_CHAN_MASK) >> AC_FMT_CHAN_SHIFT) + 1;
}

/* -------------------------------------------------------------------------- */
/*
 * HDA codec descriptions
 */

/* some defines */

#define QEMU_HDA_ID_VENDOR  0x1af4
#define QEMU_HDA_PCM_FORMATS (AC_SUPPCM_BITS_16 |       \
                              0x1fc /* 16 -> 96 kHz */)
#define QEMU_HDA_AMP_NONE    (0)
#define QEMU_HDA_AMP_STEPS   0x4a

#define   PARAM mixemu
#define   HDA_MIXER
#include "hda-codec-common.h"

#define   PARAM nomixemu
#include  "hda-codec-common.h"

#include "hda-codec-ad1989a.h"

#define HDA_TIMER_TICKS (SCALE_MS)
#define B_SIZE sizeof(st->buf)
#define B_MASK (sizeof(st->buf) - 1)

/* -------------------------------------------------------------------------- */

static const char *fmt2name[] = {
    [ AUDIO_FORMAT_U8  ] = "PCM-U8",
    [ AUDIO_FORMAT_S8  ] = "PCM-S8",
    [ AUDIO_FORMAT_U16 ] = "PCM-U16",
    [ AUDIO_FORMAT_S16 ] = "PCM-S16",
    [ AUDIO_FORMAT_U32 ] = "PCM-U32",
    [ AUDIO_FORMAT_S32 ] = "PCM-S32",
};

#define TYPE_HDA_AUDIO "hda-audio"
OBJECT_DECLARE_SIMPLE_TYPE(HDAAudioState, HDA_AUDIO)

typedef struct HDAAudioStream HDAAudioStream;

struct HDAAudioStream {
    HDAAudioState *state;
    const desc_node *node;
    bool output, running;
    uint32_t stream;
    uint32_t channel;
    uint32_t format;
    uint32_t gain_left, gain_right;
    bool mute_left, mute_right;
    uint32_t amp_steps;     /* gain value that means 0 dB / full scale */
    uint32_t dma_channels;  /* channels per frame on the DMA stream */
    int64_t dma_pos;        /* bytes pulled from DMA by a group leader */
    struct audsettings as;
    union {
        SWVoiceIn *in;
        SWVoiceOut *out;
    } voice;
    uint8_t compat_buf[HDA_BUFFER_SIZE];
    uint32_t compat_bpos;
    uint8_t buf[8192]; /* size must be power of two */
    int64_t rpos;
    int64_t wpos;
    QEMUTimer *buft;
    int64_t buft_start;
};

#define HDA_AUDIO_MAX_STREAMS 8

typedef struct HDANodeState {
    uint8_t amp_out[2];
    uint8_t amp_in[HDA_MAX_CONN][2];
    uint8_t conn_sel;
    uint8_t pinctl;
    uint8_t power;
    uint8_t eapd;
    uint8_t unsol;
    uint8_t sdi;
    uint8_t beep;
    uint8_t conv;       /* stream/channel of converters without a voice */
    uint16_t format;    /* stream format of converters without a voice */
    uint16_t digi;      /* S/PDIF converter control 1 (low) / 2 (high) */
    uint16_t coef_index;
    uint32_t config;    /* pin default config, guest may override */
} HDANodeState;

enum {
    HDA_GPIO_DATA,
    HDA_GPIO_MASK,
    HDA_GPIO_DIRECTION,
    HDA_GPIO_WAKE_MASK,
    HDA_GPIO_UNSOL_MASK,
    HDA_GPIO_STICKY_MASK,
    HDA_GPIO_NREGS
};

struct HDAAudioState {
    HDACodecDevice hda;
    const char *name;

    AudioBackend *audio_be;
    const desc_codec *desc;
    HDAAudioStream st[HDA_AUDIO_MAX_STREAMS];
    bool running_compat[16];
    bool running_real[2 * 16];

    /* stateful codecs only */
    HDANodeState nodes[HDA_MAX_NID];
    uint8_t gpio[HDA_GPIO_NREGS];

    /* properties */
    uint32_t debug;
    bool     mixer;
};

static inline uint32_t hda_bytes_per_second(HDAAudioStream *st)
{
    return 2 * (uint32_t)st->as.nchannels * (uint32_t)st->as.freq;
}

static inline void hda_timer_sync_adjust(HDAAudioStream *st, int64_t target_pos)
{
    int64_t limit = B_SIZE / 8;
    int64_t corr = 0;

    if (target_pos > limit) {
        corr = HDA_TIMER_TICKS;
    }
    if (target_pos < -limit) {
        corr = -HDA_TIMER_TICKS;
    }
    if (target_pos < -(2 * limit)) {
        corr = -(4 * HDA_TIMER_TICKS);
    }
    if (corr == 0) {
        return;
    }

    trace_hda_audio_adjust(st->node->name, target_pos);
    st->buft_start += corr;
}

static void hda_audio_input_timer(void *opaque)
{
    HDAAudioStream *st = opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    int64_t uptime = now - st->buft_start;
    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;
    int64_t wanted_rpos;

    if (uptime <= 0) {
        /* wanted_rpos <= 0 */
        goto out_timer;
    }

    wanted_rpos = muldiv64(uptime, hda_bytes_per_second(st),
                           NANOSECONDS_PER_SECOND);
    wanted_rpos &= -4; /* IMPORTANT! clip to frames */

    if (wanted_rpos <= rpos) {
        /* we already transmitted the data */
        goto out_timer;
    }

    int64_t to_transfer = MIN(wpos - rpos, wanted_rpos - rpos);
    while (to_transfer) {
        uint32_t start = (rpos & B_MASK);
        uint32_t chunk = MIN(B_SIZE - start, to_transfer);
        int rc = hda_codec_xfer(
                &st->state->hda, st->stream, false, st->buf + start, chunk);
        if (!rc) {
            break;
        }
        rpos += chunk;
        to_transfer -= chunk;
        st->rpos += chunk;
    }

out_timer:

    if (st->running) {
        timer_mod_anticipate_ns(st->buft, now + HDA_TIMER_TICKS);
    }
}

static void hda_audio_input_cb(void *opaque, int avail)
{
    HDAAudioStream *st = opaque;

    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;

    int64_t to_transfer = MIN(B_SIZE - (wpos - rpos), avail);

    while (to_transfer) {
        uint32_t start = (uint32_t) (wpos & B_MASK);
        uint32_t chunk = (uint32_t) MIN(B_SIZE - start, to_transfer);
        uint32_t read = audio_be_read(st->state->audio_be, st->voice.in,
                                      st->buf + start, chunk);
        wpos += read;
        to_transfer -= read;
        st->wpos += read;
        if (chunk != read) {
            break;
        }
    }

    hda_timer_sync_adjust(st, -((wpos - rpos) - (B_SIZE >> 1)));
}

/*
 * Stateful codecs: every output converter on a stream tag sees the whole
 * interleaved frame (the guest programs the full channel count into each
 * converter's format and a channel offset into its stream id).  Only the
 * converter at channel 0 pulls from DMA; it then copies each member's
 * channel pair into that member's ring, so every converter keeps a plain
 * stereo/mono voice and the DMA position advances exactly once.
 */
static void hda_audio_output_timer_group(HDAAudioStream *st, int64_t now)
{
    HDAAudioState *a = st->state;
    HDAAudioStream *grp[HDA_AUDIO_MAX_STREAMS];
    uint8_t tmp[4096];
    uint32_t frame = 2 * st->dma_channels;
    int64_t uptime = now - st->buft_start;
    int64_t wanted, frames;
    int i, ng = 0;

    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        HDAAudioStream *m = &a->st[i];
        if (m->node && m->output && m->running && m->stream == st->stream &&
            m->dma_channels == st->dma_channels &&
            m->channel < st->dma_channels) {
            grp[ng++] = m;
        }
    }

    if (uptime <= 0 || frame == 0) {
        return;
    }
    wanted = muldiv64(uptime, frame * st->as.freq, NANOSECONDS_PER_SECOND);
    wanted -= wanted % frame;
    if (wanted <= st->dma_pos) {
        return;
    }

    frames = (wanted - st->dma_pos) / frame;
    for (i = 0; i < ng; i++) {
        uint32_t unit = 2 * grp[i]->as.nchannels;
        frames = MIN(frames, (B_SIZE - (grp[i]->wpos - grp[i]->rpos)) / unit);
    }

    while (frames > 0) {
        int64_t n = MIN(frames, sizeof(tmp) / frame);
        int64_t f;

        if (!hda_codec_xfer(&a->hda, st->stream, true, tmp, n * frame)) {
            break;
        }
        for (i = 0; i < ng; i++) {
            HDAAudioStream *m = grp[i];
            uint32_t unit = 2 * m->as.nchannels;
            for (f = 0; f < n; f++) {
                memcpy(m->buf + (m->wpos & B_MASK),
                       tmp + f * frame + 2 * m->channel, unit);
                m->wpos += unit;
            }
        }
        st->dma_pos += n * frame;
        frames -= n;
    }
}

static void hda_audio_output_timer(void *opaque)
{
    HDAAudioStream *st = opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (st->state->desc->stateful) {
        if (st->channel != 0) {
            /* a group member: the channel 0 converter feeds our ring */
            return;
        }
        hda_audio_output_timer_group(st, now);
        goto out_timer;
    }

    int64_t uptime = now - st->buft_start;
    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;
    int64_t wanted_wpos;

    if (uptime <= 0) {
        /* wanted_wpos <= 0 */
        goto out_timer;
    }

    wanted_wpos = muldiv64(uptime, hda_bytes_per_second(st),
                           NANOSECONDS_PER_SECOND);
    wanted_wpos &= -4; /* IMPORTANT! clip to frames */

    if (wanted_wpos <= wpos) {
        /* we already received the data */
        goto out_timer;
    }

    int64_t to_transfer = MIN(B_SIZE - (wpos - rpos), wanted_wpos - wpos);
    while (to_transfer) {
        uint32_t start = (wpos & B_MASK);
        uint32_t chunk = MIN(B_SIZE - start, to_transfer);
        int rc = hda_codec_xfer(
                &st->state->hda, st->stream, true, st->buf + start, chunk);
        if (!rc) {
            break;
        }
        wpos += chunk;
        to_transfer -= chunk;
        st->wpos += chunk;
    }

out_timer:

    if (st->running) {
        timer_mod_anticipate_ns(st->buft, now + HDA_TIMER_TICKS);
    }
}

static void hda_audio_output_cb(void *opaque, int avail)
{
    HDAAudioStream *st = opaque;

    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;

    int64_t to_transfer = MIN(wpos - rpos, avail);

    if (wpos - rpos == B_SIZE) {
        /* drop buffer, reset timer adjust */
        st->rpos = 0;
        st->wpos = 0;
        st->buft_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        trace_hda_audio_overrun(st->node->name);
        return;
    }

    while (to_transfer) {
        uint32_t start = (uint32_t) (rpos & B_MASK);
        uint32_t chunk = (uint32_t) MIN(B_SIZE - start, to_transfer);
        uint32_t written = audio_be_write(st->state->audio_be, st->voice.out,
                                          st->buf + start, chunk);
        rpos += written;
        to_transfer -= written;
        st->rpos += written;
        if (chunk != written) {
            break;
        }
    }

    hda_timer_sync_adjust(st, (wpos - rpos) - (B_SIZE >> 1));
}

static void hda_audio_set_running(HDAAudioStream *st, bool running)
{
    if (st->node == NULL) {
        return;
    }
    if (st->running == running) {
        return;
    }
    st->running = running;
    trace_hda_audio_running(st->node->name, st->stream, st->running);
    if (running) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        st->rpos = 0;
        st->wpos = 0;
        st->dma_pos = 0;
        st->buft_start = now;
        timer_mod_anticipate_ns(st->buft, now + HDA_TIMER_TICKS);
    } else {
        timer_del(st->buft);
    }
    if (st->output) {
        audio_be_set_active_out(st->state->audio_be, st->voice.out, st->running);
    } else {
        audio_be_set_active_in(st->state->audio_be, st->voice.in, st->running);
    }
}

static void hda_audio_set_amp(HDAAudioStream *st)
{
    bool muted;
    uint32_t left, right;

    if (st->node == NULL) {
        return;
    }

    muted = st->mute_left && st->mute_right;
    left  = st->mute_left  ? 0 : st->gain_left;
    right = st->mute_right ? 0 : st->gain_right;

    left = MIN(255, left * 255 / st->amp_steps);
    right = MIN(255, right * 255 / st->amp_steps);

    if (!st->state->mixer) {
        return;
    }
    if (st->output) {
        audio_be_set_volume_out_lr(st->state->audio_be, st->voice.out,
                                   muted, left, right);
    } else {
        audio_be_set_volume_in_lr(st->state->audio_be, st->voice.in,
                                  muted, left, right);
    }
}

static void hda_audio_setup(HDAAudioStream *st)
{
    audio_callback_fn cb;

    if (st->node == NULL) {
        return;
    }

    trace_hda_audio_format(st->node->name, st->as.nchannels,
                           fmt2name[st->as.fmt], st->as.freq);

    if (st->output) {
        cb = hda_audio_output_cb;
        timer_del(st->buft);
        st->voice.out = audio_be_open_out(st->state->audio_be, st->voice.out,
                                     st->node->name, st, cb, &st->as);
    } else {
        cb = hda_audio_input_cb;
        timer_del(st->buft);
        st->voice.in = audio_be_open_in(st->state->audio_be, st->voice.in,
                                   st->node->name, st, cb, &st->as);
    }
}

/* -------------------------------------------------------------------------- */
/* stateful codecs                                                            */

static HDAAudioStream *hda_node_stream(HDAAudioState *a, const desc_node *node)
{
    HDAAudioStream *st;

    if (node->stindex == HDA_NO_STREAM) {
        return NULL;
    }
    st = a->st + node->stindex;
    return st->node ? st : NULL;
}

/*
 * Voice settings follow the converter's slice of the DMA frame: the guest
 * programs the full channel count into every converter of a multichannel
 * stream, plus a channel offset; the converter itself carries two of them.
 */
static void hda_audio_stateful_setup(HDAAudioStream *st)
{
    struct audsettings as = st->as;
    uint32_t total;

    hda_codec_parse_fmt(st->format, &as);
    total = as.nchannels;
    st->dma_channels = total;
    if (st->channel < total) {
        as.nchannels = MIN(2, total - st->channel);
    } else {
        as.nchannels = 2;
    }
    st->as = as;
    hda_audio_setup(st);
}

static uint32_t hda_amp_zero_db(const desc_codec *desc, const desc_node *node)
{
    const desc_param *param = hda_codec_find_param(node, AC_PAR_AMP_OUT_CAP);
    uint32_t caps = param ? param->val : 0;
    uint32_t ofs = (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;
    uint32_t steps = (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;

    return ofs ? ofs : (steps ? steps : QEMU_HDA_AMP_STEPS);
}

/*
 * The host voice volume of a DAC is its own output amp; the volume of an
 * ADC is the output amp of the selector feeding it (where the capture
 * volume lives on codecs such as the AD198x).
 */
static void hda_audio_stateful_sync_amps(HDAAudioState *a)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        HDAAudioStream *st = &a->st[i];
        const desc_node *src;
        const uint8_t *g;

        if (st->node == NULL) {
            continue;
        }
        src = st->node;
        if (!st->output && src->conn) {
            src = hda_codec_find_node(a->desc, src->conn[0]) ?: src;
        }
        g = a->nodes[src->nid].amp_out;
        st->amp_steps = hda_amp_zero_db(a->desc, src);
        st->gain_left = g[0] & AC_AMP_GAIN;
        st->mute_left = g[0] & AC_AMP_MUTE;
        st->gain_right = g[1] & AC_AMP_GAIN;
        st->mute_right = g[1] & AC_AMP_MUTE;
        hda_audio_set_amp(st);
    }
}

static void hda_audio_stateful_reset(HDAAudioState *a)
{
    const desc_codec *desc = a->desc;
    uint32_t i, j;

    memset(a->nodes, 0, sizeof(a->nodes));
    memset(a->gpio, 0, sizeof(a->gpio));
    for (i = 0; i < desc->nnodes; i++) {
        const desc_node *node = desc->nodes + i;
        a->nodes[node->nid].config = node->config;
        a->nodes[node->nid].pinctl = node->pinctl;
    }
    for (i = 0; i < desc->ninit; i++) {
        const HDANodeInit *ini = desc->init + i;
        HDANodeState *ns = &a->nodes[ini->nid];
        ns->conn_sel = ini->conn_sel;
        ns->amp_out[0] = ini->amp_out[0];
        ns->amp_out[1] = ini->amp_out[1];
        for (j = 0; j < HDA_MAX_CONN; j++) {
            ns->amp_in[j][0] = ini->amp_in[j][0];
            ns->amp_in[j][1] = ini->amp_in[j][1];
        }
    }
    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        HDAAudioStream *st = &a->st[i];
        if (st->node == NULL) {
            continue;
        }
        hda_audio_set_running(st, false);
        st->stream = 0;
        st->channel = 0;
        st->format = AC_FMT_TYPE_PCM | AC_FMT_BITS_16 |
            (1 << AC_FMT_CHAN_SHIFT);
        hda_audio_stateful_setup(st);
    }
    hda_audio_stateful_sync_amps(a);
}

static void hda_audio_command_stateful(HDAAudioState *a, const desc_node *node,
                                       uint32_t verb, uint32_t payload)
{
    HDANodeState *ns = &a->nodes[node->nid];
    HDAAudioStream *st = hda_node_stream(a, node);
    const desc_param *param;
    uint32_t response = 0, count, shift, idx, lr;

    switch (verb) {
    case AC_VERB_PARAMETERS:
        param = hda_codec_find_param(node, payload);
        response = param ? param->val : 0;
        break;
    case AC_VERB_GET_SUBSYSTEM_ID:
        response = a->desc->iid;
        break;

    /* connections */
    case AC_VERB_GET_CONNECT_LIST:
        param = hda_codec_find_param(node, AC_PAR_CONNLIST_LEN);
        count = param ? param->val : 0;
        shift = 0;
        while (payload < count && shift < 32) {
            response |= node->conn[payload] << shift;
            payload++;
            shift += 8;
        }
        break;
    case AC_VERB_GET_CONNECT_SEL:
        response = ns->conn_sel;
        break;
    case AC_VERB_SET_CONNECT_SEL:
        param = hda_codec_find_param(node, AC_PAR_CONNLIST_LEN);
        if (param && payload < param->val) {
            ns->conn_sel = payload;
        }
        break;

    /* pins */
    case AC_VERB_GET_CONFIG_DEFAULT:
        response = ns->config;
        break;
    case AC_VERB_SET_CONFIG_DEFAULT_BYTES_0:
    case AC_VERB_SET_CONFIG_DEFAULT_BYTES_1:
    case AC_VERB_SET_CONFIG_DEFAULT_BYTES_2:
    case AC_VERB_SET_CONFIG_DEFAULT_BYTES_3:
        shift = (verb - AC_VERB_SET_CONFIG_DEFAULT_BYTES_0) * 8;
        ns->config = (ns->config & ~(0xffu << shift)) | (payload << shift);
        break;
    case AC_VERB_GET_PIN_WIDGET_CONTROL:
        response = ns->pinctl;
        break;
    case AC_VERB_SET_PIN_WIDGET_CONTROL:
        ns->pinctl = payload;
        break;
    case AC_VERB_GET_PIN_SENSE:
    case AC_VERB_SET_PIN_SENSE:
        break;
    case AC_VERB_GET_EAPD_BTLENABLE:
        response = ns->eapd;
        break;
    case AC_VERB_SET_EAPD_BTLENABLE:
        ns->eapd = payload;
        break;
    case AC_VERB_GET_UNSOLICITED_RESPONSE:
        response = ns->unsol;
        break;
    case AC_VERB_SET_UNSOLICITED_ENABLE:
        ns->unsol = payload;
        break;

    /* misc per-node registers */
    case AC_VERB_GET_POWER_STATE:
        response = (ns->power << 4) | ns->power;
        break;
    case AC_VERB_SET_POWER_STATE:
        ns->power = payload & 0x0f;
        break;
    case AC_VERB_GET_SDI_SELECT:
        response = ns->sdi;
        break;
    case AC_VERB_SET_SDI_SELECT:
        ns->sdi = payload;
        break;
    case AC_VERB_GET_BEEP_CONTROL:
        response = ns->beep;
        break;
    case AC_VERB_SET_BEEP_CONTROL:
        ns->beep = payload;
        break;
    case AC_VERB_GET_DIGI_CONVERT_1:
        response = ns->digi;
        break;
    case AC_VERB_SET_DIGI_CONVERT_1:
        ns->digi = (ns->digi & 0xff00) | payload;
        break;
    case AC_VERB_SET_DIGI_CONVERT_2:
        ns->digi = (ns->digi & 0x00ff) | (payload << 8);
        break;
    case AC_VERB_GET_COEF_INDEX:
        response = ns->coef_index;
        break;
    case AC_VERB_SET_COEF_INDEX:
        ns->coef_index = payload;
        break;
    case AC_VERB_GET_PROC_COEF:
    case AC_VERB_SET_PROC_COEF:
    case AC_VERB_GET_PROC_STATE:
    case AC_VERB_SET_PROC_STATE:
    case AC_VERB_GET_VOLUME_KNOB_CONTROL:
    case AC_VERB_SET_VOLUME_KNOB_CONTROL:
        break;

    /* function group GPIOs */
    case AC_VERB_GET_GPIO_DATA ... AC_VERB_GET_GPIO_STICKY_MASK:
        response = a->gpio[verb - AC_VERB_GET_GPIO_DATA];
        break;
    case AC_VERB_SET_GPIO_DATA ... AC_VERB_SET_GPIO_STICKY_MASK:
        a->gpio[verb - AC_VERB_SET_GPIO_DATA] = payload;
        break;

    /* converters */
    case AC_VERB_SET_CHANNEL_STREAMID:
        ns->conv = payload;
        if (st) {
            hda_audio_set_running(st, false);
            st->stream = (payload >> 4) & 0x0f;
            st->channel = payload & 0x0f;
            dprint(a, 2, "%s: stream %d, channel %d\n",
                   st->node->name, st->stream, st->channel);
            hda_audio_stateful_setup(st);
            hda_audio_set_amp(st);
            hda_audio_set_running(st,
                a->running_real[st->output * 16 + st->stream]);
        }
        break;
    case AC_VERB_GET_CONV:
        response = st ? (st->stream << 4 | st->channel) : ns->conv;
        break;
    case AC_VERB_SET_STREAM_FORMAT:
        ns->format = payload;
        if (st) {
            st->format = payload;
            hda_audio_stateful_setup(st);
            hda_audio_set_amp(st);
        }
        break;
    case AC_VERB_GET_STREAM_FORMAT:
        response = st ? st->format : ns->format;
        break;

    /* amplifiers, on any node */
    case AC_VERB_GET_AMP_GAIN_MUTE:
        lr = (payload & AC_AMP_GET_LEFT) ? 0 : 1;
        idx = payload & AC_AMP_GET_INDEX;
        if (payload & AC_AMP_GET_OUTPUT) {
            response = ns->amp_out[lr];
        } else if (idx < HDA_MAX_CONN) {
            response = ns->amp_in[idx][lr];
        }
        break;
    case AC_VERB_SET_AMP_GAIN_MUTE:
        idx = (payload & AC_AMP_SET_INDEX) >> AC_AMP_SET_INDEX_SHIFT;
        for (lr = 0; lr < 2; lr++) {
            if (!(payload & (lr ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT))) {
                continue;
            }
            if (payload & AC_AMP_SET_OUTPUT) {
                ns->amp_out[lr] = payload & 0xff;
            }
            if ((payload & AC_AMP_SET_INPUT) && idx < HDA_MAX_CONN) {
                ns->amp_in[idx][lr] = payload & 0xff;
            }
        }
        hda_audio_stateful_sync_amps(a);
        break;

    case AC_VERB_SET_CODEC_RESET:
        hda_audio_stateful_reset(a);
        break;

    default:
        dprint(a, 1, "%s: not handled: nid %d (%s), verb 0x%x, payload 0x%x\n",
               __func__, node->nid, node->name, verb, payload);
        break;
    }
    hda_codec_response(&a->hda, true, response);
}

static void hda_audio_command(HDACodecDevice *hda, uint32_t nid, uint32_t data)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    HDAAudioStream *st;
    const desc_node *node = NULL;
    const desc_param *param;
    uint32_t verb, payload, response, count, shift;

    if ((data & 0x70000) == 0x70000) {
        /* 12/8 id/payload */
        verb = (data >> 8) & 0xfff;
        payload = data & 0x00ff;
    } else {
        /* 4/16 id/payload */
        verb = (data >> 8) & 0xf00;
        payload = data & 0xffff;
    }

    node = hda_codec_find_node(a->desc, nid);
    if (node == NULL) {
        goto fail;
    }
    dprint(a, 2, "%s: nid %d (%s), verb 0x%x, payload 0x%x\n",
           __func__, nid, node->name, verb, payload);

    if (a->desc->stateful) {
        hda_audio_command_stateful(a, node, verb, payload);
        return;
    }

    switch (verb) {
    /* all nodes */
    case AC_VERB_PARAMETERS:
        param = hda_codec_find_param(node, payload);
        if (param == NULL) {
            goto fail;
        }
        hda_codec_response(hda, true, param->val);
        break;
    case AC_VERB_GET_SUBSYSTEM_ID:
        hda_codec_response(hda, true, a->desc->iid);
        break;

    /* all functions */
    case AC_VERB_GET_CONNECT_LIST:
        param = hda_codec_find_param(node, AC_PAR_CONNLIST_LEN);
        count = param ? param->val : 0;
        response = 0;
        shift = 0;
        while (payload < count && shift < 32) {
            response |= node->conn[payload] << shift;
            payload++;
            shift += 8;
        }
        hda_codec_response(hda, true, response);
        break;

    /* pin widget */
    case AC_VERB_GET_CONFIG_DEFAULT:
        hda_codec_response(hda, true, node->config);
        break;
    case AC_VERB_GET_PIN_WIDGET_CONTROL:
        hda_codec_response(hda, true, node->pinctl);
        break;
    case AC_VERB_SET_PIN_WIDGET_CONTROL:
        if (node->pinctl != payload) {
            dprint(a, 1, "unhandled pin control bit\n");
        }
        hda_codec_response(hda, true, 0);
        break;

    /* audio in/out widget */
    case AC_VERB_SET_CHANNEL_STREAMID:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        hda_audio_set_running(st, false);
        st->stream = (payload >> 4) & 0x0f;
        st->channel = payload & 0x0f;
        dprint(a, 2, "%s: stream %d, channel %d\n",
               st->node->name, st->stream, st->channel);
        hda_audio_set_running(st, a->running_real[st->output * 16 + st->stream]);
        hda_codec_response(hda, true, 0);
        break;
    case AC_VERB_GET_CONV:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        response = st->stream << 4 | st->channel;
        hda_codec_response(hda, true, response);
        break;
    case AC_VERB_SET_STREAM_FORMAT:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        st->format = payload;
        hda_codec_parse_fmt(st->format, &st->as);
        hda_audio_setup(st);
        hda_codec_response(hda, true, 0);
        break;
    case AC_VERB_GET_STREAM_FORMAT:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        hda_codec_response(hda, true, st->format);
        break;
    case AC_VERB_GET_AMP_GAIN_MUTE:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        if (payload & AC_AMP_GET_LEFT) {
            response = st->gain_left | (st->mute_left ? AC_AMP_MUTE : 0);
        } else {
            response = st->gain_right | (st->mute_right ? AC_AMP_MUTE : 0);
        }
        hda_codec_response(hda, true, response);
        break;
    case AC_VERB_SET_AMP_GAIN_MUTE:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        dprint(a, 1, "amp (%s): %s%s%s%s index %d  gain %3d %s\n",
               st->node->name,
               (payload & AC_AMP_SET_OUTPUT) ? "o" : "-",
               (payload & AC_AMP_SET_INPUT)  ? "i" : "-",
               (payload & AC_AMP_SET_LEFT)   ? "l" : "-",
               (payload & AC_AMP_SET_RIGHT)  ? "r" : "-",
               (payload & AC_AMP_SET_INDEX) >> AC_AMP_SET_INDEX_SHIFT,
               (payload & AC_AMP_GAIN),
               (payload & AC_AMP_MUTE) ? "muted" : "");
        if (payload & AC_AMP_SET_LEFT) {
            st->gain_left = payload & AC_AMP_GAIN;
            st->mute_left = payload & AC_AMP_MUTE;
        }
        if (payload & AC_AMP_SET_RIGHT) {
            st->gain_right = payload & AC_AMP_GAIN;
            st->mute_right = payload & AC_AMP_MUTE;
        }
        hda_audio_set_amp(st);
        hda_codec_response(hda, true, 0);
        break;

    /* not supported */
    case AC_VERB_SET_POWER_STATE:
    case AC_VERB_GET_POWER_STATE:
    case AC_VERB_GET_SDI_SELECT:
        hda_codec_response(hda, true, 0);
        break;
    default:
        goto fail;
    }
    return;

fail:
    dprint(a, 1, "%s: not handled: nid %d (%s), verb 0x%x, payload 0x%x\n",
           __func__, nid, node ? node->name : "?", verb, payload);
    hda_codec_response(hda, true, 0);
}

static void hda_audio_stream(HDACodecDevice *hda, uint32_t stnr, bool running, bool output)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    int s;

    a->running_compat[stnr] = running;
    a->running_real[output * 16 + stnr] = running;
    for (s = 0; s < ARRAY_SIZE(a->st); s++) {
        if (a->st[s].node == NULL) {
            continue;
        }
        if (a->st[s].output != output) {
            continue;
        }
        if (a->st[s].stream != stnr) {
            continue;
        }
        hda_audio_set_running(&a->st[s], running);
    }
}

static void hda_audio_init(HDACodecDevice *hda,
                           const struct desc_codec *desc,
                           Error **errp)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    HDAAudioStream *st;
    const desc_node *node;
    const desc_param *param;
    uint32_t i, type;

    if (!audio_be_check(&a->audio_be, errp)) {
        return;
    }

    a->desc = desc;
    a->name = object_get_typename(OBJECT(a));
    dprint(a, 1, "%s: cad %d\n", __func__, a->hda.cad);

    for (i = 0; i < a->desc->nnodes; i++) {
        node = a->desc->nodes + i;
        param = hda_codec_find_param(node, AC_PAR_AUDIO_WIDGET_CAP);
        if (param == NULL) {
            continue;
        }
        if (desc->stateful && node->nid >= HDA_MAX_NID) {
            error_setg(errp, "%s: node 0x%x out of range", desc->name,
                       node->nid);
            return;
        }
        type = (param->val & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
        if (node->stindex == HDA_NO_STREAM) {
            continue;
        }
        switch (type) {
        case AC_WID_AUD_OUT:
        case AC_WID_AUD_IN:
            assert(node->stindex < ARRAY_SIZE(a->st));
            st = a->st + node->stindex;
            st->state = a;
            st->node = node;
            if (type == AC_WID_AUD_OUT) {
                /* unmute output by default */
                st->gain_left = QEMU_HDA_AMP_STEPS;
                st->gain_right = QEMU_HDA_AMP_STEPS;
                st->compat_bpos = sizeof(st->compat_buf);
                st->output = true;
                st->buft = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        hda_audio_output_timer, st);
            } else {
                st->output = false;
                st->buft = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        hda_audio_input_timer, st);
            }
            st->amp_steps = QEMU_HDA_AMP_STEPS;
            st->dma_channels = 2;
            st->format = AC_FMT_TYPE_PCM | AC_FMT_BITS_16 |
                (1 << AC_FMT_CHAN_SHIFT);
            hda_codec_parse_fmt(st->format, &st->as);
            if (!desc->stateful) {
                hda_audio_setup(st);
            }
            break;
        }
    }

    if (desc->stateful) {
        hda_audio_stateful_reset(a);
    }
}

static void hda_audio_exit(HDACodecDevice *hda)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    HDAAudioStream *st;
    int i;

    dprint(a, 1, "%s\n", __func__);
    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        st = a->st + i;
        if (st->node == NULL) {
            continue;
        }
        timer_free(st->buft);
        if (st->output) {
            audio_be_close_out(a->audio_be, st->voice.out);
        } else {
            audio_be_close_in(a->audio_be, st->voice.in);
        }
    }
}

static int hda_audio_post_load(void *opaque, int version)
{
    HDAAudioState *a = opaque;
    HDAAudioStream *st;
    int i;

    dprint(a, 1, "%s\n", __func__);
    if (version == 1) {
        /* assume running_compat[] is for output streams */
        for (i = 0; i < ARRAY_SIZE(a->running_compat); i++)
            a->running_real[16 + i] = a->running_compat[i];
    }

    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        st = a->st + i;
        if (st->node == NULL)
            continue;
        if (a->desc->stateful) {
            hda_audio_stateful_setup(st);
        } else {
            hda_codec_parse_fmt(st->format, &st->as);
            hda_audio_setup(st);
        }
        hda_audio_set_amp(st);
        hda_audio_set_running(st, a->running_real[st->output * 16 + st->stream]);
    }
    if (a->desc->stateful) {
        hda_audio_stateful_sync_amps(a);
    }
    return 0;
}

static void hda_audio_reset(DeviceState *dev)
{
    HDAAudioState *a = HDA_AUDIO(dev);
    HDAAudioStream *st;
    int i;

    dprint(a, 1, "%s\n", __func__);
    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        st = a->st + i;
        if (st->node != NULL) {
            hda_audio_set_running(st, false);
        }
    }
    if (a->desc && a->desc->stateful) {
        hda_audio_stateful_reset(a);
    }
}

static bool vmstate_hda_audio_stream_buf_needed(void *opaque)
{
    HDAAudioStream *st = opaque;
    return st->state;
}

static const VMStateDescription vmstate_hda_audio_stream_buf = {
    .name = "hda-audio-stream/buffer",
    .version_id = 1,
    .needed = vmstate_hda_audio_stream_buf_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER(buf, HDAAudioStream),
        VMSTATE_INT64(rpos, HDAAudioStream),
        VMSTATE_INT64(wpos, HDAAudioStream),
        VMSTATE_TIMER_PTR(buft, HDAAudioStream),
        VMSTATE_INT64(buft_start, HDAAudioStream),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hda_audio_stream = {
    .name = "hda-audio-stream",
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(stream, HDAAudioStream),
        VMSTATE_UINT32(channel, HDAAudioStream),
        VMSTATE_UINT32(format, HDAAudioStream),
        VMSTATE_UINT32(gain_left, HDAAudioStream),
        VMSTATE_UINT32(gain_right, HDAAudioStream),
        VMSTATE_BOOL(mute_left, HDAAudioStream),
        VMSTATE_BOOL(mute_right, HDAAudioStream),
        VMSTATE_UINT32(compat_bpos, HDAAudioStream),
        VMSTATE_BUFFER(compat_buf, HDAAudioStream),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_hda_audio_stream_buf,
        NULL
    }
};

static const VMStateDescription vmstate_hda_node = {
    .name = "hda-audio-node",
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(amp_out, HDANodeState, 2),
        VMSTATE_UINT8_2DARRAY(amp_in, HDANodeState, HDA_MAX_CONN, 2),
        VMSTATE_UINT8(conn_sel, HDANodeState),
        VMSTATE_UINT8(pinctl, HDANodeState),
        VMSTATE_UINT8(power, HDANodeState),
        VMSTATE_UINT8(eapd, HDANodeState),
        VMSTATE_UINT8(unsol, HDANodeState),
        VMSTATE_UINT8(sdi, HDANodeState),
        VMSTATE_UINT8(beep, HDANodeState),
        VMSTATE_UINT8(conv, HDANodeState),
        VMSTATE_UINT16(format, HDANodeState),
        VMSTATE_UINT16(digi, HDANodeState),
        VMSTATE_UINT16(coef_index, HDANodeState),
        VMSTATE_UINT32(config, HDANodeState),
        VMSTATE_END_OF_LIST()
    }
};

static bool vmstate_hda_audio_stateful_needed(void *opaque)
{
    HDAAudioState *a = opaque;
    return a->desc && a->desc->stateful;
}

static const VMStateDescription vmstate_hda_audio_stateful = {
    .name = "hda-audio/stateful",
    .version_id = 1,
    .needed = vmstate_hda_audio_stateful_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_SUB_ARRAY(st, HDAAudioState, 4,
                                 HDA_AUDIO_MAX_STREAMS - 4, 0,
                                 vmstate_hda_audio_stream, HDAAudioStream),
        VMSTATE_STRUCT_ARRAY(nodes, HDAAudioState, HDA_MAX_NID, 0,
                             vmstate_hda_node, HDANodeState),
        VMSTATE_UINT8_ARRAY(gpio, HDAAudioState, HDA_GPIO_NREGS),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hda_audio = {
    .name = "hda-audio",
    .version_id = 2,
    .post_load = hda_audio_post_load,
    .fields = (const VMStateField[]) {
        /* the first four streams keep the pre-stateful layout */
        VMSTATE_STRUCT_SUB_ARRAY(st, HDAAudioState, 0, 4, 0,
                                 vmstate_hda_audio_stream,
                                 HDAAudioStream),
        VMSTATE_BOOL_ARRAY(running_compat, HDAAudioState, 16),
        VMSTATE_BOOL_ARRAY_V(running_real, HDAAudioState, 2 * 16, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_hda_audio_stateful,
        NULL
    }
};

static const Property hda_audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(HDAAudioState, audio_be),
    DEFINE_PROP_UINT32("debug", HDAAudioState, debug,   0),
    DEFINE_PROP_BOOL("mixer", HDAAudioState, mixer,  true),
};

static void hda_audio_init_output(HDACodecDevice *hda, Error **errp)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    const struct desc_codec *desc = &output_mixemu;

    if (!a->mixer) {
        desc = &output_nomixemu;
    }

    hda_audio_init(hda, desc, errp);
}

static void hda_audio_init_duplex(HDACodecDevice *hda, Error **errp)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    const struct desc_codec *desc = &duplex_mixemu;

    if (!a->mixer) {
        desc = &duplex_nomixemu;
    }

    hda_audio_init(hda, desc, errp);
}

static void hda_audio_init_micro(HDACodecDevice *hda, Error **errp)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    const struct desc_codec *desc = &micro_mixemu;

    if (!a->mixer) {
        desc = &micro_nomixemu;
    }

    hda_audio_init(hda, desc, errp);
}

static void hda_audio_init_ad1989a(HDACodecDevice *hda, Error **errp)
{
    hda_audio_init(hda, &ad1989a, errp);
}

static void hda_audio_base_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->exit = hda_audio_exit;
    k->command = hda_audio_command;
    k->stream = hda_audio_stream;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    device_class_set_legacy_reset(dc, hda_audio_reset);
    dc->vmsd = &vmstate_hda_audio;
    device_class_set_props(dc, hda_audio_properties);
}

static const TypeInfo hda_audio_info = {
    .name          = TYPE_HDA_AUDIO,
    .parent        = TYPE_HDA_CODEC_DEVICE,
    .instance_size = sizeof(HDAAudioState),
    .class_init    = hda_audio_base_class_init,
    .abstract      = true,
};

static void hda_audio_output_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_output;
    dc->desc = "HDA Audio Codec, output-only (line-out)";
}

static const TypeInfo hda_audio_output_info = {
    .name          = "hda-output",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_output_class_init,
};

static void hda_audio_duplex_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_duplex;
    dc->desc = "HDA Audio Codec, duplex (line-out, line-in)";
}

static const TypeInfo hda_audio_duplex_info = {
    .name          = "hda-duplex",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_duplex_class_init,
};

static void hda_audio_micro_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_micro;
    dc->desc = "HDA Audio Codec, duplex (speaker, microphone)";
}

static const TypeInfo hda_audio_micro_info = {
    .name          = "hda-micro",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_micro_class_init,
};

static void hda_audio_ad1989a_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_ad1989a;
    dc->desc = "HDA Audio Codec, Analog Devices AD1989A "
               "(6ch out, 3 ADCs, as wired in the Trilithic EASyCAP)";
}

static const TypeInfo hda_audio_ad1989a_info = {
    .name          = "hda-ad1989a",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_ad1989a_class_init,
};

static void hda_audio_register_types(void)
{
    type_register_static(&hda_audio_info);
    type_register_static(&hda_audio_output_info);
    type_register_static(&hda_audio_duplex_info);
    type_register_static(&hda_audio_micro_info);
    type_register_static(&hda_audio_ad1989a_info);
}

type_init(hda_audio_register_types)
