#include "drivers/pci.h"
#include "lib/io.h"
#include "lib/string.h"

static int s_mech;
static pci_dev_t s_devs[PCI_MAX_DEVICES];
static int s_count;

/* ── config space access ───────────────────────────────────────────────── */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    off &= 0xFC;
    if (s_mech == 1) {
        outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) |
                        ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | off);
        return inl(0xCFC);
    }
    if (s_mech == 2) {
        if (dev >= 16)
            return 0xFFFFFFFFu;
        outb(0xCF8, (uint8_t)(0xF0 | (fn << 1)));
        outb(0xCFA, bus);
        uint32_t v = inl((uint16_t)(0xC000 | (dev << 8) | off));
        outb(0xCF8, 0);
        return v;
    }
    return 0xFFFFFFFFu;
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                 uint32_t v) {
    off &= 0xFC;
    if (s_mech == 1) {
        outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) |
                        ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | off);
        outl(0xCFC, v);
    } else if (s_mech == 2 && dev < 16) {
        outb(0xCF8, (uint8_t)(0xF0 | (fn << 1)));
        outb(0xCFA, bus);
        outl((uint16_t)(0xC000 | (dev << 8) | off), v);
        outb(0xCF8, 0);
    }
}

/* ── detection (same checks as Linux's pci_direct_probe) ───────────────── */

/* Does bus 0 hold something that looks like a host bridge or VGA? Guards
 * against chipsets that echo the address register without being PCI. */
static bool sane_bus0(void) {
    int devs = s_mech == 2 ? 16 : 32;
    for (int d = 0; d < devs; d++) {
        uint32_t cls = pci_read32(0, (uint8_t)d, 0, 0x08) >> 16;
        if (cls == 0x0600 || cls == 0x0300) /* host bridge / VGA */
            return true;
        uint32_t id = pci_read32(0, (uint8_t)d, 0, 0x00);
        if ((id & 0xFFFF) == 0x8086 ||
            (id & 0xFFFF) == 0x0E11) /* Intel/Compaq */
            return true;
    }
    return false;
}

static int detect(void) {
    /* mechanism #1: the address register must keep what we write */
    outb(0xCFB, 0x01);
    uint32_t saved = inl(0xCF8);
    outl(0xCF8, 0x80000000u);
    bool m1 = inl(0xCF8) == 0x80000000u;
    outl(0xCF8, saved);
    if (m1) {
        s_mech = 1;
        if (sane_bus0())
            return 1;
    }
    /* mechanism #2: CSE and forward registers read back as written */
    outb(0xCFB, 0x00);
    outb(0xCF8, 0x00);
    outb(0xCFA, 0x00);
    if (inb(0xCF8) == 0x00 && inb(0xCFA) == 0x00) {
        s_mech = 2;
        if (sane_bus0())
            return 2;
    }
    s_mech = 0;
    return 0;
}

/* ── scan ──────────────────────────────────────────────────────────────── */

static void scan_bus(uint8_t bus, int depth);

static void add_function(uint8_t bus, uint8_t dev, uint8_t fn, int depth) {
    uint32_t id = pci_read32(bus, dev, fn, 0x00);
    if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0x0000)
        return;
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    uint8_t hdr = (uint8_t)(pci_read32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
    if (s_count < PCI_MAX_DEVICES) {
        pci_dev_t *d = &s_devs[s_count++];
        d->bus = bus;
        d->dev = dev;
        d->fn = fn;
        d->vendor = (uint16_t)id;
        d->device = (uint16_t)(id >> 16);
        d->revision = (uint8_t)cls;
        d->prog_if = (uint8_t)(cls >> 8);
        d->subclass = (uint8_t)(cls >> 16);
        d->class_code = (uint8_t)(cls >> 24);
        d->irq = (uint8_t)pci_read32(bus, dev, fn, 0x3C);
        if (d->irq == 0 || d->irq > 15)
            d->irq = 0xFF;
        int nbars = hdr == 0 ? 6 : hdr == 1 ? 2 : 0;
        for (int i = 0; i < 6; i++)
            d->bar[i] = i < nbars
                            ? pci_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4))
                            : 0;
    }
    /* PCI-to-PCI bridge: scan what is behind it */
    if (hdr == 1 && depth < 4) {
        uint8_t secondary = (uint8_t)(pci_read32(bus, dev, fn, 0x18) >> 8);
        if (secondary > bus)
            scan_bus(secondary, depth + 1);
    }
}

