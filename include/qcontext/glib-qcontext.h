/*
 * GlibQContext: GLib-based QContext implementation
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef GLIB_QCONTEXT_H
#define GLIB_QCONTEXT_H

#include <glib.h>
#include "qom/object.h"
#include "qapi/error.h"
#include "qcontext/qcontext.h"

/* QContextGlib implementation */

#define TYPE_GLIB_QCONTEXT "glib-qcontext"
#define GLIB_QCONTEXT(obj) \
        OBJECT_CHECK(GlibQContext, (obj), TYPE_GLIB_QCONTEXT)
#define GLIB_QCONTEXT_CLASS(klass) \
        OBJECT_CLASS_CHECK(GlibQContextClass, (klass), TYPE_GLIB_QCONTEXT)
#define GLIB_QCONTEXT_GET_CLASS(obj) \
        OBJECT_GET_CLASS(GlibQContextClass, (obj), TYPE_GLIB_QCONTEXT)

#define GLIB_QCONTEXT_MAX_POLL_FDS (2 * 1024)

typedef struct GlibQSource GlibQSource;

typedef struct GlibQContext {
    /* <private */
    QContext parent;

    char *test;
    GMainContext *g_main_context;
    int max_priority;
    GPollFD poll_fds[GLIB_QCONTEXT_MAX_POLL_FDS];
    gint n_poll_fds;
    QTAILQ_HEAD(, GlibQSource) sources;

    /* <public> */
} GlibQContext;

typedef struct GlibQContextClass {
    QContextClass parent;

    void (*init)(GlibQContext *gctx, const char *name, Error **errp);
    void (*set_context)(GlibQContext *gctx, GMainContext *ctx);
    GMainContext *(*get_context)(GlibQContext *gctx);
} GlibQContextClass;

GlibQContext *glib_qcontext_new(const char *id, bool threaded, Error **errp);
GMainContext *glib_qcontext_get_context(GlibQContext *gctx);

#endif /* GLIB_QCONTEXT_H */
