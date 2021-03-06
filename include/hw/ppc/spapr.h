#if !defined(__HW_SPAPR_H__)
#define __HW_SPAPR_H__

#include "sysemu/dma.h"
#include "hw/ppc/xics.h"

struct VIOsPAPRBus;
struct sPAPRPHBState;
struct sPAPRNVRAM;

#define HPTE64_V_HPTE_DIRTY     0x0000000000000040ULL

typedef struct sPAPREnvironment {
    struct VIOsPAPRBus *vio_bus;
    QLIST_HEAD(, sPAPRPHBState) phbs;
    hwaddr msi_win_addr;
    MemoryRegion msiwindow;
    struct sPAPRNVRAM *nvram;
    XICSState *icp;

    hwaddr ram_limit;
    void *htab;
    uint32_t htab_shift;
    hwaddr rma_size;
    int vrma_adjust;
    hwaddr fdt_addr, rtas_addr;
    long rtas_size;
    void *fdt_skel;
    void *fdt;
    target_ulong entry_point;
    uint32_t next_irq;
    uint64_t rtc_offset;
    char *cpu_model;
    bool has_graphics;

    uint32_t check_exception_irq;
    Notifier epow_notifier;

    /* Migration state */
    int htab_save_index;
    bool htab_first_pass;
    int htab_fd;
    /* platform state - sensors and indicators */
    uint32_t state;
} sPAPREnvironment;

#define H_SUCCESS         0
#define H_BUSY            1        /* Hardware busy -- retry later */
#define H_CLOSED          2        /* Resource closed */
#define H_NOT_AVAILABLE   3
#define H_CONSTRAINED     4        /* Resource request constrained to max allowed */
#define H_PARTIAL         5
#define H_IN_PROGRESS     14       /* Kind of like busy */
#define H_PAGE_REGISTERED 15
#define H_PARTIAL_STORE   16
#define H_PENDING         17       /* returned from H_POLL_PENDING */
#define H_CONTINUE        18       /* Returned from H_Join on success */
#define H_LONG_BUSY_START_RANGE         9900  /* Start of long busy range */
#define H_LONG_BUSY_ORDER_1_MSEC        9900  /* Long busy, hint that 1msec \
                                                 is a good time to retry */
#define H_LONG_BUSY_ORDER_10_MSEC       9901  /* Long busy, hint that 10msec \
                                                 is a good time to retry */
#define H_LONG_BUSY_ORDER_100_MSEC      9902  /* Long busy, hint that 100msec \
                                                 is a good time to retry */
#define H_LONG_BUSY_ORDER_1_SEC         9903  /* Long busy, hint that 1sec \
                                                 is a good time to retry */
#define H_LONG_BUSY_ORDER_10_SEC        9904  /* Long busy, hint that 10sec \
                                                 is a good time to retry */
#define H_LONG_BUSY_ORDER_100_SEC       9905  /* Long busy, hint that 100sec \
                                                 is a good time to retry */
#define H_LONG_BUSY_END_RANGE           9905  /* End of long busy range */
#define H_HARDWARE        -1       /* Hardware error */
#define H_FUNCTION        -2       /* Function not supported */
#define H_PRIVILEGE       -3       /* Caller not privileged */
#define H_PARAMETER       -4       /* Parameter invalid, out-of-range or conflicting */
#define H_BAD_MODE        -5       /* Illegal msr value */
#define H_PTEG_FULL       -6       /* PTEG is full */
#define H_NOT_FOUND       -7       /* PTE was not found" */
#define H_RESERVED_DABR   -8       /* DABR address is reserved by the hypervisor on this processor" */
#define H_NO_MEM          -9
#define H_AUTHORITY       -10
#define H_PERMISSION      -11
#define H_DROPPED         -12
#define H_SOURCE_PARM     -13
#define H_DEST_PARM       -14
#define H_REMOTE_PARM     -15
#define H_RESOURCE        -16
#define H_ADAPTER_PARM    -17
#define H_RH_PARM         -18
#define H_RCQ_PARM        -19
#define H_SCQ_PARM        -20
#define H_EQ_PARM         -21
#define H_RT_PARM         -22
#define H_ST_PARM         -23
#define H_SIGT_PARM       -24
#define H_TOKEN_PARM      -25
#define H_MLENGTH_PARM    -27
#define H_MEM_PARM        -28
#define H_MEM_ACCESS_PARM -29
#define H_ATTR_PARM       -30
#define H_PORT_PARM       -31
#define H_MCG_PARM        -32
#define H_VL_PARM         -33
#define H_TSIZE_PARM      -34
#define H_TRACE_PARM      -35

