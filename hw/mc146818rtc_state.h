#ifndef MC146818RTC_STATE_H
#define MC146818RTC_STATE_H

#include "isa.h"
#include "qapi/qc.h"

qc_declaration typedef struct RTCState {
    ISADevice _immutable dev;
    MemoryRegion _immutable io;
    uint8_t cmos_data[128];
    uint8_t cmos_index;
    struct tm current_tm;
    int32_t base_year;
    qemu_irq _immutable irq;
    qemu_irq _immutable sqw_irq;
    int32_t _immutable it_shift;
    /* periodic timer */
    QEMUTimer *periodic_timer;
    int64_t next_periodic_time;
    /* second update */
    int64_t next_second_time;
    uint16_t _derived irq_reinject_on_ack_count;
    uint32_t irq_coalesced;
    uint32_t period;
    QEMUTimer _broken *coalesced_timer;
    QEMUTimer *second_timer;
    QEMUTimer *second_timer2;
    Notifier _broken clock_reset_notifier;
    LostTickPolicy _immutable lost_tick_policy;
    Notifier _broken suspend_notifier;
} RTCState;

#endif /* !MC146818RTC_STATE_H */
