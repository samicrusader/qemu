/*
 * QTest testcases for the Sealevel SeaIO digital I/O cards
 * (sealevel-3096 ISA, sealevel-8001 PCI) and their chardev channels
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "hw/pci/pci_regs.h"

#define SL3096_BASE     0x300
#define SL8001_DEVFN    QPCI_DEVFN(0x5, 0)

/* ---- QOM helpers ------------------------------------------------------ */

static uint64_t qom_get(QTestState *qts, const char *id, const char *prop)
{
    g_autofree char *path = g_strdup_printf("/machine/peripheral/%s", id);
    QDict *rsp;
    uint64_t val;

    rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                    "{ 'path': %s, 'property': %s } }", path, prop);
    g_assert(qdict_haskey(rsp, "return"));
    val = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return val;
}

static void qom_set(QTestState *qts, const char *id, const char *prop,
                    uint64_t val)
{
    g_autofree char *path = g_strdup_printf("/machine/peripheral/%s", id);
    QDict *rsp;

    rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                    "{ 'path': %s, 'property': %s, 'value': %" PRIu64 " } }",
                    path, prop, val);
    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
}

/* Chardev input lands asynchronously: poll the QOM view until it settles. */
static void qom_wait(QTestState *qts, const char *id, const char *prop,
                     uint64_t mask, uint64_t want)
{
    for (int i = 0; i < 500; i++) {
        if ((qom_get(qts, id, prop) & mask) == want) {
            return;
        }
        g_usleep(10 * 1000);
    }
    g_assert_cmphex(qom_get(qts, id, prop) & mask, ==, want);
}

static void system_reset(QTestState *qts)
{
    qobject_unref(qtest_qmp(qts, "{ 'execute': 'system_reset' }"));
    qtest_qmp_eventwait(qts, "RESET");
}

/* ---- 3096 registers --------------------------------------------------- */

static void test_3096_regs(void)
{
    QTestState *qts = qtest_init("-device sealevel-3096,id=dio");

    /* Nothing applied: every opto dark, port A reads all ones. */
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 0), ==, 0xff);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 1), ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 3), ==, 0x00);

    /* Inputs are active low on the bus. */
    qom_set(qts, "dio", "inputs", 0x05);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 0), ==, 0xfa);

    /* Port A is read-only. */
    qtest_outb(qts, SL3096_BASE + 0, 0x00);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 0), ==, 0xfa);

    /* Relays: 1 closes, read back as the ones complement. */
    qtest_outb(qts, SL3096_BASE + 2, 0x81);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 2), ==, 0x7e);
    g_assert_cmphex(qom_get(qts, "dio", "relays"), ==, 0x81);

    /* Only four ports are decoded. */
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 4), ==, 0xff);

    /* Reset drops the relays and leaves the external inputs alone. */
    system_reset(qts);
    g_assert_cmphex(qom_get(qts, "dio", "relays"), ==, 0x00);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 2), ==, 0xff);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 0), ==, 0xfa);

    qtest_quit(qts);
}

static void test_3096_options(void)
{
    QTestState *qts = qtest_init(
        "-device sealevel-3096,id=dio,iobase=0x280,readback-inverted=off");

    qtest_outb(qts, 0x282, 0x81);
    g_assert_cmphex(qtest_inb(qts, 0x282), ==, 0x81);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 2), ==, 0xff);

    qtest_quit(qts);
}

static void test_3096_irq(void)
{
    QTestState *qts = qtest_init("-device sealevel-3096,id=dio,irq=5");

    qtest_irq_intercept_in(qts, "ioapic");
    g_assert_false(qtest_get_irq(qts, 5));

    /* The jumper follows PA0 going low, i.e. opto 0 lit. */
    qom_set(qts, "dio", "inputs", 0x01);
    g_assert_true(qtest_get_irq(qts, 5));
    qom_set(qts, "dio", "inputs", 0x02);
    g_assert_false(qtest_get_irq(qts, 5));

    qtest_quit(qts);
}

/* ---- 8001 registers --------------------------------------------------- */

typedef struct Card8001 {
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar bar;
} Card8001;