#define H_MASK_PARM       -37
#define H_MCG_FULL        -38
#define H_ALIAS_EXIST     -39
#define H_P_COUNTER       -40
#define H_TABLE_FULL      -41
#define H_ALT_TABLE       -42
#define H_MR_CONDITION    -43
#define H_NOT_ENOUGH_RESOURCES -44
#define H_R_STATE         -45
#define H_RESCINDEND      -46
#define H_P2              -55
#define H_P3              -56
#define H_P4              -57
#define H_P5              -58
#define H_P6              -59
#define H_P7              -60
#define H_P8              -61
#define H_P9              -62
#define H_UNSUPPORTED_FLAG -256
#define H_MULTI_THREADS_ACTIVE -9005


/* Long Busy is a condition that can be returned by the firmware
 * when a call cannot be completed now, but the identical call
 * should be retried later.  This prevents calls blocking in the
 * firmware for long periods of time.  Annoyingly the firmware can return
 * a range of return codes, hinting at how long we should wait before
 * retrying.  If you don't care for the hint, the macro below is a good
 * way to check for the long_busy return codes
 */
#define H_IS_LONG_BUSY(x)  ((x >= H_LONG_BUSY_START_RANGE) \
                            && (x <= H_LONG_BUSY_END_RANGE))

/* Flags */
#define H_LARGE_PAGE      (1ULL<<(63-16))
#define H_EXACT           (1ULL<<(63-24))       /* Use exact PTE or return H_PTEG_FULL */
#define H_R_XLATE         (1ULL<<(63-25))       /* include a valid logical page num in the pte if the valid bit is set */
#define H_READ_4          (1ULL<<(63-26))       /* Return 4 PTEs */
#define H_PAGE_STATE_CHANGE (1ULL<<(63-28))
#define H_PAGE_UNUSED     ((1ULL<<(63-29)) | (1ULL<<(63-30)))
#define H_PAGE_SET_UNUSED (H_PAGE_STATE_CHANGE | H_PAGE_UNUSED)
#define H_PAGE_SET_LOANED (H_PAGE_SET_UNUSED | (1ULL<<(63-31)))
#define H_PAGE_SET_ACTIVE H_PAGE_STATE_CHANGE
#define H_AVPN            (1ULL<<(63-32))       /* An avpn is provided as a sanity test */
#define H_ANDCOND         (1ULL<<(63-33))
#define H_ICACHE_INVALIDATE (1ULL<<(63-40))     /* icbi, etc.  (ignored for IO pages) */
#define H_ICACHE_SYNCHRONIZE (1ULL<<(63-41))    /* dcbst, icbi, etc (ignored for IO pages */
#define H_ZERO_PAGE       (1ULL<<(63-48))       /* zero the page before mapping (ignored for IO pages) */
#define H_COPY_PAGE       (1ULL<<(63-49))
#define H_N               (1ULL<<(63-61))
#define H_PP1             (1ULL<<(63-62))
#define H_PP2             (1ULL<<(63-63))

/* H_SET_MODE flags */
#define H_SET_MODE_ENDIAN        4
#define H_SET_MODE_ENDIAN_BIG    0
#define H_SET_MODE_ENDIAN_LITTLE 1

/* VASI States */
#define H_VASI_INVALID    0
#define H_VASI_ENABLED    1
#define H_VASI_ABORTED    2
#define H_VASI_SUSPENDING 3
#define H_VASI_SUSPENDED  4
#define H_VASI_RESUMED    5
#define H_VASI_COMPLETED  6

/* DABRX flags */
#define H_DABRX_HYPERVISOR (1ULL<<(63-61))
#define H_DABRX_KERNEL     (1ULL<<(63-62))
#define H_DABRX_USER       (1ULL<<(63-63))

/* Each control block has to be on a 4K boundary */
#define H_CB_ALIGNMENT     4096

