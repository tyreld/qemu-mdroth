/*
 * QEMU sPAPR PCI host originated from Uninorth PCI host
 *
 * Copyright (c) 2011 Alexey Kardashevskiy, IBM Corporation.
 * Copyright (C) 2011 David Gibson, IBM Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci_host.h"
#include "hw/ppc/spapr.h"
#include "hw/pci-host/spapr.h"
#include "exec/address-spaces.h"
#include <libfdt.h>
#include "trace.h"

#include "hw/pci/pci_bus.h"

/* Copied from the kernel arch/powerpc/platforms/pseries/msi.c */
#define RTAS_QUERY_FN           0
#define RTAS_CHANGE_FN          1
#define RTAS_RESET_FN           2
#define RTAS_CHANGE_MSI_FN      3
#define RTAS_CHANGE_MSIX_FN     4

/* Interrupt types to return on RTAS_CHANGE_* */
#define RTAS_TYPE_MSI           1
#define RTAS_TYPE_MSIX          2

#define _FDT(exp) \
    do { \
        int ret = (exp);                                           \
        if (ret < 0) {                                             \
            g_warning("fdt error: %d", ret);                       \
            return ret;                                            \
        }                                                          \
    } while (0)

static sPAPRPHBState *find_phb(sPAPREnvironment *spapr, uint64_t buid)
{
    sPAPRPHBState *sphb;

    QLIST_FOREACH(sphb, &spapr->phbs, list) {
        if (sphb->buid != buid) {
            continue;
        }
        return sphb;
    }

    return NULL;
}

static PCIDevice *find_dev(sPAPREnvironment *spapr, uint64_t buid,
                           uint32_t config_addr)
{
    sPAPRPHBState *sphb = find_phb(spapr, buid);
    PCIHostState *phb = PCI_HOST_BRIDGE(sphb);
    int bus_num = (config_addr >> 16) & 0xFF;
    int devfn = (config_addr >> 8) & 0xFF;

    if (!phb) {
        return NULL;
    }

    return pci_find_device(phb->bus, bus_num, devfn);
}

static uint32_t rtas_pci_cfgaddr(uint32_t arg)
{
    /* This handles the encoding of extended config space addresses */
    return ((arg >> 20) & 0xf00) | (arg & 0xff);
}

static void finish_read_pci_config(sPAPREnvironment *spapr, uint64_t buid,
                                   uint32_t addr, uint32_t size,
                                   target_ulong rets)
{
    PCIDevice *pci_dev;
    uint32_t val;

    if ((size != 1) && (size != 2) && (size != 4)) {
        /* access must be 1, 2 or 4 bytes */
        rtas_st(rets, 0, -1);
        return;
    }

    pci_dev = find_dev(spapr, buid, addr);
    addr = rtas_pci_cfgaddr(addr);

    if (!pci_dev || (addr % size) || (addr >= pci_config_size(pci_dev))) {
        /* Access must be to a valid device, within bounds and
         * naturally aligned */
        rtas_st(rets, 0, -1);
        return;
    }

    val = pci_host_config_read_common(pci_dev, addr,
                                      pci_config_size(pci_dev), size);

    rtas_st(rets, 0, 0);
    rtas_st(rets, 1, val);
}

static void rtas_ibm_read_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                     uint32_t token, uint32_t nargs,
                                     target_ulong args,
                                     uint32_t nret, target_ulong rets)
{
    uint64_t buid;
    uint32_t size, addr;

    if ((nargs != 4) || (nret != 2)) {
        rtas_st(rets, 0, -1);
        return;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    size = rtas_ld(args, 3);
    addr = rtas_ld(args, 0);

    finish_read_pci_config(spapr, buid, addr, size, rets);
}

static void rtas_read_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                 uint32_t token, uint32_t nargs,
                                 target_ulong args,
                                 uint32_t nret, target_ulong rets)
{
    uint32_t size, addr;

    if ((nargs != 2) || (nret != 2)) {
        rtas_st(rets, 0, -1);
        return;
    }

    size = rtas_ld(args, 1);
    addr = rtas_ld(args, 0);

    finish_read_pci_config(spapr, 0, addr, size, rets);
}

static void finish_write_pci_config(sPAPREnvironment *spapr, uint64_t buid,
                                    uint32_t addr, uint32_t size,
                                    uint32_t val, target_ulong rets)
{
    PCIDevice *pci_dev;

    if ((size != 1) && (size != 2) && (size != 4)) {
        /* access must be 1, 2 or 4 bytes */
        rtas_st(rets, 0, -1);
        return;
    }

    pci_dev = find_dev(spapr, buid, addr);
    addr = rtas_pci_cfgaddr(addr);

    if (!pci_dev || (addr % size) || (addr >= pci_config_size(pci_dev))) {
        /* Access must be to a valid device, within bounds and
         * naturally aligned */
        rtas_st(rets, 0, -1);
        return;
    }

    pci_host_config_write_common(pci_dev, addr, pci_config_size(pci_dev),
                                 val, size);

    rtas_st(rets, 0, 0);
}

static void rtas_ibm_write_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                      uint32_t token, uint32_t nargs,
                                      target_ulong args,
                                      uint32_t nret, target_ulong rets)
{
    uint64_t buid;
    uint32_t val, size, addr;

    if ((nargs != 5) || (nret != 1)) {
        rtas_st(rets, 0, -1);
        return;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    val = rtas_ld(args, 4);
    size = rtas_ld(args, 3);
    addr = rtas_ld(args, 0);

    finish_write_pci_config(spapr, buid, addr, size, val, rets);
}

static void rtas_write_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                  uint32_t token, uint32_t nargs,
                                  target_ulong args,
                                  uint32_t nret, target_ulong rets)
{
    uint32_t val, size, addr;

    if ((nargs != 3) || (nret != 1)) {
        rtas_st(rets, 0, -1);
        return;
    }


    val = rtas_ld(args, 2);
    size = rtas_ld(args, 1);
    addr = rtas_ld(args, 0);

    finish_write_pci_config(spapr, 0, addr, size, val, rets);
}

/*
 * Find an entry with config_addr or returns the empty one if not found AND
 * alloc_new is set.
 * At the moment the msi_table entries are never released so there is
 * no point to look till the end of the list if we need to find the free entry.
 */
