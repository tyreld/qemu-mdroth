/*
 * QSource: QEMU event source class
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QSOURCE_H
#define QSOURCE_H

#include "qom/object.h"
#include "qcontext/qcontext.h"

typedef struct QSource QSource;

typedef bool (*QSourceCB)(QSource *qsource);

typedef struct QSourceFuncs {
    bool (*prepare)(QSource *qsource, int *timeout);
    bool (*check)(QSource *qsource);
    bool (*dispatch)(QSource *qsource);
    void (*finalize)(QSource *qsource);
} QSourceFuncs;

typedef struct QSourceClass {
    ObjectClass parent_class;

    void (*add_poll)(QSource *qsource, GPollFD *pfd);
    void (*remove_poll)(QSource *qsource, GPollFD *pfd);
    void (*set_source_funcs)(QSource *qsource, QSourceFuncs funcs);
    QSourceCB (*get_callback_func)(QSource *qsource);
    void (*set_callback_func)(QSource *qsource, QSourceCB cb);
    void (*set_user_data)(QSource *qsource, void *user_data);
    void *(*get_user_data)(QSource *qsource);
} QSourceClass;

struct QSource {
    /* <private */
    Object parent_obj;

    QSourceFuncs source_funcs;
    QSourceCB callback_func;
    GArray *poll_fds;
    void *user_data;
    struct QContext *ctx;
    char *name;

    /* <public> */
};

#define TYPE_QSOURCE "qsource"
#define QSOURCE(obj) OBJECT_CHECK(QSource, (obj), TYPE_QSOURCE)
#define QSOURCE_CLASS(klass) OBJECT_CLASS_CHECK(QSourceClass, (klass), TYPE_QSOURCE)
#define QSOURCE_GET_CLASS(obj) OBJECT_GET_CLASS(QSourceClass, (obj), TYPE_QSOURCE)

QSource *qsource_new(QSourceFuncs source_funcs, QSourceCB cb, const char *name,
                   void *opaque);

#endif /* QSOURCE_H */