/* pSeries hypervisor opcodes */
#define H_REMOVE                0x04
#define H_ENTER                 0x08
#define H_READ                  0x0c
#define H_CLEAR_MOD             0x10
#define H_CLEAR_REF             0x14
#define H_PROTECT               0x18
#define H_GET_TCE               0x1c
#define H_PUT_TCE               0x20
#define H_SET_SPRG0             0x24
#define H_SET_DABR              0x28
#define H_PAGE_INIT             0x2c
#define H_SET_ASR               0x30
#define H_ASR_ON                0x34
#define H_ASR_OFF               0x38
#define H_LOGICAL_CI_LOAD       0x3c
#define H_LOGICAL_CI_STORE      0x40
#define H_LOGICAL_CACHE_LOAD    0x44
#define H_LOGICAL_CACHE_STORE   0x48
#define H_LOGICAL_ICBI          0x4c
#define H_LOGICAL_DCBF          0x50
#define H_GET_TERM_CHAR         0x54
#define H_PUT_TERM_CHAR         0x58
#define H_REAL_TO_LOGICAL       0x5c
#define H_HYPERVISOR_DATA       0x60
#define H_EOI                   0x64
#define H_CPPR                  0x68
#define H_IPI                   0x6c
#define H_IPOLL                 0x70
#define H_XIRR                  0x74
#define H_PERFMON               0x7c
#define H_MIGRATE_DMA           0x78
#define H_REGISTER_VPA          0xDC
#define H_CEDE                  0xE0
#define H_CONFER                0xE4
#define H_PROD                  0xE8
#define H_GET_PPP               0xEC
#define H_SET_PPP               0xF0
#define H_PURR                  0xF4
#define H_PIC                   0xF8
#define H_REG_CRQ               0xFC
#define H_FREE_CRQ              0x100
#define H_VIO_SIGNAL            0x104
#define H_SEND_CRQ              0x108
#define H_COPY_RDMA             0x110
#define H_REGISTER_LOGICAL_LAN  0x114
#define H_FREE_LOGICAL_LAN      0x118
#define H_ADD_LOGICAL_LAN_BUFFER 0x11C
#define H_SEND_LOGICAL_LAN      0x120
#define H_BULK_REMOVE           0x124
#define H_MULTICAST_CTRL        0x130
#define H_SET_XDABR             0x134
#define H_STUFF_TCE             0x138
#define H_PUT_TCE_INDIRECT      0x13C
#define H_CHANGE_LOGICAL_LAN_MAC 0x14C
#define H_VTERM_PARTNER_INFO    0x150
#define H_REGISTER_VTERM        0x154
#define H_FREE_VTERM            0x158
#define H_RESET_EVENTS          0x15C
#define H_ALLOC_RESOURCE        0x160
#define H_FREE_RESOURCE         0x164
#define H_MODIFY_QP             0x168
#define H_QUERY_QP              0x16C
#define H_REREGISTER_PMR        0x170
#define H_REGISTER_SMR          0x174
#define H_QUERY_MR              0x178
#define H_QUERY_MW              0x17C
#define H_QUERY_HCA             0x180
#define H_QUERY_PORT            0x184
#define H_MODIFY_PORT           0x188
#define H_DEFINE_AQP1           0x18C
#define H_GET_TRACE_BUFFER      0x190
#define H_DEFINE_AQP0           0x194
#define H_RESIZE_MR             0x198
#define H_ATTACH_MCQP           0x19C
#define H_DETACH_MCQP           0x1A0
#define H_CREATE_RPT            0x1A4
#define H_REMOVE_RPT            0x1A8
#define H_REGISTER_RPAGES       0x1AC
#define H_DISABLE_AND_GETC      0x1B0
#define H_ERROR_DATA            0x1B4
#define H_GET_HCA_INFO          0x1B8
#define H_GET_PERF_COUNT        0x1BC
#define H_MANAGE_TRACE          0x1C0
#define H_FREE_LOGICAL_LAN_BUFFER 0x1D4
#define H_QUERY_INT_STATE       0x1E4
#define H_POLL_PENDING          0x1D8
#define H_ILLAN_ATTRIBUTES      0x244
#define H_MODIFY_HEA_QP         0x250
#define H_QUERY_HEA_QP          0x254
#define H_QUERY_HEA             0x258
#define H_QUERY_HEA_PORT        0x25C
#define H_MODIFY_HEA_PORT       0x260
#define H_REG_BCMC              0x264
#define H_DEREG_BCMC            0x268
#define H_REGISTER_HEA_RPAGES   0x26C
#define H_DISABLE_AND_GET_HEA   0x270
#define H_GET_HEA_INFO          0x274
#define H_ALLOC_HEA_RESOURCE    0x278
#define H_ADD_CONN              0x284
#define H_DEL_CONN              0x288
#define H_JOIN                  0x298
#define H_VASI_STATE            0x2A4
#define H_ENABLE_CRQ            0x2B0
#define H_GET_EM_PARMS          0x2B8
#define H_SET_MPP               0x2D0
#define H_GET_MPP               0x2D4
#define H_SET_MODE              0x31C
#define MAX_HCALL_OPCODE        H_SET_MODE