static int spapr_msicfg_find(sPAPRPHBState *phb, uint32_t config_addr,
                             bool alloc_new)
{
    int i;

    for (i = 0; i < SPAPR_MSIX_MAX_DEVS; ++i) {
        if (!phb->msi_table[i].nvec) {
            break;
        }
        if (phb->msi_table[i].config_addr == config_addr) {
            return i;
        }
    }
    if ((i < SPAPR_MSIX_MAX_DEVS) && alloc_new) {
        trace_spapr_pci_msi("Allocating new MSI config", i, config_addr);
        return i;
    }

    return -1;
}

/*
 * Set MSI/MSIX message data.
 * This is required for msi_notify()/msix_notify() which
 * will write at the addresses via spapr_msi_write().
 */
static void spapr_msi_setmsg(PCIDevice *pdev, hwaddr addr, bool msix,
                             unsigned first_irq, unsigned req_num)
{
    unsigned i;
    MSIMessage msg = { .address = addr, .data = first_irq };

    if (!msix) {
        msi_set_message(pdev, msg);
        trace_spapr_pci_msi_setup(pdev->name, 0, msg.address);
        return;
    }

    for (i = 0; i < req_num; ++i, ++msg.data) {
        msix_set_message(pdev, i, msg);
        trace_spapr_pci_msi_setup(pdev->name, i, msg.address);
    }
}

static void rtas_ibm_change_msi(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                uint32_t token, uint32_t nargs,
                                target_ulong args, uint32_t nret,
                                target_ulong rets)
{
    uint32_t config_addr = rtas_ld(args, 0);
    uint64_t buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    unsigned int func = rtas_ld(args, 3);
    unsigned int req_num = rtas_ld(args, 4); /* 0 == remove all */
    unsigned int seq_num = rtas_ld(args, 5);
    unsigned int ret_intr_type;
    int ndev, irq;
    sPAPRPHBState *phb = NULL;
    PCIDevice *pdev = NULL;

    switch (func) {
    case RTAS_CHANGE_MSI_FN:
    case RTAS_CHANGE_FN:
        ret_intr_type = RTAS_TYPE_MSI;
        break;
    case RTAS_CHANGE_MSIX_FN:
        ret_intr_type = RTAS_TYPE_MSIX;
        break;
    default:
        fprintf(stderr, "rtas_ibm_change_msi(%u) is not implemented\n", func);
        rtas_st(rets, 0, -3); /* Parameter error */
        return;
    }

    /* Fins sPAPRPHBState */
    phb = find_phb(spapr, buid);
    if (phb) {
        pdev = find_dev(spapr, buid, config_addr);
    }
    if (!phb || !pdev) {
        rtas_st(rets, 0, -3); /* Parameter error */
        return;
    }

    /* Releasing MSIs */
    if (!req_num) {
        ndev = spapr_msicfg_find(phb, config_addr, false);
        if (ndev < 0) {
            trace_spapr_pci_msi("MSI has not been enabled", -1, config_addr);
            rtas_st(rets, 0, -1); /* Hardware error */
            return;
        }
        trace_spapr_pci_msi("Released MSIs", ndev, config_addr);
        rtas_st(rets, 0, 0);
        rtas_st(rets, 1, 0);
        return;
    }

    /* Enabling MSI */

    /* Find a device number in the map to add or reuse the existing one */
    ndev = spapr_msicfg_find(phb, config_addr, true);
    if (ndev >= SPAPR_MSIX_MAX_DEVS || ndev < 0) {
        fprintf(stderr, "No free entry for a new MSI device\n");
        rtas_st(rets, 0, -1); /* Hardware error */
        return;
    }
    trace_spapr_pci_msi("Configuring MSI", ndev, config_addr);

    /* Check if there is an old config and MSI number has not changed */
    if (phb->msi_table[ndev].nvec && (req_num != phb->msi_table[ndev].nvec)) {
        /* Unexpected behaviour */
        fprintf(stderr, "Cannot reuse MSI config for device#%d", ndev);
        rtas_st(rets, 0, -1); /* Hardware error */
        return;
    }

    /* There is no cached config, allocate MSIs */
    if (!phb->msi_table[ndev].nvec) {
        irq = spapr_allocate_irq_block(req_num, false,
                                       ret_intr_type == RTAS_TYPE_MSI);
        if (irq < 0) {
            fprintf(stderr, "Cannot allocate MSIs for device#%d", ndev);
            rtas_st(rets, 0, -1); /* Hardware error */
            return;
        }
        phb->msi_table[ndev].irq = irq;
        phb->msi_table[ndev].nvec = req_num;
        phb->msi_table[ndev].config_addr = config_addr;
    }

    /* Setup MSI/MSIX vectors in the device (via cfgspace or MSIX BAR) */
    spapr_msi_setmsg(pdev, spapr->msi_win_addr, ret_intr_type == RTAS_TYPE_MSIX,
                     phb->msi_table[ndev].irq, req_num);

    rtas_st(rets, 0, 0);
    rtas_st(rets, 1, req_num);
    rtas_st(rets, 2, ++seq_num);
    rtas_st(rets, 3, ret_intr_type);

    trace_spapr_pci_rtas_ibm_change_msi(func, req_num);
}

static void rtas_ibm_query_interrupt_source_number(PowerPCCPU *cpu,
                                                   sPAPREnvironment *spapr,
                                                   uint32_t token,
                                                   uint32_t nargs,
                                                   target_ulong args,
                                                   uint32_t nret,
                                                   target_ulong rets)
{
    uint32_t config_addr = rtas_ld(args, 0);
    uint64_t buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    unsigned int intr_src_num = -1, ioa_intr_num = rtas_ld(args, 3);
    int ndev;
    sPAPRPHBState *phb = NULL;

    /* Fins sPAPRPHBState */
    phb = find_phb(spapr, buid);
    if (!phb) {
        rtas_st(rets, 0, -3); /* Parameter error */
        return;
    }

    /* Find device descriptor and start IRQ */
    ndev = spapr_msicfg_find(phb, config_addr, false);
    if (ndev < 0) {
        trace_spapr_pci_msi("MSI has not been enabled", -1, config_addr);
        rtas_st(rets, 0, -1); /* Hardware error */
        return;
    }

    intr_src_num = phb->msi_table[ndev].irq + ioa_intr_num;
    trace_spapr_pci_rtas_ibm_query_interrupt_source_number(ioa_intr_num,
                                                           intr_src_num);

    rtas_st(rets, 0, 0);
    rtas_st(rets, 1, intr_src_num);
    rtas_st(rets, 2, 1);/* 0 == level; 1 == edge */
}