static void card8001_init(QTestState *qts, Card8001 *c)
{
    uint64_t size;

    c->bus = qpci_new_pc(qts, NULL);
    c->dev = qpci_device_find(c->bus, SL8001_DEVFN);
    g_assert(c->dev);
    qpci_device_enable(c->dev);
    c->bar = qpci_iomap(c->dev, 2, &size);
    g_assert_cmpuint(size, ==, 8);
}

static void card8001_free(Card8001 *c)
{
    g_free(c->dev);
    qpci_free_pc(c->bus);
}

static uint8_t rd8001(Card8001 *c, unsigned off)
{
    return qpci_io_readb(c->dev, c->bar, off);
}

static void wr8001(Card8001 *c, unsigned off, uint8_t v)
{
    qpci_io_writeb(c->dev, c->bar, off, v);
}

static void test_8001_pci(void)
{
    QTestState *qts = qtest_init("-device sealevel-8001,id=pio,addr=05.0");
    Card8001 c;

    card8001_init(qts, &c);
    g_assert_cmphex(qpci_config_readw(c.dev, PCI_VENDOR_ID), ==, 0x135e);
    g_assert_cmphex(qpci_config_readw(c.dev, PCI_DEVICE_ID), ==, 0x8001);
    g_assert_cmphex(qpci_config_readb(c.dev, PCI_INTERRUPT_PIN), ==, 1);
    /* seaio_driver.ko only ever reads BAR2. */
    g_assert_cmphex(qpci_config_readl(c.dev, PCI_BASE_ADDRESS_0), ==, 0);
    g_assert_cmphex(qpci_config_readl(c.dev, PCI_BASE_ADDRESS_2) &
                    PCI_BASE_ADDRESS_SPACE, ==, PCI_BASE_ADDRESS_SPACE_IO);

    card8001_free(&c);
    qtest_quit(qts);
}

static void test_8001_regs(void)
{
    QTestState *qts = qtest_init("-device sealevel-8001,id=pio,addr=05.0");
    Card8001 c;

    card8001_init(qts, &c);
    qom_set(qts, "pio", "inputs", 0xaabbccdd);

    /* Reset state: all ports inputs, reading the external lines. */
    g_assert_cmphex(rd8001(&c, 0), ==, 0xdd);
    g_assert_cmphex(rd8001(&c, 1), ==, 0xcc);
    g_assert_cmphex(rd8001(&c, 2), ==, 0xbb);
    g_assert_cmphex(rd8001(&c, 3), ==, 0xaa);
    g_assert_cmphex(rd8001(&c, 4), ==, 0x00);
    g_assert_cmphex(qom_get(qts, "pio", "outputs"), ==, 0);

    /* Presetting: the latch takes the write, the pins don't change yet. */
    wr8001(&c, 1, 0x55);
    g_assert_cmphex(rd8001(&c, 1), ==, 0xcc);
    g_assert_cmphex(qom_get(qts, "pio", "outputs"), ==, 0);

    wr8001(&c, 4, 0x02);
    g_assert_cmphex(rd8001(&c, 4), ==, 0x02);
    g_assert_cmphex(rd8001(&c, 1), ==, 0x55);
    g_assert_cmphex(qom_get(qts, "pio", "outputs"), ==, 0x5500);
    g_assert_cmphex(qom_get(qts, "pio", "direction"), ==, 0x02);

    /* Only the low nibble of the direction register exists. */
    wr8001(&c, 4, 0xf6);
    g_assert_cmphex(rd8001(&c, 4), ==, 0x06);

    /* IRQ control: stored, IRQST never set, +6/+7 read zero. */
    wr8001(&c, 5, 0x83);
    g_assert_cmphex(rd8001(&c, 5), ==, 0x83);
    g_assert_cmphex(rd8001(&c, 6), ==, 0x00);
    g_assert_cmphex(rd8001(&c, 7), ==, 0x00);

    /* Wide access splits into byte ports (C is now an output, latch 0). */
    g_assert_cmphex(qpci_io_readl(c.dev, c.bar, 0), ==, 0xaa0055dd);

    system_reset(qts);
    card8001_free(&c);
    card8001_init(qts, &c);
    g_assert_cmphex(rd8001(&c, 4), ==, 0x00);
    g_assert_cmphex(rd8001(&c, 5), ==, 0x00);
    g_assert_cmphex(rd8001(&c, 1), ==, 0xcc);
    g_assert_cmphex(qom_get(qts, "pio", "outputs"), ==, 0);

    card8001_free(&c);
    qtest_quit(qts);
}

