/*
 * Sealevel Systems 3096 (DIO-16) ISA digital I/O adapter
 *
 * Eight optically isolated inputs and eight normally-open reed relays on a
 * DB-37.  The card decodes four consecutive I/O ports (A9-A2 on the DIP
 * switch):
 *
 *   base+0  R   Port A, opto inputs, active low (lit opto reads 0)
 *   base+1  R   unassigned, reads 0
 *   base+2  R/W Port C, relay latch, 1 = relay closed; reads back the
 *               ones complement of the latch
 *   base+3  R   unassigned, reads 0
 *
 * The newer manual's IRQEN/IRQST register at base+5 belongs to later boards
 * and cannot exist on one that decodes only four ports.  On this card the
 * interrupt is a jumper (E2) that drives the selected IRQ from Port A bit 0
 * going low; set "irq" to model that jumper.  Neither the Sealevel NT driver
 * nor WinRTDIO uses it - both poll.
 *
 * Host side, through QOM (qom-get/qom-set on the device path):
 *   inputs   u8 RW  opto inputs, 1 = voltage applied (opto lit)
 *   relays   u8 RO  relay contacts, 1 = closed
 *
 * or through chardevs (see include/hw/misc/sealevel-chan.h), same polarity:
 *   chardev-a / invert-a   opto inputs in
 *   chardev-c / invert-c   relay contacts out
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/sealevel-chan.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_SEALEVEL_3096 "sealevel-3096"
OBJECT_DECLARE_SIMPLE_TYPE(Sealevel3096State, SEALEVEL_3096)

#define SL3096_IOSIZE   4
#define SL3096_PORT_A   0
#define SL3096_PORT_C   2

struct Sealevel3096State {
    ISADevice parent_obj;

    MemoryRegion io;
    qemu_irq irq;

    /* properties */
    uint32_t iobase;
    uint32_t isairq;            /* 0 = jumper E2 off */
    bool readback_inverted;

    /* state */
    uint8_t inputs;             /* 1 = opto lit */
    uint8_t relays;             /* 1 = relay closed */

    SeaIOChan chan_a;
    SeaIOChan chan_c;
};

static void sl3096_update_irq(Sealevel3096State *s)
{
    if (s->irq) {
        /* PA0 reads low while its opto is lit; that low drives the IRQ. */
        qemu_set_irq(s->irq, s->inputs & 1);
    }
}

static uint64_t sl3096_read(void *opaque, hwaddr addr, unsigned size)
{
    Sealevel3096State *s = opaque;
    uint8_t val = 0;

    switch (addr) {
    case SL3096_PORT_A:
        val = ~s->inputs;
        break;
    case SL3096_PORT_C:
        val = s->readback_inverted ? ~s->relays : s->relays;
        break;
    default:
        break;
    }
    trace_sealevel_3096_read(addr, val);
    return val;
}

static void sl3096_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    Sealevel3096State *s = opaque;

    trace_sealevel_3096_write(addr, val);
    if (addr != SL3096_PORT_C) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sealevel-3096: write 0x%02" PRIx64
                      " to read-only port base+%" HWADDR_PRIu "\n",
                      val, addr);
        return;
    }
    if (s->relays != (uint8_t)val) {
        trace_sealevel_3096_relays(s->relays, val);
        s->relays = val;
        sealevel_chan_update(&s->chan_c);
    }
}