static void rtas_set_indicator(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                               uint32_t token, uint32_t nargs,
                               target_ulong args, uint32_t nret,
                               target_ulong rets)
{
    uint32_t indicator = rtas_ld(args, 0);
    uint32_t drc_index = rtas_ld(args, 1);
    uint32_t indicator_state = rtas_ld(args, 2);
    DrcEntry *drc_entry = NULL;
    int i;

    switch (indicator) {
    case 9001: /* Isolation state */
        for (i = 0; i < SPAPR_DRC_TABLE_SIZE; i++) {
            if (drc_table[i].drc_index == drc_index) {
                drc_entry = &drc_table[i];
                break;
            }
        }

        if (drc_entry) {
            drc_entry->state = indicator_state;
        }
        break;

    case 9003: /* Allocation State */
        break;
    }

    rtas_st(rets, 0, 0);
}

static void rtas_set_power_level(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                 uint32_t token, uint32_t nargs,
                                 target_ulong args, uint32_t nret,
                                 target_ulong rets)
{
    uint32_t power_lvl = rtas_ld(args, 0);

    rtas_st(rets, 0, 0);
    rtas_st(rets, 1, power_lvl);
}

static void rtas_get_sensor_state(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                  uint32_t token, uint32_t nargs,
                                  target_ulong args, uint32_t nret,
                                  target_ulong rets)
{
    uint32_t sensor = rtas_ld(args, 0);
    uint32_t drc_index = rtas_ld(args, 0);
    uint32_t sensor_state = 0;
    DrcEntry *drc_entry = NULL;
    int i;

    switch (sensor) {
    case 9003: /* DR-Entity-Sense */
        for (i = 0; i < SPAPR_DRC_TABLE_SIZE; i++) {
            if (drc_table[i].drc_index == drc_index) {
                drc_entry = &drc_table[i];
                break;
            }
        }

        if (drc_entry) {
            sensor_state = drc_entry->state;
        }

        break;
    }

    rtas_st(rets, 0, 0);
    /* TODO: force this so drmgr doesn't complain, fix this properly soon */
    sensor_state = 2;
    rtas_st(rets, 1, sensor_state);
}

/* XXX: temporary code for debugging */
static void print_fdt_prop(void *fdt, int offset)
{
    const struct fdt_property *prop;
    const char *prop_name;
    int prop_len;

    prop = fdt_get_property_by_offset(fdt, offset, &prop_len);
    prop_name = fdt_string(fdt, fdt32_to_cpu(prop->nameoff));

    switch (prop_len) {
    case 1:
        g_print("prop name: %s, len: %d, value: %xh\n",
                prop_name, prop_len, ((uint8_t *)prop->data)[0]);
        break;
    case 2:
        g_print("prop name: %s, len: %d, value: %xh\n",
                prop_name, prop_len, ((uint16_t *)prop->data)[0]);
        break;
    case 4:
        g_print("prop name: %s, len: %d, value: %xh\n",
                prop_name, prop_len, ((uint32_t *)prop->data)[0]);
        break;
    case 8:
        g_print("prop name: %s, len: %d, value: %lxh\n",
                prop_name, prop_len, ((uint64_t *)prop->data)[0]);
        break;
    case 0:
        g_print("prop name: %s, len: %d, value: <none>\n",
                prop_name, prop_len);
        break;
    default:
        g_print("prop name: %s, len: %d, value: <buffer>\n",
                prop_name, prop_len);
        break;
    }
}

/* XXX: temporary code for debugging */
static void print_fdt(void *fdt, int offset, int depth)
{
    int next_offset = offset;
    int tag;

    do {
        int offset = next_offset;
        const char *nodename;
        int nodename_len;

        tag = fdt_next_tag(fdt, offset, &next_offset);
        switch (tag) {
        case FDT_BEGIN_NODE:
            depth++;
            nodename = fdt_get_name(fdt, offset, &nodename_len);
            g_print("BEGIN NODE ('%s', depth: %d)\n", nodename, depth);
            break;
        case FDT_END_NODE:
            g_print("END NODE (depth: %d)\n", depth);
            depth--;
            break;
        case FDT_PROP:
            print_fdt_prop(fdt, offset);
        default:
            /* skip */
            break;
        }
    } while (tag != FDT_END);
}

/* configure connector work area offsets, int32_t units */
#define CC_IDX_NODE_NAME_OFFSET 2
#define CC_IDX_PROP_NAME_OFFSET 2
#define CC_IDX_PROP_LEN 3
#define CC_IDX_PROP_DATA_OFFSET 4

#define CC_VAL_DATA_OFFSET ((CC_IDX_PROP_DATA_OFFSET + 1) * 4)
#define CC_RET_NEXT_SIB 1
#define CC_RET_NEXT_CHILD 2
#define CC_RET_NEXT_PROPERTY 3
#define CC_RET_PREV_PARENT 4
#define CC_RET_ERROR -1
#define CC_RET_SUCCESS 0

static void rtas_ibm_configure_connector(PowerPCCPU *cpu,
                                         sPAPREnvironment *spapr,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args, uint32_t nret,
                                         target_ulong rets)
{
    uint64_t wa_addr = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 0);
    DrcEntry *drc_entry = NULL;
    ConfigureConnectorState *ccs;
    void *wa_buf;
    int32_t *wa_buf_int;
    hwaddr map_len = 0x1024;
    uint32_t drc_index;
    int rc = 0, next_offset, tag, prop_len, node_name_len;
    const struct fdt_property *prop;
    const char *node_name, *prop_name;

    wa_buf = cpu_physical_memory_map(wa_addr, &map_len, 1);
    if (!wa_buf) {
        rc = CC_RET_ERROR;
        goto error_exit;
    }
    wa_buf_int = wa_buf;

    /* TODO: this will get called initially for PHB, then for each HP device
     * we'll get a call with the device drc_index, which we'll then need to
     * use to index into the device's DT. so do we skip the device DT node
     * properties until that 2nd phase?
     */
    drc_index = *(uint32_t *)wa_buf;
    drc_entry = spapr_find_drc_entry(drc_index);
    if (!drc_entry) {
        rc = -1;
        goto error_exit;
    }

    ccs = &drc_entry->cc_state;
    g_warning("ccs->state: %d", ccs->state);
    if (ccs->state == CC_STATE_PENDING) {
        /* fdt should've been been attached to drc_entry during realize/hotplug */
        g_assert(ccs->fdt);
        ccs->offset = 0;
        ccs->depth = 0;
        ccs->state = CC_STATE_ACTIVE;
    }

