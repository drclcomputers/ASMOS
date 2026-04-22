#ifndef FDD_H
#define FDD_H

#include "lib/core.h"

/* Floppy controller I/O ports */
#define FDD_DOR 0x3F2  /* Digital Output Register        */
#define FDD_MSR 0x3F4  /* Main Status Register (read)    */
#define FDD_DATA 0x3F5 /* Data FIFO                      */
#define FDD_DIR 0x3F7  /* Digital Input Register (read)  */
#define FDD_CCR 0x3F7  /* Config Control Register (write)*/

/* DOR bits */
#define FDD_DOR_RESET 0x04
#define FDD_DOR_IRQ_DMA 0x08
#define FDD_DOR_MOTOR_A 0x10
#define FDD_DOR_MOTOR_B 0x20
#define FDD_DOR_SEL_A 0x00
#define FDD_DOR_SEL_B 0x01

/* MSR bits */
#define FDD_MSR_RQM 0x80 /* Ready for data transfer */
#define FDD_MSR_DIO 0x40 /* Direction: 1=controller->CPU */
#define FDD_MSR_BUSY 0x10

/* FDC commands */
#define FDD_CMD_READ 0xE6    /* Read sector (with MT/MFM/SK flags) */
#define FDD_CMD_WRITE 0xC5   /* Write sector */
#define FDD_CMD_RECAL 0x07   /* Recalibrate */
#define FDD_CMD_SENSE 0x08   /* Sense interrupt */
#define FDD_CMD_SEEK 0x0F    /* Seek */
#define FDD_CMD_SPECIFY 0x03 /* Specify drive timings */
#define FDD_CMD_VERSION 0x10 /* Get FDC version */

/* Standard 1.44 MB geometry */
#define FDD_HEADS 2
#define FDD_TRACKS 80
#define FDD_SECTORS 18
#define FDD_SECTOR_SIZE 512
#define FDD_SECTORS_TOTAL (FDD_HEADS * FDD_TRACKS * FDD_SECTORS)

#define DMA_FLIP_FLOP 0x0C
#define DMA2_ADDR 0x04
#define DMA2_COUNT 0x05
#define DMA2_PAGE 0x81
#define DMA_MASK 0x0A
#define DMA_MODE 0x0B
#define DMA_UNMASK 0x0A

bool fdd_init(uint8_t drive_id);
bool fdd_read_sector(uint8_t drive_id, uint32_t lba, void *buf);
bool fdd_write_sector(uint8_t drive_id, uint32_t lba, const void *buf);
bool fdd_is_present(uint8_t drive_id);
void fdd_motor_off(uint8_t drive_id);

#endif
