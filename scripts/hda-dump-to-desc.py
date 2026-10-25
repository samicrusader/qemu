#!/usr/bin/env python3
#
# Convert an ALSA /proc/asound/cardN/codec#M dump (the format hda-emu and
# alsa-info use) into QEMU hda-codec.c desc_node tables.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# usage: hda-dump-to-desc.py DUMP PREFIX [--vendor-id 0x...] [--rev-id 0x...]
#            [--subsys-id 0x...] [--pcm-bits 0x2] [--pin NID=CONFIG ...]
#            [--stream NID=INDEX ...] [--name NID=NAME ...]
#
# Converters (Audio Output / Audio Input widgets) get a QEMU stream index only
# when one is given with --stream; every other converter is emitted with
# HDA_NO_STREAM so it is fully register-modelled but never opens a voice.
# --pcm-bits replaces the bit-depth mask of the function group's default PCM
# parameter (and of analog converters that inherit it): QEMU's codec only
# streams 16-bit samples, so advertising 20/24-bit would let the guest pick a
# format QEMU cannot carry.

import argparse
import re
import sys

WID_TYPES = {
    'Audio Output': 0x0, 'Audio Input': 0x1, 'Audio Mixer': 0x2,
    'Audio Selector': 0x3, 'Pin Complex': 0x4, 'Power Widget': 0x5,
    'Volume Knob Widget': 0x6, 'Beep Generator Widget': 0x7,
    'Vendor Defined Widget': 0xf,
}

AC_WCAP_DIGITAL = 1 << 9
AC_WCAP_CONN_LIST = 1 << 8


def amp_caps(m):
    ofs, nsteps, stepsize, mute = (int(x, 0) for x in m.groups())
    return (mute << 31) | (stepsize << 16) | (nsteps << 8) | ofs


def parse(path):
    hdr = {}
    nodes = []
    cur = None
    pcm_target = None
    lines = open(path).read().splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        i += 1
        m = re.match(r'Node (0x[0-9a-f]+) \[(.+?)\] wcaps (0x[0-9a-f]+)', ln)
        if m:
            cur = {'nid': int(m.group(1), 16), 'type': m.group(2),
                   'wcaps': int(m.group(3), 16), 'conn': [], 'sel': 0,
                   'amp_out_vals': [], 'amp_in_vals': []}
            nodes.append(cur)
            pcm_target = cur
            continue
        if cur is None:
            for key, rx in (('vendor', r'Vendor Id: (0x[0-9a-f]+)'),
                            ('subsys', r'Subsystem Id: (0x[0-9a-f]+)'),
                            ('rev', r'Revision Id: (0x[0-9a-f]+)')):
                m = re.match(rx, ln)
                if m:
                    hdr[key] = int(m.group(1), 16)
            m = re.match(r'Default Amp-In caps: ofs=(\S+), nsteps=(\S+), '
                         r'stepsize=(\S+), mute=(\d)', ln)
            if m:
                hdr['amp_in'] = amp_caps(m)
            m = re.match(r'Default Amp-Out caps: ofs=(\S+), nsteps=(\S+), '
                         r'stepsize=(\S+), mute=(\d)', ln)
            if m:
                hdr['amp_out'] = amp_caps(m)
            m = re.match(r'GPIO: io=(\d+), o=(\d+), i=(\d+), unsolicited=(\d), '
                         r'wake=(\d)', ln)
            if m:
                io, o, gi, uns, wake = (int(x) for x in m.groups())
                hdr['gpio'] = (wake << 31) | (uns << 30) | (gi << 16) | \
                              (o << 8) | io
            m = re.match(r'\s+rates \[(0x[0-9a-f]+)\]', ln)
            if m:
                hdr['rates'] = int(m.group(1), 16)
            m = re.match(r'\s+bits \[(0x[0-9a-f]+)\]', ln)
            if m:
                hdr['bits'] = int(m.group(1), 16)
            m = re.match(r'\s+formats \[(0x[0-9a-f]+)\]', ln)
            if m:
                hdr['formats'] = int(m.group(1), 16)
            continue
        s = ln.strip()
        m = re.match(r'Amp-Out caps: ofs=(\S+), nsteps=(\S+), stepsize=(\S+), '
                     r'mute=(\d)', s)
        if m:
            cur['amp_out'] = amp_caps(m)
            continue
        m = re.match(r'Amp-In caps: ofs=(\S+), nsteps=(\S+), stepsize=(\S+), '
                     r'mute=(\d)', s)
        if m:
            cur['amp_in'] = amp_caps(m)
            continue
        m = re.match(r'Amp-Out vals:\s+(.*)', s)
        if m:
            cur['amp_out_vals'] = [[int(v, 16) for v in grp.split()]
                                   for grp in re.findall(r'\[([^\]]*)\]',
                                                         m.group(1))]
            continue
        m = re.match(r'Amp-In vals:\s+(.*)', s)
        if m:
            cur['amp_in_vals'] = [[int(v, 16) for v in grp.split()]
                                  for grp in re.findall(r'\[([^\]]*)\]',
                                                        m.group(1))]
            continue
        m = re.match(r'Pincap (0x[0-9a-f]+)', s)
        if m:
            cur['pincap'] = int(m.group(1), 16)
            continue
        m = re.match(r'Pin Default (0x[0-9a-f]+)', s)
        if m:
            cur['config'] = int(m.group(1), 16)
            continue
        m = re.match(r'rates \[(0x[0-9a-f]+)\]', s)
        if m:
            cur['rates'] = int(m.group(1), 16)
            continue
        m = re.match(r'bits \[(0x[0-9a-f]+)\]', s)
        if m:
            cur['bits'] = int(m.group(1), 16)
            continue
        m = re.match(r'formats \[(0x[0-9a-f]+)\]', s)
        if m:
            cur['formats'] = int(m.group(1), 16)
            continue
        m = re.match(r'Connection: (\d+)', s)
        if m:
            n = int(m.group(1))
            ents = lines[i].split()
            i += 1
            for idx, e in enumerate(ents):
                if e.endswith('*'):
                    cur['sel'] = idx
                    e = e[:-1]
                cur['conn'].append(int(e, 16))
            assert len(cur['conn']) == n, (hex(cur['nid']), n, ents)
            continue
    return hdr, nodes


