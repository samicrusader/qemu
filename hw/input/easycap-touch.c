/*
 * Trilithic EASyCAP front-panel touchscreen
 *
 * On the real unit the 3.5" 320x240 resistive panel is read by the
 * microcontroller on the carrier board, which reports touches to the host
 * as "~TS<flag><xxxx><yyyy>\r" on one port of an FT4232H.  The carrier MCU
 * is emulated outside QEMU (it sits behind a usb-serial chardev), so this
 * device only turns the display's pointer into touches and hands them to
 * that emulator over a chardev:
 *
 *   down X Y\n     finger down at front-panel pixel X,Y
 *   move X Y\n     finger moved while down (clamped to the panel)
 *   up\n           finger lifted
 *
 * X/Y are pixels inside the panel region of the guest screen (x/y/width/
 * height properties; the EASyCAP draws its 320x240 UI at +0+0).  Presses
 * outside the region are ignored.  The device registers an absolute pointer
 * handler and activates it, so the UI shows the host cursor ungrabbed and
 * the guest's own mouse never moves: the panel behaves like a touchscreen.
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_EASYCAP_TOUCH "easycap-touch"
OBJECT_DECLARE_SIMPLE_TYPE(EasycapTouchState, EASYCAP_TOUCH)

struct EasycapTouchState {
    DeviceState parent_obj;

    CharFrontend chr;
    QemuInputHandlerState *hs;
    QemuConsole *src;

    /* panel region in guest screen pixels */
    uint32_t x, y, width, height;

    int32_t abs_x, abs_y;
    bool button;
    bool touching;
    int32_t last_x, last_y;
};

static void easycap_touch_send(EasycapTouchState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void easycap_touch_send(EasycapTouchState *s, const char *fmt, ...)
{
    g_autofree char *line = NULL;
    va_list ap;

    va_start(ap, fmt);
    line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)line, strlen(line));
}

static void easycap_touch_event(DeviceState *dev, QemuConsole *src,
                                QemuInputEvent *evt)
{
    EasycapTouchState *s = EASYCAP_TOUCH(dev);

    if (src) {
        s->src = src;
    }
    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->abs_x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->abs_y = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->button = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void easycap_touch_sync(DeviceState *dev)
{
    EasycapTouchState *s = EASYCAP_TOUCH(dev);
    QemuConsole *con = s->src ? s->src : qemu_console_lookup_by_index(0);
    int w = qemu_console_get_width(con, 640);
    int h = qemu_console_get_height(con, 480);
    int64_t px = (int64_t)s->abs_x * w / (INPUT_EVENT_ABS_MAX + 1);
    int64_t py = (int64_t)s->abs_y * h / (INPUT_EVENT_ABS_MAX + 1);
    int64_t lx = px - s->x;
    int64_t ly = py - s->y;
    bool inside = lx >= 0 && lx < s->width && ly >= 0 && ly < s->height;

    lx = MIN(MAX(lx, 0), (int64_t)s->width - 1);
    ly = MIN(MAX(ly, 0), (int64_t)s->height - 1);

    if (s->button && !s->touching) {
        if (inside) {
            s->touching = true;
            s->last_x = lx;
            s->last_y = ly;
            easycap_touch_send(s, "down %d %d\n", s->last_x, s->last_y);
        }
    } else if (s->button && s->touching) {
        if (lx != s->last_x || ly != s->last_y) {
            s->last_x = lx;
            s->last_y = ly;
            easycap_touch_send(s, "move %d %d\n", s->last_x, s->last_y);
        }
    } else if (!s->button && s->touching) {
        s->touching = false;
        easycap_touch_send(s, "up\n");
    }
}

static const QemuInputHandler easycap_touch_handler = {
    .name  = "EASyCAP front-panel touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = easycap_touch_event,
    .sync  = easycap_touch_sync,
};

static void easycap_touch_realize(DeviceState *dev, Error **errp)
{
    EasycapTouchState *s = EASYCAP_TOUCH(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "easycap-touch: 'chardev' property is required");
        return;
    }
    if (!s->width || !s->height) {
        error_setg(errp, "easycap-touch: panel width/height must be non-zero");
        return;
    }
    s->hs = qemu_input_handler_register(dev, &easycap_touch_handler);
    qemu_input_handler_activate(s->hs);
}

static void easycap_touch_unrealize(DeviceState *dev)
{
    EasycapTouchState *s = EASYCAP_TOUCH(dev);

    if (s->hs) {
        qemu_input_handler_unregister(s->hs);
        s->hs = NULL;
    }
}

static const Property easycap_touch_properties[] = {
    DEFINE_PROP_CHR("chardev", EasycapTouchState, chr),
    DEFINE_PROP_UINT32("x", EasycapTouchState, x, 0),
    DEFINE_PROP_UINT32("y", EasycapTouchState, y, 0),
    DEFINE_PROP_UINT32("width", EasycapTouchState, width, 320),
    DEFINE_PROP_UINT32("height", EasycapTouchState, height, 240),
};

static void easycap_touch_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Trilithic EASyCAP front-panel touchscreen (pointer to "
               "touch events on a chardev)";
    dc->realize = easycap_touch_realize;
    dc->unrealize = easycap_touch_unrealize;
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    device_class_set_props(dc, easycap_touch_properties);
}

static const TypeInfo easycap_touch_info = {
    .name          = TYPE_EASYCAP_TOUCH,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(EasycapTouchState),
    .class_init    = easycap_touch_class_init,
};

static void easycap_touch_register_types(void)
{
    type_register_static(&easycap_touch_info);
}

type_init(easycap_touch_register_types)
