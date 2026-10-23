/*
 * Sealevel Systems 8001 (PIO-32.PCI) digital I/O adapter
 *
 * 32 TTL lines on a DB-37 as four 8-bit ports, each switchable between
 * input and output.  Eight I/O ports in BAR2, which is the only BAR the
 * Linux SeaIO driver reads:
 *
 *   base+0..3  R/W  Ports A-D.  A write always lands in the port's output
 *                   latch (so a port can be preset while still an input).
 *                   A read returns the latch for an output port and the
 *                   external lines for an input port.  Active high.
 *   base+4     R/W  DIRA-DIRD in bits 0-3, 1 = output.  0 after reset.
 *   base+5     R/W  IRQEN (7), IRQST (6, read clears), IRC1:IRC0 (1:0)
 *   base+6..7  R    0
 *
 * The interrupt (source Port A bit 0) is stored but never raised: the
 * DASDEC's seaio_driver.ko never requests an IRQ and only polls.
 *
 * Host side, through QOM (qom-get/qom-set on the device path):
 *   inputs     u32 RW  external levels, Port A in bits 0-7 ... D in 24-31
 *   outputs    u32 RO  levels driven by the card, output ports only
 *   direction  u8  RO  DIRA-DIRD
 *
 * or through chardevs (see include/hw/misc/sealevel-chan.h), one per port:
 *   chardev-a..d / invert-a..d   reports the port while it is an output,
 *                                accepts its input lines while an input
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/sealevel-chan.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_SEALEVEL_8001 "sealevel-8001"
OBJECT_DECLARE_SIMPLE_TYPE(Sealevel8001State, SEALEVEL_8001)

#define PCI_VENDOR_ID_SEALEVEL      0x135e
#define PCI_DEVICE_ID_SEALEVEL_8001 0x8001

#define SL8001_BAR      2
#define SL8001_IOSIZE   8
#define SL8001_NPORTS   4
#define SL8001_DIR      4
#define SL8001_IRQCTL   5

#define SL8001_IRQEN    0x80
#define SL8001_IRQST    0x40
#define SL8001_IRC      0x03

struct Sealevel8001State {
    PCIDevice parent_obj;

    MemoryRegion io;

    uint8_t latch[SL8001_NPORTS];
    uint32_t inputs;            /* external lines, A in bits 0-7 */
    uint8_t dir;
    uint8_t irqctl;

    SeaIOChan chan[SL8001_NPORTS];
};

static uint32_t sl8001_outputs(Sealevel8001State *s)
{
    uint32_t out = 0;

    for (int i = 0; i < SL8001_NPORTS; i++) {
        if (s->dir & BIT(i)) {
            out |= (uint32_t)s->latch[i] << (8 * i);
        }
    }
    return out;
}

static uint64_t sl8001_read(void *opaque, hwaddr addr, unsigned size)
{
    Sealevel8001State *s = opaque;
    uint8_t val = 0;

    switch (addr) {
    case 0 ... SL8001_NPORTS - 1:
        val = (s->dir & BIT(addr)) ? s->latch[addr]
                                   : extract32(s->inputs, 8 * addr, 8);
        break;
    case SL8001_DIR:
        val = s->dir;
        break;
    case SL8001_IRQCTL:
        val = s->irqctl;
        s->irqctl &= ~SL8001_IRQST;
        break;
    default:
        break;
    }
    trace_sealevel_8001_read(addr, val);
    return val;
}

static void sl8001_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    Sealevel8001State *s = opaque;
    uint32_t before = sl8001_outputs(s);
    uint32_t after;

    trace_sealevel_8001_write(addr, val);
    switch (addr) {
    case 0 ... SL8001_NPORTS - 1:
        s->latch[addr] = val;
        break;
    case SL8001_DIR:
        s->dir = val & 0x0f;
        break;
    case SL8001_IRQCTL:
        s->irqctl = (s->irqctl & SL8001_IRQST) |
                    (val & (SL8001_IRQEN | SL8001_IRC));
        if (val & SL8001_IRQEN) {
            qemu_log_mask(LOG_UNIMP,
                          "sealevel-8001: interrupts are not modelled\n");
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sealevel-8001: write 0x%02" PRIx64
                      " to read-only port base+%" HWADDR_PRIu "\n",
                      val, addr);
        break;
    }

    after = sl8001_outputs(s);
    if (after != before) {
        trace_sealevel_8001_outputs(s->dir, before, after);
    }
    for (int i = 0; i < SL8001_NPORTS; i++) {
        sealevel_chan_update(&s->chan[i]);
    }
}

