// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef PCI_H
#define PCI_H

/*
 * PCI bus scan. Safe on ISA-only machines (no PCI found -> pci_count() 0).
 * Supports configuration mechanism #1 (0xCF8/0xCFC, everything after ~1994)
 * and #2 (0xCF8/0xCFA, some early 486/Pentium PCI chipsets).
 */

#include "lib/core.h"

#define PCI_MAX_DEVICES 32

typedef struct {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t irq;     /* interrupt line set up by the BIOS, 0xFF = none */
    uint32_t bar[6]; /* raw BAR values */
} pci_dev_t;

/* Detect the config mechanism and scan all buses. Returns devices found. */
int pci_init(void);
int pci_mechanism(void); /* 0 = no PCI, 1 or 2 */
int pci_count(void);
const pci_dev_t *pci_get(int i);

/* nth (0-based) device matching; NULL if none. 0xFF / 0xFFFF = any. */
const pci_dev_t *pci_find(uint16_t vendor, uint16_t device, int nth);
const pci_dev_t *pci_find_class(uint8_t class_code, uint8_t subclass, int nth);

/* I/O port base of BAR i, or 0 if that BAR is not an I/O BAR. */
uint16_t pci_bar_io(const pci_dev_t *d, int i);
/* Set I/O space / memory space / bus master enable bits. */
void pci_enable(const pci_dev_t *d, bool io, bool mem, bool master);

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

const char *pci_class_name(uint8_t class_code, uint8_t subclass);

#endif