retry:
    tag = fdt_next_tag(ccs->fdt, ccs->offset, &next_offset);
    g_warning("tag: %d", tag);

    switch (tag) {
    case FDT_BEGIN_NODE:
        ccs->depth++;
        node_name = fdt_get_name(ccs->fdt, ccs->offset, &node_name_len);
        g_warning("node_name_len: %d", node_name_len);
        g_warning("node_name: %s", node_name);
        g_warning("node depth: %d", ccs->depth);
        wa_buf_int[CC_IDX_NODE_NAME_OFFSET] = CC_VAL_DATA_OFFSET;
        strcpy(wa_buf + wa_buf_int[CC_IDX_NODE_NAME_OFFSET], node_name);
        rc = CC_RET_NEXT_CHILD;
        break;
    case FDT_END_NODE:
        ccs->depth--;
        if (ccs->depth == 0) {
            /* reached the end of top-level node, declare success */
            ccs->state = CC_STATE_PENDING;
            rc = CC_RET_SUCCESS;
        } else {
            rc = CC_RET_PREV_PARENT;
        }
        break;
    case FDT_PROP:
        prop = fdt_get_property_by_offset(ccs->fdt, ccs->offset, &prop_len);
        prop_name = fdt_string(ccs->fdt, fdt32_to_cpu(prop->nameoff));
        g_warning("prop_name: %s, prop_len: %d", prop_name, prop_len);
        wa_buf_int[CC_IDX_PROP_NAME_OFFSET] = CC_VAL_DATA_OFFSET;
        wa_buf_int[CC_IDX_PROP_LEN] = prop_len;
        wa_buf_int[CC_IDX_PROP_DATA_OFFSET] =
            CC_VAL_DATA_OFFSET + strlen(prop_name) + 1;

        strcpy(wa_buf + wa_buf_int[CC_IDX_PROP_NAME_OFFSET], prop_name);
        memcpy(wa_buf + wa_buf_int[CC_IDX_PROP_DATA_OFFSET],
               prop->data, prop_len);
        rc = CC_RET_NEXT_PROPERTY;
        break;
    case FDT_END:
        rc = CC_RET_ERROR;
        break;
    default:
        ccs->offset = next_offset;
        goto retry;
    }

    ccs->offset = next_offset;

error_exit:
    cpu_physical_memory_unmap(wa_buf, 0x1024, 1, 0x1024);
    rtas_st(rets, 0, rc);
}

static int pci_spapr_swizzle(int slot, int pin)
{
    return (slot + pin) % PCI_NUM_PINS;
}

static int pci_spapr_map_irq(PCIDevice *pci_dev, int irq_num)
{
    /*
     * Here we need to convert pci_dev + irq_num to some unique value
     * which is less than number of IRQs on the specific bus (4).  We
     * use standard PCI swizzling, that is (slot number + pin number)
     * % 4.
     */
    return pci_spapr_swizzle(PCI_SLOT(pci_dev->devfn), irq_num);
}

static void pci_spapr_set_irq(void *opaque, int irq_num, int level)
{
    /*
     * Here we use the number returned by pci_spapr_map_irq to find a
     * corresponding qemu_irq.
     */
    sPAPRPHBState *phb = opaque;

    trace_spapr_pci_lsi_set(phb->dtbusname, irq_num, phb->lsi_table[irq_num].irq);
    qemu_set_irq(spapr_phb_lsi_qirq(phb, irq_num), level);
}

/*
 * MSI/MSIX memory region implementation.
 * The handler handles both MSI and MSIX.
 * For MSI-X, the vector number is encoded as a part of the address,
 * data is set to 0.
 * For MSI, the vector number is encoded in least bits in data.
 */
static void spapr_msi_write(void *opaque, hwaddr addr,
                            uint64_t data, unsigned size)
{
    uint32_t irq = data;

    trace_spapr_pci_msi_write(addr, data, irq);

    qemu_irq_pulse(xics_get_qirq(spapr->icp, irq));
}

static const MemoryRegionOps spapr_msi_ops = {
    /* There is no .read as the read result is undefined by PCI spec */
    .read = NULL,
    .write = spapr_msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN
};

void spapr_pci_msi_init(sPAPREnvironment *spapr, hwaddr addr)
{
    /*
     * As MSI/MSIX interrupts trigger by writing at MSI/MSIX vectors,
     * we need to allocate some memory to catch those writes coming
     * from msi_notify()/msix_notify().
     * As MSIMessage:addr is going to be the same and MSIMessage:data
     * is going to be a VIRQ number, 4 bytes of the MSI MR will only
     * be used.
     */
    spapr->msi_win_addr = addr;
    memory_region_init_io(&spapr->msiwindow, NULL, &spapr_msi_ops, spapr,
                          "msi", getpagesize());
    memory_region_add_subregion(get_system_memory(), spapr->msi_win_addr,
                                &spapr->msiwindow);
}

/*
 * PHB PCI device
 */