def kv(items, conv_val):
    out = {}
    for it in items or []:
        k, v = it.split('=', 1)
        out[int(k, 0)] = conv_val(v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('prefix')
    ap.add_argument('--vendor-id', type=lambda x: int(x, 0))
    ap.add_argument('--rev-id', type=lambda x: int(x, 0))
    ap.add_argument('--subsys-id', type=lambda x: int(x, 0))
    ap.add_argument('--pcm-bits', type=lambda x: int(x, 0))
    ap.add_argument('--fg-cap', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--pin', action='append')
    ap.add_argument('--stream', action='append')
    ap.add_argument('--name', action='append')
    ap.add_argument('--title', default='')
    a = ap.parse_args()

    hdr, nodes = parse(a.dump)
    pins = kv(a.pin, lambda v: int(v, 0))
    streams = kv(a.stream, lambda v: int(v, 0))
    names = kv(a.name, str)
    vendor = a.vendor_id if a.vendor_id is not None else hdr['vendor']
    rev = a.rev_id if a.rev_id is not None else hdr.get('rev', 0)
    subsys = a.subsys_id if a.subsys_id is not None else hdr.get('subsys', 0)
    bits = a.pcm_bits if a.pcm_bits is not None else hdr['bits']
    p = a.prefix
    first, last = nodes[0]['nid'], nodes[-1]['nid']

    o = []
    w = o.append
    w('/*')
    w(' * %s' % (a.title or ('%s codec description' % p)))
    w(' *')
    w(' * GENERATED by scripts/hda-dump-to-desc.py - do not edit by hand.')
    w(' * Source dump: %s' % a.dump.replace('\\', '/').split('/')[-1])
    w(' * Command line: %s' % ' '.join(sys.argv[2:]))
    w(' *')
    w(' * SPDX-License-Identifier: GPL-2.0-or-later')
    w(' */')
    w('')
    w('#define %s_VENDOR_ID 0x%08x' % (p.upper(), vendor))
    w('')
    w('static const desc_param %s_params_root[] = {' % p)
    w('    { .id = AC_PAR_VENDOR_ID,  .val = 0x%08x },' % vendor)
    w('    { .id = AC_PAR_SUBSYSTEM_ID, .val = 0x%08x },' % subsys)
    w('    { .id = AC_PAR_REV_ID,     .val = 0x%08x },' % rev)
    w('    { .id = AC_PAR_NODE_COUNT, .val = 0x00010001 },')
    w('};')
    w('')
    w('static const desc_param %s_params_afg[] = {' % p)
    w('    { .id = AC_PAR_FUNCTION_TYPE, .val = 0x00000101 },')
    w('    { .id = AC_PAR_NODE_COUNT, .val = 0x%08x },'
      % ((first << 16) | (last - first + 1)))
    w('    { .id = AC_PAR_AUDIO_FG_CAP, .val = 0x%08x },' % a.fg_cap)
    w('    { .id = AC_PAR_PCM,        .val = 0x%08x },'
      % ((bits << 16) | hdr['rates']))
    w('    { .id = AC_PAR_STREAM,     .val = 0x%08x },' % hdr['formats'])
    w('    { .id = AC_PAR_AMP_IN_CAP, .val = 0x%08x },' % hdr.get('amp_in', 0))
    w('    { .id = AC_PAR_AMP_OUT_CAP, .val = 0x%08x },' % hdr.get('amp_out', 0))
    w('    { .id = AC_PAR_POWER_STATE, .val = 0x0000000f },')
    w('    { .id = AC_PAR_GPIO_CAP,   .val = 0x%08x },' % hdr.get('gpio', 0))
    w('};')
    w('')

    for n in nodes:
        nid = n['nid']
        wtype = (n['wcaps'] >> 20) & 0xf
        prm = [('AC_PAR_AUDIO_WIDGET_CAP', n['wcaps'])]
        if n['conn']:
            prm.append(('AC_PAR_CONNLIST_LEN', len(n['conn'])))
        if 'pincap' in n:
            prm.append(('AC_PAR_PIN_CAP', n['pincap']))
        prm.append(('AC_PAR_AMP_IN_CAP', n.get('amp_in', hdr.get('amp_in', 0))))
        prm.append(('AC_PAR_AMP_OUT_CAP', n.get('amp_out', hdr.get('amp_out', 0))))
        if wtype in (0, 1):
            nb = n.get('bits', bits)
            if not (n['wcaps'] & AC_WCAP_DIGITAL):
                nb = bits
            prm.append(('AC_PAR_PCM', (nb << 16) | n.get('rates', hdr['rates'])))
            prm.append(('AC_PAR_STREAM', n.get('formats', hdr['formats'])))
        if n['wcaps'] & (1 << 10):
            prm.append(('AC_PAR_POWER_STATE', 0x0000000f))
        w('static const desc_param %s_params_%02x[] = {' % (p, nid))
        for k, v in prm:
            w('    { .id = %s, .val = 0x%08x },' % (k, v))
        w('};')
        if n['conn']:
            w('static uint32_t %s_conn_%02x[] = { %s };'
              % (p, nid, ', '.join('0x%02x' % c for c in n['conn'])))
        w('')

    w('static const desc_node %s_nodes[] = {' % p)
    w('    {')
    w('        .nid     = AC_NODE_ROOT,')
    w('        .name    = "root",')
    w('        .params  = %s_params_root,' % p)
    w('        .nparams = ARRAY_SIZE(%s_params_root),' % p)
    w('        .stindex = HDA_NO_STREAM,')
    w('    },{')
    w('        .nid     = 1,')
    w('        .name    = "func",')
    w('        .params  = %s_params_afg,' % p)
    w('        .nparams = ARRAY_SIZE(%s_params_afg),' % p)
    w('        .stindex = HDA_NO_STREAM,')
    for n in nodes:
        nid = n['nid']
        w('    },{')
        w('        .nid     = 0x%02x,' % nid)
        w('        .name    = "%s",' % names.get(nid, '%s-%02x' % (p, nid)))
        w('        .params  = %s_params_%02x,' % (p, nid))
        w('        .nparams = ARRAY_SIZE(%s_params_%02x),' % (p, nid))
        if 'config' in n or nid in pins:
            w('        .config  = 0x%08x,' % pins.get(nid, n.get('config', 0)))
        if n['conn']:
            w('        .conn    = %s_conn_%02x,' % (p, nid))
        w('        .stindex = %s,' % (streams[nid] if nid in streams
                                       else 'HDA_NO_STREAM'))
    w('    },')
    w('};')
    w('')

    # power-on register values taken from the dump (amp gains/mutes and
    # selector positions); pin controls start at 0 as on a codec reset.
    w('static const HDANodeInit %s_node_init[] = {' % p)
    for n in nodes:
        ent = []
        if n['conn'] and n['sel']:
            ent.append('.conn_sel = %d' % n['sel'])
        if n['amp_out_vals']:
            v = n['amp_out_vals'][0]
            ent.append('.amp_out = { 0x%02x, 0x%02x }' % (v[0], v[-1]))
        if n['amp_in_vals']:
            ent.append('.amp_in = { %s }' % ', '.join(
                '{ 0x%02x, 0x%02x }' % (v[0], v[-1]) for v in n['amp_in_vals']))
        if ent:
            w('    { .nid = 0x%02x, %s },' % (n['nid'], ', '.join(ent)))
    w('};')
    w('')
    w('static const desc_codec %s = {' % p)
    w('    .name     = "%s",' % p)
    w('    .iid      = 0x%08x,' % subsys)
    w('    .nodes    = %s_nodes,' % p)
    w('    .nnodes   = ARRAY_SIZE(%s_nodes),' % p)
    w('    .stateful = true,')
    w('    .init     = %s_node_init,' % p)
    w('    .ninit    = ARRAY_SIZE(%s_node_init),' % p)
    w('};')
    sys.stdout.write('\n'.join(o) + '\n')


if __name__ == '__main__':
    main()