/* The hcalls above are standardized in PAPR and implemented by pHyp
 * as well.
 *
 * We also need some hcalls which are specific to qemu / KVM-on-POWER.
 * So far we just need one for H_RTAS, but in future we'll need more
 * for extensions like virtio.  We put those into the 0xf000-0xfffc
 * range which is reserved by PAPR for "platform-specific" hcalls.
 */
#define KVMPPC_HCALL_BASE       0xf000
#define KVMPPC_H_RTAS           (KVMPPC_HCALL_BASE + 0x0)
#define KVMPPC_H_LOGICAL_MEMOP  (KVMPPC_HCALL_BASE + 0x1)
#define KVMPPC_HCALL_MAX        KVMPPC_H_LOGICAL_MEMOP

extern sPAPREnvironment *spapr;

/*#define DEBUG_SPAPR_HCALLS*/

#ifdef DEBUG_SPAPR_HCALLS
#define hcall_dprintf(fmt, ...) \
    do { fprintf(stderr, "%s: " fmt, __func__, ## __VA_ARGS__); } while (0)
#else
#define hcall_dprintf(fmt, ...) \
    do { } while (0)
#endif

typedef target_ulong (*spapr_hcall_fn)(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                       target_ulong opcode,
                                       target_ulong *args);

void spapr_register_hypercall(target_ulong opcode, spapr_hcall_fn fn);
target_ulong spapr_hypercall(PowerPCCPU *cpu, target_ulong opcode,
                             target_ulong *args);

int spapr_allocate_irq(int hint, bool lsi);
int spapr_allocate_irq_block(int num, bool lsi, bool msi);

static inline int spapr_allocate_msi(int hint)
{
    return spapr_allocate_irq(hint, false);
}

static inline int spapr_allocate_lsi(int hint)
{
    return spapr_allocate_irq(hint, true);
}

static inline uint32_t rtas_ld(target_ulong phys, int n)
{
    return ldl_be_phys(phys + 4*n);
}

static inline void rtas_st(target_ulong phys, int n, uint32_t val)
{
    stl_be_phys(phys + 4*n, val);
}

typedef void (*spapr_rtas_fn)(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                              uint32_t token,
                              uint32_t nargs, target_ulong args,
                              uint32_t nret, target_ulong rets);
int spapr_rtas_register(const char *name, spapr_rtas_fn fn);
target_ulong spapr_rtas_call(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                             uint32_t token, uint32_t nargs, target_ulong args,
                             uint32_t nret, target_ulong rets);
int spapr_rtas_device_tree_setup(void *fdt, hwaddr rtas_addr,
                                 hwaddr rtas_size);

#define SPAPR_TCE_PAGE_SHIFT   12
#define SPAPR_TCE_PAGE_SIZE    (1ULL << SPAPR_TCE_PAGE_SHIFT)
#define SPAPR_TCE_PAGE_MASK    (SPAPR_TCE_PAGE_SIZE - 1)

#define SPAPR_VIO_BASE_LIOBN    0x00000000
#define SPAPR_PCI_BASE_LIOBN    0x80000000

#define RTAS_ERROR_LOG_MAX      2048

#define RTAS_EVENT_SCAN_RATE    1

typedef struct sPAPRTCETable sPAPRTCETable;

#define TYPE_SPAPR_TCE_TABLE "spapr-tce-table"
#define SPAPR_TCE_TABLE(obj) \
    OBJECT_CHECK(sPAPRTCETable, (obj), TYPE_SPAPR_TCE_TABLE)

struct sPAPRTCETable {
    DeviceState parent;
    uint32_t liobn;
    uint32_t window_size;
    uint32_t nb_table;
    uint64_t *table;
    bool bypass;
    int fd;
    MemoryRegion iommu;
    QLIST_ENTRY(sPAPRTCETable) list;
};

void spapr_events_init(sPAPREnvironment *spapr);
void spapr_events_fdt_skel(void *fdt, uint32_t check_exception_irq);
sPAPRTCETable *spapr_tce_new_table(DeviceState *owner, uint32_t liobn,
                                   size_t window_size);
MemoryRegion *spapr_tce_get_iommu(sPAPRTCETable *tcet);
void spapr_tce_set_bypass(sPAPRTCETable *tcet, bool bypass);
int spapr_dma_dt(void *fdt, int node_off, const char *propname,
                 uint32_t liobn, uint64_t window, uint32_t size);
int spapr_tcet_dma_dt(void *fdt, int node_off, const char *propname,
                      sPAPRTCETable *tcet);

struct sPAPRPHBState;

typedef struct ConfigureConnectorState {
    void *fdt;
    int offset;
    int depth;
    enum {
        CC_STATE_IDLE = 0,
        CC_STATE_PENDING = 0,
        CC_STATE_ACTIVE,
    } state;
} ConfigureConnectorState;

/* the state field needs to support multiple index values */

#define INDICATOR_ISOLATION_MASK            0x0001   /* 9001 one bit */
#define INDICATOR_GLOBAL_INTERRUPT_MASK     0x0002   /* 9005 one bit */
#define INDICATOR_ERROR_LOG_MASK            0x0004   /* 9006 one bit */
#define INDICATOR_IDENTIFY_MASK             0x0008   /* 9007 one bit */
#define INDICATOR_RESET_MASK                0x0010   /* 9009 one bit */
#define INDICATOR_DR_MASK                   0x00e0   /* 9002 three bits */
#define INDICATOR_ALLOCATION_MASK           0x0300   /* 9003 two bits */
#define INDICATOR_EPOW_MASK                 0x1c00   /* 9 three bits */
#define SENSOR_ENTITY_SENSE_MASK            0xe000   /* 9003 three bits */

#define INDICATOR_ISOLATION_SHIFT           0x00     /* bit 0 */
#define INDICATOR_GLOBAL_INTERRUPT_SHIFT    0x01     /* bit 1 */
#define INDICATOR_ERROR_LOG_SHIFT           0x02     /* bit 2 */
#define INDICATOR_IDENTIFY_SHIFT            0x03     /* bit 3 */
#define INDICATOR_RESET_SHIFT               0x04     /* bit 4 */
#define INDICATOR_DR_SHIFT                  0x05     /* bits 5-7 */
#define INDICATOR_ALLOCATION_SHIFT          0x08     /* bits 8-9 */
#define INDICATOR_EPOW_SHIFT                0x0a     /* bits 10-12 */
#define SENSOR_ENTITY_SENSE_SHIFT           0x0d     /* bits 13-15 */

#define DECODE_DRC_STATE(state, m, s)                  \
    ((((uint32_t)(state) & (uint32_t)(m))) >> (s))

#define ENCODE_DRC_STATE(val, m, s) \
    (((uint32_t)(val) << (s)) & (uint32_t)(m))

#define EPOW_NORMAL 11
#define DR_ENTITY_SENSE_PRESENT 1
#define NO_SUCH_INDICATOR -3
typedef struct DrcEntry DrcEntry;

struct DrcEntry {
    uint32_t drc_index;
    uint64_t phb_buid;
    void *fdt;
    int fdt_offset;
    uint32_t state;
    ConfigureConnectorState cc_state;
    DrcEntry *child_entries;
};

#define FDT_MAX_SIZE            0x10000
#define SPAPR_DRC_TABLE_SIZE    32
#define SPAPR_DRC_PHB_SLOT_MAX  32
#define SPAPR_DRC_DEV_ID_BASE   0x40000000
extern DrcEntry drc_table[SPAPR_DRC_TABLE_SIZE];
DrcEntry *spapr_add_phb_to_drc_table(uint64_t buid, uint32_t state);
DrcEntry *spapr_phb_to_drc_entry(uint64_t buid);
DrcEntry *spapr_find_drc_entry(int drc_index);

void spapr_pci_hotplug_add(DeviceState *qdev, int slot);
void spapr_pci_hotplug_remove(DeviceState *qdev, int slot);
void spapr_load_phb_node(DrcEntry *drc_entry);
void print_fdt(void *fdt, int offset, int depth);
#endif /* !defined (__HW_SPAPR_H__) */