static void scan_bus(uint8_t bus, int depth) {
    int devs = s_mech == 2 ? 16 : 32;
    for (int dev = 0; dev < devs; dev++) {
        uint32_t id = pci_read32(bus, (uint8_t)dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0x0000)
            continue;
        add_function(bus, (uint8_t)dev, 0, depth);
        uint8_t hdr = (uint8_t)(pci_read32(bus, (uint8_t)dev, 0, 0x0C) >> 16);
        if (hdr & 0x80) /* multi-function device */
            for (int fn = 1; fn < 8; fn++)
                add_function(bus, (uint8_t)dev, (uint8_t)fn, depth);
    }
}

int pci_init(void) {
    s_count = 0;
    if (detect())
        scan_bus(0, 0);
    return s_count;
}

int pci_mechanism(void) { return s_mech; }
int pci_count(void) { return s_count; }
const pci_dev_t *pci_get(int i) {
    return (i >= 0 && i < s_count) ? &s_devs[i] : NULL;
}

const pci_dev_t *pci_find(uint16_t vendor, uint16_t device, int nth) {
    for (int i = 0; i < s_count; i++)
        if ((vendor == 0xFFFF || s_devs[i].vendor == vendor) &&
            (device == 0xFFFF || s_devs[i].device == device) && nth-- == 0)
            return &s_devs[i];
    return NULL;
}

const pci_dev_t *pci_find_class(uint8_t class_code, uint8_t subclass, int nth) {
    for (int i = 0; i < s_count; i++)
        if (s_devs[i].class_code == class_code &&
            (subclass == 0xFF || s_devs[i].subclass == subclass) && nth-- == 0)
            return &s_devs[i];
    return NULL;
}

uint16_t pci_bar_io(const pci_dev_t *d, int i) {
    if (!d || i < 0 || i > 5 || !(d->bar[i] & 1))
        return 0;
    return (uint16_t)(d->bar[i] & 0xFFFC);
}

void pci_enable(const pci_dev_t *d, bool io, bool mem, bool master) {
    if (!d)
        return;
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, 0x04);
    uint32_t cmd = v & 0xFFFF;
    if (io)
        cmd |= 1;
    if (mem)
        cmd |= 2;
    if (master)
        cmd |= 4;
    /* status (upper half) is write-1-to-clear: write zeros there */
    pci_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

const char *pci_class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x00:
        return s == 1 ? "VGA (old)" : "Unclassified";
    case 0x01:
        return s == 0x01   ? "IDE controller"
               : s == 0x06 ? "SATA controller"
               : s == 0x00 ? "SCSI controller"
                           : "Storage";
    case 0x02:
        return s == 0 ? "Ethernet" : "Network";
    case 0x03:
        return s == 0 ? "VGA" : "Display";
    case 0x04:
        return s == 1 ? "Audio" : "Multimedia";
    case 0x05:
        return "Memory";
    case 0x06:
        return s == 0x00   ? "Host bridge"
               : s == 0x01 ? "ISA bridge"
               : s == 0x04 ? "PCI bridge"
                           : "Bridge";
    case 0x07:
        return "Comms";
    case 0x08:
        return "System";
    case 0x0C:
        return s == 0x03 ? "USB" : s == 0x05 ? "SMBus" : "Serial bus";
    default:
        return "Other";
    }
}