static int spapr_map_BARs(sPAPRPHBState *phb, PCIDevice *dev)
{
    /* Assumptions:
     * each region that has been initialized will be set to:
     * r->addr = PCI_BAR_UNMAPPED or a valid address
     * r->size = BAR size, 0 means this is not a registered BAR
     * r->type = BAR type (i/o or mem)
     * r->memory = memory region
     *
     * the dev's BAR in config space will have the one's complement
     * of the BAR size masked to the appropriate size.
     * to get the BAR size, read the BAR from config space,
     * invert (~) and add 1 while masking the information bits
     *
     * NB: using pci_bar_address() via pci_uddate_mappings()
     * to get the bar address and size
     */

    PCIIORegion *r;
    int i, ret = -1;

    /* force the address space for registered memory regions
     * to be the PHB - this is different from the generic
     * pci behavior which uses default guest memory regions
     * as the containers.
     */

    for (i = 0; i < PCI_NUM_REGIONS; i++) {
        r = &dev->io_regions[i];
        /* this region isn't registered */
        if (!r->size) {
            continue;
        }

        /* we need to map at least 1 BAR */
        ret = 0;
        if (r->type & PCI_BASE_ADDRESS_SPACE_IO) {
            r->address_space = &phb->iospace;
        } else {
            r->address_space = &phb->memspace;
        }
        /* guarantee a limit into the BAR MemoryRegion */
        r->memory->size = int128_make64(r->size);
    }
    /* map the BAR range as a subregion of the PHB range
     * this call checks for conflicting subregions
     * and warns if any are encountered.
     */
    pci_update_mappings(dev);
    return ret;
}

static int spapr_phb_add_pci_dt(DeviceState *qdev, PCIDevice *dev)
{
    sPAPRPHBState *phb = SPAPR_PCI_HOST_BRIDGE(qdev);
    DrcEntry *drc_entry, *drc_entry_slot;
    ConfigureConnectorState *ccs;
    int slot = PCI_SLOT(dev->devfn);
    void *fdt;
    char nodename[512];
    int offset;

    /* TODO: for now we assume a DT node was created for this PHB as part
     * of machine realization. when we add support for hotplugging PHBs,
     * we'll need to create the PHB DT node here and skip PCI device bits.
     * How we map the PCIDevice to creating a new PCIHostState is left as
     * an exercise for the reader... do we need a bus_add hotplug
     * interface? Alternative we can create a new PHB for each device,
     * but that may have other limitations/issues.
     */
    drc_entry = spapr_phb_to_drc_entry(phb->buid);
    g_assert(drc_entry);
    drc_entry_slot = &drc_entry->child_entries[slot];

    g_warning("drc_entry_slot index = %d", drc_entry_slot->drc_index);


    /* map memory region for device BARs */
    if (-1 == spapr_map_BARs(phb, dev)) {
        return -1;
    }

    /* add OF node for pci device and required OF DT properties */
    fdt = g_malloc0(FDT_MAX_SIZE);
    offset = fdt_create(fdt, FDT_MAX_SIZE);
    sprintf(nodename, "pci@%" PRIx64, (long unsigned)1024);
    offset = fdt_begin_node(fdt, nodename);
    /* TODO: check endianess */
    _FDT(fdt_property_cell(fdt, "vendor-id",
                           pci_default_read_config(dev, PCI_VENDOR_ID, 2)));
    _FDT(fdt_property_cell(fdt, "device-id",
                          pci_default_read_config(dev, PCI_DEVICE_ID, 2)));
    _FDT(fdt_property_cell(fdt, "revision-id",
                          pci_default_read_config(dev, PCI_REVISION_ID, 1)));
    _FDT(fdt_property_cell(fdt, "class-code",
                          pci_default_read_config(dev, PCI_CLASS_DEVICE, 2)));

    /* NB: interrupts may not be returned for all devices - ? */
    _FDT(fdt_property_cell(fdt, "interrupts",
                          pci_default_read_config(dev, PCI_CLASS_DEVICE, 2)));

    /* if this device is NOT a bridge */
    if (PCI_HEADER_TYPE_NORMAL ==
        pci_default_read_config(dev, PCI_HEADER_TYPE, 1)) {

        _FDT(fdt_property_cell(fdt, "min-grant",
                      pci_default_read_config(dev, PCI_MIN_GNT, 1)));
        _FDT(fdt_property_cell(fdt, "max-latency",
                      pci_default_read_config(dev, PCI_MAX_LAT, 1)));
        _FDT(fdt_property_cell(fdt, "subsystem-id",
                      pci_default_read_config(dev, PCI_SUBSYSTEM_ID, 2)));
        _FDT(fdt_property_cell(fdt, "subsystem-vendor-id",
                      pci_default_read_config(dev, PCI_SUBSYSTEM_VENDOR_ID, 2)));
    }

    _FDT(fdt_property_cell(fdt, "cache-line-size",
                          pci_default_read_config(dev, PCI_CACHE_LINE_SIZE, 1)));

    /* the following fdt cells are masked off the pci status register */
    int pci_status = pci_default_read_config(dev, PCI_STATUS, 2);
    _FDT(fdt_property_cell(fdt, "devsel-speed",
                          PCI_STATUS_DEVSEL_MASK & pci_status));
    _FDT(fdt_property_cell(fdt, "fast-back-to-back",
                          PCI_STATUS_FAST_BACK & pci_status));
    _FDT(fdt_property_cell(fdt, "66mhz-capable",
                          PCI_STATUS_66MHZ & pci_status));
    _FDT(fdt_property_cell(fdt, "66mhz-capable",
                          PCI_STATUS_UDF & pci_status));

    /* end of pci status register fdt cells */

    _FDT(fdt_property(fdt, "ibm,my-drc-index",
                      &drc_entry_slot->drc_index,
                      sizeof(drc_entry_slot->drc_index)));

    char dev_fw_name_buf[32];
    sprintf(dev_fw_name_buf, "unknown\n");
    char *namep = pci_dev_fw_name(&dev->qdev, dev_fw_name_buf, 31);
    _FDT(fdt_property_string(fdt, "name", namep));

    fdt_end_node(fdt);
    fdt_finish(fdt);

    /* hold on to the node, configure_connector will pass it to the guest
     * later
     */
    ccs = &drc_entry_slot->cc_state;
    ccs->fdt = fdt;
    ccs->offset = offset;
    ccs->state = CC_STATE_PENDING;

    g_warning("NEW FDT");
    print_fdt(fdt, offset, -1);

    return 0;
}

static void spapr_phb_remove_pci_dt(DeviceState *qdev, PCIDevice *dev)
{
    /* TODO */
}

