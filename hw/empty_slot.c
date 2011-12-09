/*
 * QEMU Empty Slot
 *
 * The empty_slot device emulates known to a bus but not connected devices.
 *
 * Copyright (c) 2010 Artyom Tarasenko
 *
 * This code is licensed under the GNU GPL v2 or (at your option) any later
 * version.
 */

#include "hw.h"
#include "sysbus.h"
#include "empty_slot.h"

//#define DEBUG_EMPTY_SLOT

#ifdef DEBUG_EMPTY_SLOT
#define DPRINTF(fmt, ...)                                       \
    do { printf("empty_slot: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

typedef struct EmptySlot {
    SysBusDevice busdev;
    uint64_t size;
} EmptySlot;

static uint32_t empty_slot_readl(void *opaque, target_phys_addr_t addr)
{
    DPRINTF("read from " TARGET_FMT_plx "\n", addr);
    return 0;
}

static void empty_slot_writel(void *opaque, target_phys_addr_t addr,
                              uint32_t val)
{
    DPRINTF("write 0x%x to " TARGET_FMT_plx "\n", val, addr);
}

CPUReadMemoryFunc * const empty_slot_read[3] = {
    empty_slot_readl,
    empty_slot_readl,
    empty_slot_readl,
};

static CPUWriteMemoryFunc * const empty_slot_write[3] = {
    empty_slot_writel,
    empty_slot_writel,
    empty_slot_writel,
};

void empty_slot_init(target_phys_addr_t addr, uint64_t slot_size)
{
    if (slot_size > 0) {
        /* Only empty slots larger than 0 byte need handling. */
        DeviceState *dev;
        SysBusDevice *s;
        EmptySlot *e;

        dev = qdev_create(NULL, "empty_slot");
        s = sysbus_from_qdev(dev);
        e = FROM_SYSBUS(EmptySlot, s);
        e->size = slot_size;

        qdev_init_nofail(dev);

        sysbus_mmio_map(s, 0, addr);
    }
}

static int empty_slot_init1(SysBusDevice *dev)
{
    EmptySlot *s = FROM_SYSBUS(EmptySlot, dev);
    ram_addr_t empty_slot_offset;

    empty_slot_offset = cpu_register_io_memory(empty_slot_read,
                                               empty_slot_write, s,
                                               DEVICE_NATIVE_ENDIAN);
    sysbus_init_mmio(dev, s->size, empty_slot_offset | IO_MEM_RAM);
    return 0;
}

static void empty_slot_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = empty_slot_init1;
}

static DeviceInfo empty_slot_info = {
    .name = "empty_slot",
    .size = sizeof(EmptySlot),
    .class_init = empty_slot_class_init,
};

static void empty_slot_register_devices(void)
{
    qdev_register_subclass(&empty_slot_info, TYPE_SYS_BUS_DEVICE);
}

device_init(empty_slot_register_devices);