/* ---- chardev channels ------------------------------------------------- */

typedef struct Chan {
    int listen_fd;
    int fd;
    char *path;
    char buf[64];
    size_t len;
} Chan;

static void chan_listen(Chan *ch, const char *dir, const char *name)
{
    memset(ch, 0, sizeof(*ch));
    ch->path = g_build_filename(dir, name, NULL);
    ch->listen_fd = qtest_socket_server(ch->path);
    ch->fd = -1;
}

static char *chan_arg(Chan *ch, const char *id)
{
    return g_strdup_printf("-chardev socket,id=%s,path=%s ", id, ch->path);
}

static void chan_set_timeout(int fd, int ms)
{
#ifndef _WIN32
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
#else
    DWORD tv = ms;
#endif
    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                               (void *)&tv, sizeof(tv)), ==, 0);
}

static void chan_accept(Chan *ch)
{
    ch->fd = accept(ch->listen_fd, NULL, NULL);
    g_assert_cmpint(ch->fd, >=, 0);
    close(ch->listen_fd);
    ch->listen_fd = -1;
}

/* Next line from the card, or NULL if nothing arrives within @ms. */
static const char *chan_line(Chan *ch, int ms)
{
    static char line[64];
    char *nl;

    chan_set_timeout(ch->fd, ms);
    while (!(nl = memchr(ch->buf, '\n', ch->len))) {
        ssize_t n = recv(ch->fd, ch->buf + ch->len,
                         sizeof(ch->buf) - ch->len, 0);
        if (n <= 0) {
            return NULL;
        }
        ch->len += n;
    }
    *nl = '\0';
    g_strlcpy(line, ch->buf, sizeof(line));
    ch->len -= nl + 1 - ch->buf;
    memmove(ch->buf, nl + 1, ch->len);
    return line;
}

static void chan_expect(Chan *ch, const char *want)
{
    const char *got = chan_line(ch, 5000);

    g_assert_nonnull(got);
    g_assert_cmpstr(got, ==, want);
}

static void chan_expect_nothing(Chan *ch)
{
    g_assert_null(chan_line(ch, 300));
}

static void chan_send(Chan *ch, const char *text)
{
    size_t len = strlen(text);

    g_assert_cmpint(send(ch->fd, text, len, 0), ==, len);
}

static void chan_close(Chan *ch)
{
    if (ch->fd >= 0) {
        close(ch->fd);
    }
    if (ch->listen_fd >= 0) {
        close(ch->listen_fd);
    }
    unlink(ch->path);
    g_free(ch->path);
}

static void test_3096_chardev(void)
{
    g_autofree char *dir = g_dir_make_tmp("sealevel-XXXXXX", NULL);
    g_autofree char *args = NULL;
    g_autofree char *ca = NULL, *cc = NULL;
    Chan in, out;
    QTestState *qts;

    chan_listen(&in, dir, "a.sock");
    chan_listen(&out, dir, "c.sock");
    ca = chan_arg(&in, "ca");
    cc = chan_arg(&out, "cc");
    args = g_strdup_printf("%s%s-device sealevel-3096,id=dio,"
                           "chardev-a=ca,chardev-c=cc", ca, cc);
    qts = qtest_init(args);
    chan_accept(&in);
    chan_accept(&out);

    /* Relays announce themselves on connect; the input port stays quiet. */
    chan_expect(&out, "00");
    chan_expect_nothing(&in);

    /* Every relay change is pushed; a repeated value is not. */
    qtest_outb(qts, SL3096_BASE + 2, 0x01);
    chan_expect(&out, "01");
    qtest_outb(qts, SL3096_BASE + 2, 0x81);
    chan_expect(&out, "81");
    qtest_outb(qts, SL3096_BASE + 2, 0x81);
    chan_expect_nothing(&out);
    chan_send(&out, "?\n");
    chan_expect(&out, "81");

    /* Inputs: byte, then single bits, then junk that must be ignored. */
    chan_send(&in, "3c\n");
    qom_wait(qts, "dio", "inputs", 0xff, 0x3c);
    chan_send(&in, "+7\r\n-2\nzz\n+9\n0x1ff\n");
    qom_wait(qts, "dio", "inputs", 0xff, 0xb8);
    g_assert_cmphex(qtest_inb(qts, SL3096_BASE + 0), ==, 0x47);
    chan_send(&in, "?\n");
    chan_expect_nothing(&in);

    /* The relay port does not take input. */
    chan_send(&out, "ff\n");
    chan_send(&in, "00\n");
    qom_wait(qts, "dio", "inputs", 0xff, 0x00);
    g_assert_cmphex(qom_get(qts, "dio", "relays"), ==, 0x81);

    /* Reset pushes the relays dropping out. */
    system_reset(qts);
    chan_expect(&out, "00");

    qtest_quit(qts);
    chan_close(&in);
    chan_close(&out);
    rmdir(dir);
}