static int spapr_device_hotplug(DeviceState *qdev, PCIDevice *dev,
                                PCIHotplugState state)
{
    int slot = PCI_SLOT(dev->devfn);

    if (state == PCI_COLDPLUG_ENABLED) {
        /* Called during machine creation */
        return 0;
    }

    if (state == PCI_HOTPLUG_ENABLED) {
        fprintf(stderr, "Hot add of device on slot %d\n", slot);

        spapr_phb_add_pci_dt(qdev, dev);
        spapr_pci_hotplug_add(qdev, slot);
    } else {
        fprintf(stderr, "Hot remove of device on slot %d\n", slot);

        spapr_phb_remove_pci_dt(qdev, dev);
        spapr_pci_hotplug_remove(qdev, slot);
    }

    return 0;
}

static AddressSpace *spapr_pci_dma_iommu(PCIBus *bus, void *opaque, int devfn)
{
    sPAPRPHBState *phb = opaque;

    return &phb->iommu_as;
}

static int spapr_phb_init(SysBusDevice *s)
{
    DeviceState *dev = DEVICE(s);
    sPAPRPHBState *sphb = SPAPR_PCI_HOST_BRIDGE(s);
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    const char *busname;
    char *namebuf;
    int i;
    PCIBus *bus;

    if (sphb->index != -1) {
        hwaddr windows_base;

        if ((sphb->buid != -1) || (sphb->dma_liobn != -1)
            || (sphb->mem_win_addr != -1)
            || (sphb->io_win_addr != -1)) {
            fprintf(stderr, "Either \"index\" or other parameters must"
                    " be specified for PAPR PHB, not both\n");
            return -1;
        }

        sphb->buid = SPAPR_PCI_BASE_BUID + sphb->index;
        sphb->dma_liobn = SPAPR_PCI_BASE_LIOBN + sphb->index;

        windows_base = SPAPR_PCI_WINDOW_BASE
            + sphb->index * SPAPR_PCI_WINDOW_SPACING;
        sphb->mem_win_addr = windows_base + SPAPR_PCI_MMIO_WIN_OFF;
        sphb->io_win_addr = windows_base + SPAPR_PCI_IO_WIN_OFF;
    }

    if (sphb->buid == -1) {
        fprintf(stderr, "BUID not specified for PHB\n");
        return -1;
    }

    if (sphb->dma_liobn == -1) {
        fprintf(stderr, "LIOBN not specified for PHB\n");
        return -1;
    }

    if (sphb->mem_win_addr == -1) {
        fprintf(stderr, "Memory window address not specified for PHB\n");
        return -1;
    }

    if (sphb->io_win_addr == -1) {
        fprintf(stderr, "IO window address not specified for PHB\n");
        return -1;
    }

    if (find_phb(spapr, sphb->buid)) {
        fprintf(stderr, "PCI host bridges must have unique BUIDs\n");
        return -1;
    }

    sphb->dtbusname = g_strdup_printf("pci@%" PRIx64, sphb->buid);

    namebuf = alloca(strlen(sphb->dtbusname) + 32);

    /* Initialize memory regions */
    sprintf(namebuf, "%s.mmio", sphb->dtbusname);
    memory_region_init(&sphb->memspace, OBJECT(sphb), namebuf, INT64_MAX);

    sprintf(namebuf, "%s.mmio-alias", sphb->dtbusname);
    memory_region_init_alias(&sphb->memwindow, OBJECT(sphb),
                             namebuf, &sphb->memspace,
                             SPAPR_PCI_MEM_WIN_BUS_OFFSET, sphb->mem_win_size);
    memory_region_add_subregion(get_system_memory(), sphb->mem_win_addr,
                                &sphb->memwindow);

    /* On ppc, we only have MMIO no specific IO space from the CPU
     * perspective.  In theory we ought to be able to embed the PCI IO
     * memory region direction in the system memory space.  However,
     * if any of the IO BAR subregions use the old_portio mechanism,
     * that won't be processed properly unless accessed from the
     * system io address space.  This hack to bounce things via
     * system_io works around the problem until all the users of
     * old_portion are updated */
    sprintf(namebuf, "%s.io", sphb->dtbusname);
    memory_region_init(&sphb->iospace, OBJECT(sphb),
                       namebuf, SPAPR_PCI_IO_WIN_SIZE);
    /* FIXME: fix to support multiple PHBs */
    memory_region_add_subregion(get_system_io(), 0, &sphb->iospace);

    sprintf(namebuf, "%s.io-alias", sphb->dtbusname);
    memory_region_init_alias(&sphb->iowindow, OBJECT(sphb), namebuf,
                             get_system_io(), 0, SPAPR_PCI_IO_WIN_SIZE);
    memory_region_add_subregion(get_system_memory(), sphb->io_win_addr,
                                &sphb->iowindow);
    /*
     * Selecting a busname is more complex than you'd think, due to
     * interacting constraints.  If the user has specified an id
     * explicitly for the phb , then we want to use the qdev default
     * of naming the bus based on the bridge device (so the user can
     * then assign devices to it in the way they expect).  For the
     * first / default PCI bus (index=0) we want to use just "pci"
     * because libvirt expects there to be a bus called, simply,
     * "pci".  Otherwise, we use the same name as in the device tree,
     * since it's unique by construction, and makes the guest visible
     * BUID clear.
     */
    if (dev->id) {
        busname = NULL;
    } else if (sphb->index == 0) {
        busname = "pci";
    } else {
        busname = sphb->dtbusname;
    }
    bus = pci_register_bus(dev, busname,
                           pci_spapr_set_irq, pci_spapr_map_irq, sphb,
                           &sphb->memspace, &sphb->iospace,
                           PCI_DEVFN(0, 0), PCI_NUM_PINS, TYPE_PCI_BUS);
    phb->bus = bus;

    sphb->dma_window_start = 0;
    sphb->dma_window_size = 0x40000000;
    sphb->tcet = spapr_tce_new_table(dev, sphb->dma_liobn,
                                     sphb->dma_window_size);
    if (!sphb->tcet) {
        fprintf(stderr, "Unable to create TCE table for %s\n", sphb->dtbusname);
        return -1;
    }
    address_space_init(&sphb->iommu_as, spapr_tce_get_iommu(sphb->tcet),
                       sphb->dtbusname);

    pci_setup_iommu(bus, spapr_pci_dma_iommu, sphb);

    QLIST_INSERT_HEAD(&spapr->phbs, sphb, list);

    /* Initialize the LSI table */
    for (i = 0; i < PCI_NUM_PINS; i++) {
        uint32_t irq;

        irq = spapr_allocate_lsi(0);
        if (!irq) {
            return -1;
        }

        sphb->lsi_table[i].irq = irq;
    }

    /* Setup hotplug */
    pci_bus_hotplug(bus, spapr_device_hotplug, DEVICE(sphb));

    return 0;
}

