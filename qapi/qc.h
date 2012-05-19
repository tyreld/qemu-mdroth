#ifndef QC_H
#define QC_H

#include "qemu-timer.h"

#define qc_declaration
#define _immutable
#define _derived
#define _broken

/* misc. visitors */
void visit_type_tm(Visitor *m, struct tm *obj, const char *name, Error **errp);
void visit_type_qemu_irq(Visitor *m, void **obj, const char *name,
                         Error **errp);
void visit_type_QEMUTimer(Visitor *v, QEMUTimer **obj, const char *name,
                          Error **errp);

#endif
