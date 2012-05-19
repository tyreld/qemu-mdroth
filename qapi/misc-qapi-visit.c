#include <time.h>
#include "qapi-visit-core.h"
#include "qc.h"
#include "qemu-timer.h"

void visit_type_tm(Visitor *v, struct tm *obj, const char *name, Error **errp)
{
    visit_start_struct(v, NULL, "struct tm", name, 0, errp);
    visit_type_int32(v, &obj->tm_year, "tm_year", errp);
    visit_type_int32(v, &obj->tm_mon, "tm_mon", errp);
    visit_type_int32(v, &obj->tm_mday, "tm_mday", errp);
    visit_type_int32(v, &obj->tm_hour, "tm_hour", errp);
    visit_type_int32(v, &obj->tm_min, "tm_min", errp);
    visit_type_int32(v, &obj->tm_sec, "tm_sec", errp);
    visit_end_struct(v, errp);
}

void visit_type_qemu_irq(Visitor *v, void **obj, const char *name,
                         Error **errp)
{
}

void visit_type_QEMUTimer(Visitor *v, QEMUTimer **obj, const char *name,
                          Error **errp)
{
    int64_t expire_time, expire_time_cpy;
    expire_time = expire_time_cpy = qemu_timer_expire_time_ns(*obj);
    visit_start_struct(v, NULL, "QEMUTimer", name, 0, errp);
    visit_type_int64(v, &expire_time, "expire_time", errp);
    visit_end_struct(v, errp);

    /* if we're modifying a QEMUTimer, re-arm/delete accordingly */
    if (expire_time != expire_time_cpy) {
        if (expire_time != -1) {
            qemu_mod_timer_ns(*obj, expire_time);
        } else {
            qemu_del_timer(*obj);
        }
    }
}