static void spapr_phb_reset(DeviceState *qdev)
{
    SysBusDevice *s = SYS_BUS_DEVICE(qdev);
    sPAPRPHBState *sphb = SPAPR_PCI_HOST_BRIDGE(s);

    /* Reset the IOMMU state */
    device_reset(DEVICE(sphb->tcet));
}

static Property spapr_phb_properties[] = {
    DEFINE_PROP_INT32("index", sPAPRPHBState, index, -1),
    DEFINE_PROP_HEX64("buid", sPAPRPHBState, buid, -1),
    DEFINE_PROP_HEX32("liobn", sPAPRPHBState, dma_liobn, -1),
    DEFINE_PROP_HEX64("mem_win_addr", sPAPRPHBState, mem_win_addr, -1),
    DEFINE_PROP_HEX64("mem_win_size", sPAPRPHBState, mem_win_size,
                      SPAPR_PCI_MMIO_WIN_SIZE),
    DEFINE_PROP_HEX64("io_win_addr", sPAPRPHBState, io_win_addr, -1),
    DEFINE_PROP_HEX64("io_win_size", sPAPRPHBState, io_win_size,
                      SPAPR_PCI_IO_WIN_SIZE),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_spapr_pci_lsi = {
    .name = "spapr_pci/lsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32_EQUAL(irq, struct spapr_pci_lsi),

        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_pci_msi = {
    .name = "spapr_pci/lsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32(config_addr, struct spapr_pci_msi),
        VMSTATE_UINT32(irq, struct spapr_pci_msi),
        VMSTATE_UINT32(nvec, struct spapr_pci_msi),

        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_pci = {
    .name = "spapr_pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64_EQUAL(buid, sPAPRPHBState),
        VMSTATE_UINT32_EQUAL(dma_liobn, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(mem_win_addr, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(mem_win_size, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(io_win_addr, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(io_win_size, sPAPRPHBState),
        VMSTATE_STRUCT_ARRAY(lsi_table, sPAPRPHBState, PCI_NUM_PINS, 0,
                             vmstate_spapr_pci_lsi, struct spapr_pci_lsi),
        VMSTATE_STRUCT_ARRAY(msi_table, sPAPRPHBState, SPAPR_MSIX_MAX_DEVS, 0,
                             vmstate_spapr_pci_msi, struct spapr_pci_msi),

        VMSTATE_END_OF_LIST()
    },
};

static const char *spapr_phb_root_bus_path(PCIHostState *host_bridge,
                                           PCIBus *rootbus)
{
    sPAPRPHBState *sphb = SPAPR_PCI_HOST_BRIDGE(host_bridge);

    return sphb->dtbusname;
}

static void spapr_phb_class_init(ObjectClass *klass, void *data)
{
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    hc->root_bus_path = spapr_phb_root_bus_path;
    sdc->init = spapr_phb_init;
    dc->props = spapr_phb_properties;
    dc->reset = spapr_phb_reset;
    dc->vmsd = &vmstate_spapr_pci;
}

static const TypeInfo spapr_phb_info = {
    .name          = TYPE_SPAPR_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(sPAPRPHBState),
    .class_init    = spapr_phb_class_init,
};

PCIHostState *spapr_create_phb(sPAPREnvironment *spapr, int index)
{
    DeviceState *dev;

    dev = qdev_create(NULL, TYPE_SPAPR_PCI_HOST_BRIDGE);
    qdev_prop_set_uint32(dev, "index", index);
    qdev_init_nofail(dev);

    return PCI_HOST_BRIDGE(dev);
}

/* Macros to operate with address in OF binding to PCI */
#define b_x(x, p, l)    (((x) & ((1<<(l))-1)) << (p))
#define b_n(x)          b_x((x), 31, 1) /* 0 if relocatable */
#define b_p(x)          b_x((x), 30, 1) /* 1 if prefetchable */
#define b_t(x)          b_x((x), 29, 1) /* 1 if the address is aliased */
#define b_ss(x)         b_x((x), 24, 2) /* the space code */
#define b_bbbbbbbb(x)   b_x((x), 16, 8) /* bus number */
#define b_ddddd(x)      b_x((x), 11, 5) /* device number */
#define b_fff(x)        b_x((x), 8, 3)  /* function number */
#define b_rrrrrrrr(x)   b_x((x), 0, 8)  /* register number */

static void spapr_create_drc_phb_dt_entries(void *fdt, int bus_off, int phb_index)
{
    char char_buf[1024];
    uint32_t int_buf[SPAPR_DRC_PHB_SLOT_MAX + 1];
    uint32_t *entries;
    int i, ret, offset;

    /* ibm,drc-indexes */
    memset(int_buf, 0 , sizeof(int_buf));
    int_buf[0] = SPAPR_DRC_PHB_SLOT_MAX;

    for (i = 1; i <= SPAPR_DRC_PHB_SLOT_MAX; i++) {
        int_buf[i] = SPAPR_DRC_DEV_ID_BASE + (phb_index << 8) + ((i - 1) << 3);
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-indexes", int_buf,
                      sizeof(int_buf));
    if (ret) {
        g_warning("error adding 'ibm,drc-indexes' field for PHB FDT");
    }

    /* ibm,drc-power-domains */
    memset(int_buf, 0, sizeof(int_buf));
    int_buf[0] = SPAPR_DRC_PHB_SLOT_MAX;

    for (i = 1; i <= SPAPR_DRC_PHB_SLOT_MAX; i++) {
        int_buf[i] = 0xffffffff;
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-power-domains", int_buf,
                      sizeof(int_buf));
    if (ret) {
        g_warning("error adding 'ibm,drc-power-domains' field for PHB FDT");
    }

    /* ibm,drc-names */
    memset(char_buf, 0, sizeof(char_buf));
    entries = (uint32_t *)&char_buf[0];
    *entries = SPAPR_DRC_PHB_SLOT_MAX;
    offset = sizeof(*entries);

    for (i = 1; i <= SPAPR_DRC_PHB_SLOT_MAX; i++) {
        offset += sprintf(char_buf + offset, "Slot %d",
                          (phb_index * SPAPR_DRC_PHB_SLOT_MAX) + i - 1);
        char_buf[offset++] = '\0';
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-names", char_buf, offset);
    if (ret) {
        g_warning("error adding 'ibm,drc-names' field for PHB FDT");
    }

    /* ibm,drc-types */
    memset(char_buf, 0, sizeof(char_buf));
    entries = (uint32_t *)&char_buf[0];
    *entries = SPAPR_DRC_PHB_SLOT_MAX;
    offset = sizeof(*entries);

    for (i = 0; i < SPAPR_DRC_PHB_SLOT_MAX; i++) {
        offset += sprintf(char_buf + offset, "SLOT");
        char_buf[offset++] = '\0';
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-types", char_buf, offset);
    if (ret) {
        g_warning("error adding 'ibm,drc-types' field for PHB FDT");
    }
}


int spapr_populate_pci_dt(sPAPRPHBState *phb,
                          uint32_t xics_phandle, uint32_t drc_index,
                          void *fdt)
{
    int bus_off, i, j;
    char nodename[256];
    uint32_t bus_range[] = { cpu_to_be32(0), cpu_to_be32(0xff) };
    struct {
        uint32_t hi;
        uint64_t child;
        uint64_t parent;
        uint64_t size;
    } QEMU_PACKED ranges[] = {
        {
            cpu_to_be32(b_ss(1)), cpu_to_be64(0),
            cpu_to_be64(phb->io_win_addr),
            cpu_to_be64(memory_region_size(&phb->iospace)),
        },
        {
            cpu_to_be32(b_ss(2)), cpu_to_be64(SPAPR_PCI_MEM_WIN_BUS_OFFSET),
            cpu_to_be64(phb->mem_win_addr),
            cpu_to_be64(memory_region_size(&phb->memwindow)),
        },
    };
    uint64_t bus_reg[] = { cpu_to_be64(phb->buid), 0 };
    uint32_t interrupt_map_mask[] = {
        cpu_to_be32(b_ddddd(-1)|b_fff(0)), 0x0, 0x0, cpu_to_be32(-1)};
    uint32_t interrupt_map[PCI_SLOT_MAX * PCI_NUM_PINS][7];

    /* Start populating the FDT */
    sprintf(nodename, "pci@%" PRIx64, phb->buid);
    bus_off = fdt_add_subnode(fdt, 0, nodename);
    if (bus_off < 0) {
        return bus_off;
    }


    /* Write PHB properties */
    _FDT(fdt_setprop_string(fdt, bus_off, "device_type", "pci"));
    _FDT(fdt_setprop_string(fdt, bus_off, "compatible", "IBM,Logical_PHB"));
    _FDT(fdt_setprop_cell(fdt, bus_off, "#address-cells", 0x3));
    _FDT(fdt_setprop_cell(fdt, bus_off, "#size-cells", 0x2));
    _FDT(fdt_setprop_cell(fdt, bus_off, "#interrupt-cells", 0x1));
    _FDT(fdt_setprop(fdt, bus_off, "used-by-rtas", NULL, 0));
    _FDT(fdt_setprop(fdt, bus_off, "bus-range", &bus_range, sizeof(bus_range)));
    _FDT(fdt_setprop(fdt, bus_off, "ranges", &ranges, sizeof(ranges)));
    _FDT(fdt_setprop(fdt, bus_off, "reg", &bus_reg, sizeof(bus_reg)));
    _FDT(fdt_setprop_cell(fdt, bus_off, "ibm,pci-config-space-type", 0x1));

    /* Build the interrupt-map, this must matches what is done
     * in pci_spapr_map_irq
     */
    _FDT(fdt_setprop(fdt, bus_off, "interrupt-map-mask",
                     &interrupt_map_mask, sizeof(interrupt_map_mask)));
    for (i = 0; i < PCI_SLOT_MAX; i++) {
        for (j = 0; j < PCI_NUM_PINS; j++) {
            uint32_t *irqmap = interrupt_map[i*PCI_NUM_PINS + j];
            int lsi_num = pci_spapr_swizzle(i, j);

            irqmap[0] = cpu_to_be32(b_ddddd(i)|b_fff(0));
            irqmap[1] = 0;
            irqmap[2] = 0;
            irqmap[3] = cpu_to_be32(j+1);
            irqmap[4] = cpu_to_be32(xics_phandle);
            irqmap[5] = cpu_to_be32(phb->lsi_table[lsi_num].irq);
            irqmap[6] = cpu_to_be32(0x8);
        }
    }
    /* Write interrupt map */
    _FDT(fdt_setprop(fdt, bus_off, "interrupt-map", &interrupt_map,
                     sizeof(interrupt_map)));

    spapr_create_drc_phb_dt_entries(fdt, bus_off, phb->index);
    if (drc_index) {
        _FDT(fdt_setprop(fdt, bus_off, "ibm,my-drc-index", &drc_index,
                         sizeof(drc_index)));
    }

    spapr_dma_dt(fdt, bus_off, "ibm,dma-window",
                 phb->dma_liobn, phb->dma_window_start,
                 phb->dma_window_size);

    return 0;
}

void spapr_pci_rtas_init(void)
{
    spapr_rtas_register("read-pci-config", rtas_read_pci_config);
    spapr_rtas_register("write-pci-config", rtas_write_pci_config);
    spapr_rtas_register("ibm,read-pci-config", rtas_ibm_read_pci_config);
    spapr_rtas_register("ibm,write-pci-config", rtas_ibm_write_pci_config);
    if (msi_supported) {
        spapr_rtas_register("ibm,query-interrupt-source-number",
                            rtas_ibm_query_interrupt_source_number);
        spapr_rtas_register("ibm,change-msi", rtas_ibm_change_msi);
    }
    spapr_rtas_register("set-indicator", rtas_set_indicator);
    spapr_rtas_register("set-power-level", rtas_set_power_level);
    spapr_rtas_register("get-sensor-state", rtas_get_sensor_state);
    spapr_rtas_register("ibm,configure-connector",
                        rtas_ibm_configure_connector);
}

static void spapr_pci_register_types(void)
{
    type_register_static(&spapr_phb_info);
}

type_init(spapr_pci_register_types)