static const MemoryRegionOps sl8001_ops = {
    .read = sl8001_read,
    .write = sl8001_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int sl8001_chan_get_output(void *opaque, unsigned port)
{
    Sealevel8001State *s = opaque;
    return (s->dir & BIT(port)) ? s->latch[port] : -1;
}

static uint8_t sl8001_chan_get_input(void *opaque, unsigned port)
{
    Sealevel8001State *s = opaque;
    return extract32(s->inputs, 8 * port, 8);
}

static void sl8001_chan_set_input(void *opaque, unsigned port, uint8_t val)
{
    Sealevel8001State *s = opaque;
    uint32_t inputs = deposit32(s->inputs, 8 * port, 8, val);

    trace_sealevel_8001_inputs(s->inputs, inputs);
    s->inputs = inputs;
}

static const SeaIOChanOps sl8001_chan_ops = {
    .get_output = sl8001_chan_get_output,
    .get_input = sl8001_chan_get_input,
    .set_input = sl8001_chan_set_input,
};

static void sl8001_get_inputs(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Sealevel8001State *s = SEALEVEL_8001(obj);
    visit_type_uint32(v, name, &s->inputs, errp);
}

static void sl8001_set_inputs(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    Sealevel8001State *s = SEALEVEL_8001(obj);
    uint32_t val;

    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    trace_sealevel_8001_inputs(s->inputs, val);
    s->inputs = val;
}

static void sl8001_get_outputs(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint32_t val = sl8001_outputs(SEALEVEL_8001(obj));
    visit_type_uint32(v, name, &val, errp);
}

static void sl8001_get_direction(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sealevel8001State *s = SEALEVEL_8001(obj);
    visit_type_uint8(v, name, &s->dir, errp);
}

static void sl8001_realize(PCIDevice *pdev, Error **errp)
{
    Sealevel8001State *s = SEALEVEL_8001(pdev);

    pdev->config[PCI_INTERRUPT_PIN] = 1;
    memory_region_init_io(&s->io, OBJECT(s), &sl8001_ops, s,
                          TYPE_SEALEVEL_8001, SL8001_IOSIZE);
    pci_register_bar(pdev, SL8001_BAR, PCI_BASE_ADDRESS_SPACE_IO, &s->io);

    for (int i = 0; i < SL8001_NPORTS; i++) {
        static const char *const names[] = {
            "8001.A", "8001.B", "8001.C", "8001.D",
        };
        sealevel_chan_realize(&s->chan[i], &sl8001_chan_ops, s, i, names[i]);
    }
}

static void sl8001_reset(DeviceState *dev)
{
    Sealevel8001State *s = SEALEVEL_8001(dev);

    /* All ports come up as inputs with interrupts off. */
    memset(s->latch, 0, sizeof(s->latch));
    s->dir = 0;
    s->irqctl = 0;
    for (int i = 0; i < SL8001_NPORTS; i++) {
        sealevel_chan_update(&s->chan[i]);
    }
}

static void sl8001_instance_init(Object *obj)
{
    object_property_add(obj, "inputs", "uint32",
                        sl8001_get_inputs, sl8001_set_inputs, NULL, NULL);
    object_property_set_description(obj, "inputs",
        "External line levels, Port A = bits 0-7 ... Port D = bits 24-31");
    object_property_add(obj, "outputs", "uint32",
                        sl8001_get_outputs, NULL, NULL, NULL);
    object_property_set_description(obj, "outputs",
        "Levels the card drives, output ports only");
    object_property_add(obj, "direction", "uint8",
                        sl8001_get_direction, NULL, NULL, NULL);
    object_property_set_description(obj, "direction",
        "DIRA-DIRD in bits 0-3, 1 = output");
}

static const VMStateDescription vmstate_sl8001 = {
    .name = TYPE_SEALEVEL_8001,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Sealevel8001State),
        VMSTATE_UINT8_ARRAY(latch, Sealevel8001State, SL8001_NPORTS),
        VMSTATE_UINT32(inputs, Sealevel8001State),
        VMSTATE_UINT8(dir, Sealevel8001State),
        VMSTATE_UINT8(irqctl, Sealevel8001State),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sl8001_properties[] = {
    DEFINE_PROP_CHR("chardev-a", Sealevel8001State, chan[0].chr),
    DEFINE_PROP_BOOL("invert-a", Sealevel8001State, chan[0].invert, false),
    DEFINE_PROP_CHR("chardev-b", Sealevel8001State, chan[1].chr),
    DEFINE_PROP_BOOL("invert-b", Sealevel8001State, chan[1].invert, false),
    DEFINE_PROP_CHR("chardev-c", Sealevel8001State, chan[2].chr),
    DEFINE_PROP_BOOL("invert-c", Sealevel8001State, chan[2].invert, false),
    DEFINE_PROP_CHR("chardev-d", Sealevel8001State, chan[3].chr),
    DEFINE_PROP_BOOL("invert-d", Sealevel8001State, chan[3].invert, false),
};

static void sl8001_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = sl8001_realize;
    k->vendor_id = PCI_VENDOR_ID_SEALEVEL;
    k->device_id = PCI_DEVICE_ID_SEALEVEL_8001;
    k->class_id = PCI_CLASS_OTHERS << 8;
    dc->desc = "Sealevel 8001 PIO-32.PCI (32 TTL digital I/O lines)";
    dc->vmsd = &vmstate_sl8001;
    device_class_set_legacy_reset(dc, sl8001_reset);
    device_class_set_props(dc, sl8001_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sl8001_info = {
    .name          = TYPE_SEALEVEL_8001,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Sealevel8001State),
    .instance_init = sl8001_instance_init,
    .class_init    = sl8001_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void sl8001_register_types(void)
{
    type_register_static(&sl8001_info);
}

type_init(sl8001_register_types)
