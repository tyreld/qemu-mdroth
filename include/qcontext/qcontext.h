/*
 * QContext: QEMU event loop context class
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QCONTEXT_H
#define QCONTEXT_H

#include "qom/object.h"
#include "qcontext/qsource.h"
#include "qapi/error.h"
#include "qemu/thread.h"

/* QContext base class */

typedef struct QContext QContext;

typedef struct QContextClass {
    ObjectClass parent_class;

    /* called after QContext id property has been set */
    void (*set_id_hook)(QContext *ctx, const char *name, Error **errp);

    /* QContext event loop functions, abstract interfaces */
    bool (*prepare)(QContext *ctx, int *timeout);
    bool (*poll)(QContext *ctx, int timeout);
    bool (*check)(QContext *ctx);
    void (*dispatch)(QContext *ctx);
    void (*notify)(QContext *ctx);

    /* QSource registration, abstract interfaces */
    void (*attach)(QContext *ctx, QSource *qsource, Error **errp);
    void (*detach)(QContext *ctx, QSource *qsource, Error **errp);
    QSource *(*find_source_by_name)(QContext *ctx, const char *name);
} QContextClass;

struct QContext {
    Object parent_obj;
    Object *container;
    char *id;
    QemuThread thread;
    bool threaded;
    bool should_run;
};

#define TYPE_QCONTEXT "qcontext"
#define QCONTEXT(obj) OBJECT_CHECK(QContext, (obj), TYPE_QCONTEXT)
#define QCONTEXT_CLASS(klass) OBJECT_CLASS_CHECK(QContextClass, (klass), TYPE_QCONTEXT)
#define QCONTEXT_GET_CLASS(obj) OBJECT_GET_CLASS(QContextClass, (obj), TYPE_QCONTEXT)

/* wrapper functions for object methods */

bool qcontext_prepare(QContext *ctx, int *timeout);
bool qcontext_poll(QContext *ctx, int timeout);
bool qcontext_check(QContext *ctx);
void qcontext_dispatch(QContext *ctx);
void qcontext_notify(QContext *ctx);
void qcontext_attach(QContext *ctx, QSource *qsource, Error **errp);
void qcontext_detach(QContext *ctx, QSource *qsource, Error **errp);
QSource *qcontext_find_source_by_name(QContext *ctx, const char *name);

/* helper functions for working with qcontexts */

QContext *qcontext_find_by_name(const char *name, Error **errp);
bool qcontext_iterate(QContext *ctx, bool blocking);
void qcontext_create_thread(QContext *ctx);
void qcontext_stop_thread(QContext *ctx);
void qcontext_register_types(void);

#endif /* QCONTEXT_H */