static void test_8001_chardev(void)
{
    g_autofree char *dir = g_dir_make_tmp("sealevel-XXXXXX", NULL);
    g_autofree char *args = NULL;
    g_autofree char *cb = NULL, *cd = NULL;
    Chan pb, pd;
    Card8001 c;
    QTestState *qts;

    chan_listen(&pb, dir, "b.sock");
    chan_listen(&pd, dir, "d.sock");
    cb = chan_arg(&pb, "cb");
    cd = chan_arg(&pd, "cd");
    args = g_strdup_printf("%s%s-device sealevel-8001,id=pio,addr=05.0,"
                           "chardev-b=cb,invert-b=on,chardev-d=cd", cb, cd);
    qts = qtest_init(args);
    chan_accept(&pb);
    chan_accept(&pd);
    card8001_init(qts, &c);

    /* Every port starts as an input: nothing is announced. */
    chan_expect_nothing(&pb);
    chan_expect_nothing(&pd);

    /* A preset latch is only reported once the port turns to output. */
    wr8001(&c, 1, 0x0f);
    chan_expect_nothing(&pb);
    wr8001(&c, 4, 0x02);
    chan_expect(&pb, "f0");            /* invert-b */
    wr8001(&c, 1, 0x3c);
    chan_expect(&pb, "c3");

    /* Back to input: silent; back to output: announced again. */
    wr8001(&c, 4, 0x00);
    chan_expect_nothing(&pb);
    wr8001(&c, 4, 0x02);
    chan_expect(&pb, "c3");

    /* Input on port D reaches the bus while D is an input. */
    chan_send(&pd, "a5\n");
    qom_wait(qts, "pio", "inputs", 0xff000000, 0xa5000000);
    g_assert_cmphex(rd8001(&c, 3), ==, 0xa5);

    /* Inverted input on port B is held while B drives... */
    chan_send(&pb, "0f\n");
    qom_wait(qts, "pio", "inputs", 0x0000ff00, 0x0000f000);
    g_assert_cmphex(rd8001(&c, 1), ==, 0x3c);
    /* ...and appears once B is an input again. */
    wr8001(&c, 4, 0x00);
    g_assert_cmphex(rd8001(&c, 1), ==, 0xf0);

    card8001_free(&c);
    qtest_quit(qts);
    chan_close(&pb);
    chan_close(&pd);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_device("sealevel-3096")) {
        qtest_add_func("/sealevel/3096/regs", test_3096_regs);
        qtest_add_func("/sealevel/3096/options", test_3096_options);
        qtest_add_func("/sealevel/3096/irq", test_3096_irq);
        qtest_add_func("/sealevel/3096/chardev", test_3096_chardev);
    }
    if (qtest_has_device("sealevel-8001")) {
        qtest_add_func("/sealevel/8001/pci", test_8001_pci);
        qtest_add_func("/sealevel/8001/regs", test_8001_regs);
        qtest_add_func("/sealevel/8001/chardev", test_8001_chardev);
    }

    return g_test_run();
}
