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
} QContextClass;

struct QContext {
    Object parent_obj;
    Object *container;
    char *id;
    QemuThread thread;
    bool threaded;
    int thread_id;
    bool should_run;
    GMainContext *gmctx;
    GHashTable *named_sources;
    gint last_pfd_count;
};

#define QEMU_QCONTEXT_MAIN "qcontext-main"
#define TYPE_QCONTEXT "qcontext"
#define QCONTEXT(obj) OBJECT_CHECK(QContext, (obj), TYPE_QCONTEXT)
#define QCONTEXT_CLASS(klass) OBJECT_CLASS_CHECK(QContextClass, (klass), TYPE_QCONTEXT)
#define QCONTEXT_GET_CLASS(obj) OBJECT_GET_CLASS(QContextClass, (obj), TYPE_QCONTEXT)

/* helper functions for working with qcontexts */

QContext *qcontext_find_by_name(const char *name, Error **errp);
void qcontext_create_thread(QContext *ctx);
void qcontext_stop_thread(QContext *ctx);
GMainContext *qcontext_get_context(QContext *ctx);
void qcontext_attach_source(QContext *ctx, GSource *source,
                            const char *name);
void qcontext_destroy_source(QContext *ctx, GSource *source);
GSource *qcontext_find_source_by_name(QContext *ctx, const char *name);
void qcontext_notify(QContext *ctx);
QContext *qcontext_new(const char *id, bool threaded, Error **errp);
void qcontext_register_types(void);

/* wrapper functions for g_main_context_* loop functions */

gboolean qcontext_prepare(QContext *ctx, gint *priority);
gint qcontext_append_query(QContext *ctx, gint max_priority, gint *timeout,
                           GArray *pfd_array);
gint qcontext_check(QContext *ctx, gint max_priority, GPollFD *fds, gint n_fds);
void qcontext_dispatch(QContext *ctx);
gboolean qcontext_iterate(QContext *ctx, gboolean blocking);

#endif /* QCONTEXT_H */
