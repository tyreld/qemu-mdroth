#ifndef SHPC_H
#define SHPC_H

#include "qemu-common.h"
#include "memory.h"
#include "vmstate.h"
#include "qidl.h"

QIDL_DECLARE_PUBLIC(SHPCDevice) {
    /* Capability offset in device's config space */
    int cap;

    /* # of hot-pluggable slots */
    int nslots;

    /* size of space for SHPC working register set */
    size_t config_size;

    /* SHPC WRS: working register set */
    uint8_t *config q_size(config_size);

    /* Used to enable checks on load. Note that writable bits are
     * never checked even if set in cmask. */
    uint8_t q_immutable *cmask;

    /* Used to implement R/W bytes */
    uint8_t q_immutable *wmask;

    /* Used to implement RW1C(Write 1 to Clear) bytes */
    uint8_t q_immutable *w1cmask;

    /* MMIO for the SHPC BAR */
    MemoryRegion mmio;

    /* Bus controlled by this SHPC */
    PCIBus q_elsewhere *sec_bus;

    /* MSI already requested for this event */
    int msi_requested;
};

void shpc_reset(PCIDevice *d);
int shpc_bar_size(PCIDevice *dev);
int shpc_init(PCIDevice *dev, PCIBus *sec_bus, MemoryRegion *bar, unsigned off);
void shpc_cleanup(PCIDevice *dev, MemoryRegion *bar);
void shpc_cap_write_config(PCIDevice *d, uint32_t addr, uint32_t val, int len);
void shpc_post_load(PCIDevice *d);

extern VMStateInfo shpc_vmstate_info;
#define SHPC_VMSTATE(_field, _type) \
    VMSTATE_BUFFER_UNSAFE_INFO(_field, _type, 0, shpc_vmstate_info, 0)

#endif