static const MemoryRegionOps sl3096_ops = {
    .read = sl3096_read,
    .write = sl3096_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void sl3096_apply_inputs(Sealevel3096State *s, uint8_t val)
{
    trace_sealevel_3096_inputs(s->inputs, val);
    s->inputs = val;
    sl3096_update_irq(s);
}

static int sl3096_chan_get_output(void *opaque, unsigned port)
{
    Sealevel3096State *s = opaque;
    return port == SL3096_PORT_C ? s->relays : -1;
}

static uint8_t sl3096_chan_get_input(void *opaque, unsigned port)
{
    Sealevel3096State *s = opaque;
    return port == SL3096_PORT_A ? s->inputs : 0;
}

static void sl3096_chan_set_input(void *opaque, unsigned port, uint8_t val)
{
    sl3096_apply_inputs(opaque, val);
}

static const SeaIOChanOps sl3096_chan_a_ops = {
    .get_output = sl3096_chan_get_output,
    .get_input = sl3096_chan_get_input,
    .set_input = sl3096_chan_set_input,
};

static const SeaIOChanOps sl3096_chan_c_ops = {
    .get_output = sl3096_chan_get_output,
    .get_input = sl3096_chan_get_input,
};

static void sl3096_get_inputs(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Sealevel3096State *s = SEALEVEL_3096(obj);
    visit_type_uint8(v, name, &s->inputs, errp);
}

static void sl3096_set_inputs(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Sealevel3096State *s = SEALEVEL_3096(obj);
    uint8_t val;

    if (!visit_type_uint8(v, name, &val, errp)) {
        return;
    }
    sl3096_apply_inputs(s, val);
}

static void sl3096_get_relays(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Sealevel3096State *s = SEALEVEL_3096(obj);
    visit_type_uint8(v, name, &s->relays, errp);
}

static void sl3096_realize(DeviceState *dev, Error **errp)
{
    Sealevel3096State *s = SEALEVEL_3096(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    if (s->iobase < 0x100 || s->iobase > 0x3fc || (s->iobase & 3)) {
        error_setg(errp, "iobase 0x%x: the DIP switch selects 0x100-0x3fc "
                   "on a 4-port boundary", s->iobase);
        return;
    }
    if (s->isairq) {
        if (s->isairq < 2 || s->isairq > 9 || s->isairq == 8) {
            error_setg(errp, "irq %u: jumper E2 selects IRQ 2/9-7, or 0 "
                       "for none", s->isairq);
            return;
        }
        /* Header position 2 is the cascade; on an AT it lands on IRQ 9. */
        s->irq = isa_get_irq(isa, s->isairq == 2 ? 9 : s->isairq);
    }

    memory_region_init_io(&s->io, OBJECT(dev), &sl3096_ops, s,
                          TYPE_SEALEVEL_3096, SL3096_IOSIZE);
    isa_register_ioport(isa, &s->io, s->iobase);

    sealevel_chan_realize(&s->chan_a, &sl3096_chan_a_ops, s,
                          SL3096_PORT_A, "3096.A");
    sealevel_chan_realize(&s->chan_c, &sl3096_chan_c_ops, s,
                          SL3096_PORT_C, "3096.C");
}

static void sl3096_reset(DeviceState *dev)
{
    Sealevel3096State *s = SEALEVEL_3096(dev);

    /* Relays drop out; the inputs are external and survive a reset. */
    s->relays = 0;
    sl3096_update_irq(s);
    sealevel_chan_update(&s->chan_c);
}

static void sl3096_instance_init(Object *obj)
{
    object_property_add(obj, "inputs", "uint8",
                        sl3096_get_inputs, sl3096_set_inputs, NULL, NULL);
    object_property_set_description(obj, "inputs",
        "Opto inputs, bit set = voltage applied (reads 0 on Port A)");
    object_property_add(obj, "relays", "uint8",
                        sl3096_get_relays, NULL, NULL, NULL);
    object_property_set_description(obj, "relays",
        "Relay contacts, bit set = closed (K1 = bit 0)");
}

static const VMStateDescription vmstate_sl3096 = {
    .name = TYPE_SEALEVEL_3096,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(inputs, Sealevel3096State),
        VMSTATE_UINT8(relays, Sealevel3096State),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sl3096_properties[] = {
    DEFINE_PROP_UINT32("iobase", Sealevel3096State, iobase, 0x300),
    DEFINE_PROP_UINT32("irq", Sealevel3096State, isairq, 0),
    DEFINE_PROP_BOOL("readback-inverted", Sealevel3096State,
                     readback_inverted, true),
    DEFINE_PROP_CHR("chardev-a", Sealevel3096State, chan_a.chr),
    DEFINE_PROP_BOOL("invert-a", Sealevel3096State, chan_a.invert, false),
    DEFINE_PROP_CHR("chardev-c", Sealevel3096State, chan_c.chr),
    DEFINE_PROP_BOOL("invert-c", Sealevel3096State, chan_c.invert, false),
};

static void sl3096_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Sealevel 3096 DIO-16 (8 reed relays, 8 opto inputs)";
    dc->realize = sl3096_realize;
    dc->vmsd = &vmstate_sl3096;
    device_class_set_legacy_reset(dc, sl3096_reset);
    device_class_set_props(dc, sl3096_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sl3096_info = {
    .name          = TYPE_SEALEVEL_3096,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Sealevel3096State),
    .instance_init = sl3096_instance_init,
    .class_init    = sl3096_class_init,
};

static void sl3096_register_types(void)
{
    type_register_static(&sl3096_info);
}

type_init(sl3096_register_types)
