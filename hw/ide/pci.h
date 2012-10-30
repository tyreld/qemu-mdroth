#ifndef HW_IDE_PCI_H
#define HW_IDE_PCI_H

#include <hw/ide/internal.h>
#include "qidl.h"

void ide_bmdma_pre_save(void *opaque);
int ide_pci_post_load(void *opaque, int version_id);

typedef struct BMDMAState BMDMAState;

QIDL_DECLARE_PUBLIC(BMDMAState) {
    IDEDMA q_immutable dma;
    uint8_t cmd;
    uint8_t status;
    uint32_t addr;

    IDEBus q_elsewhere *bus; /* via parent's corresponding bus */
    /* current transfer state */
    uint32_t cur_addr;
    uint32_t cur_prd_last;
    uint32_t cur_prd_addr;
    uint32_t cur_prd_len;
    uint8_t unit;
    BlockDriverCompletionFunc q_immutable *dma_cb;
    int64_t sector_num;
    uint32_t nsector;
    MemoryRegion addr_ioport;
    MemoryRegion extra_io;
    QEMUBH q_immutable *bh;
    qemu_irq irq;

    /* Bit 0-2 and 7:   BM status register
     * Bit 3-6:         bus->error_status */
    uint8_t migration_compat_status;
    struct PCIIDEState q_elsewhere *pci_dev; /* via parent */
};

typedef struct CMD646BAR {
    MemoryRegion cmd;
    MemoryRegion data;
    IDEBus *bus;
    struct PCIIDEState *pci_dev;
} CMD646BAR;

typedef struct PCIIDEState PCIIDEState;

QIDL_DECLARE_PUBLIC(PCIIDEState) {
    PCIDevice dev;
    IDEBus bus[2];
    BMDMAState bmdma[2];
    uint32_t q_immutable secondary; /* used only for cmd646 */
    MemoryRegion bmdma_bar;
    CMD646BAR q_immutable cmd646_bar[2]; /* used only for cmd646 */
};

static inline IDEState *bmdma_active_if(BMDMAState *bmdma)
{
    assert(bmdma->unit != (uint8_t)-1);
    return bmdma->bus->ifs + bmdma->unit;
}


void bmdma_init(IDEBus *bus, BMDMAState *bm, PCIIDEState *d);
void bmdma_cmd_writeb(BMDMAState *bm, uint32_t val);
extern MemoryRegionOps bmdma_addr_ioport_ops;
void pci_ide_create_devs(PCIDevice *dev, DriveInfo **hd_table);

extern const VMStateDescription vmstate_ide_pci;
#endif
