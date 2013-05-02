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
#include <glib.h>
#include <string.h>
#include "qom/object.h"
#include "qcontext/qcontext.h"
#include "qcontext/glib-qcontext.h"
#include "qemu/module.h"
#include "qapi/error.h"

struct GlibQSource {
    GSource source;
    guint source_id;
    char *name;
    QSource *qsource;
    QTAILQ_ENTRY(GlibQSource) next;
};

static gboolean glib_qcontext_gsource_prepare(GSource *source, int *timeout)
{
    GlibQSource *gqsource = (GlibQSource *)source;
    QSource *qsource = gqsource->qsource;

    return qsource->source_funcs.prepare(qsource, timeout);
}

static gboolean glib_qcontext_gsource_check(GSource *source)
{
    GlibQSource *gqsource = (GlibQSource *)source;
    QSource *qsource = gqsource->qsource;

    return qsource->source_funcs.check(qsource);
}

static gboolean glib_qcontext_gsource_dispatch(GSource *source, GSourceFunc cb,
                                              gpointer user_data)
{
    GlibQSource *gqsource = (GlibQSource *)source;
    QSource *qsource = gqsource->qsource;

    return qsource->source_funcs.dispatch(qsource);
}

static void glib_qcontext_gsource_finalize(GSource *source)
{
    GlibQSource *gqsource = (GlibQSource *)source;
    QSource *qsource = gqsource->qsource;

    qsource->source_funcs.finalize(qsource);
}

GSourceFuncs glib_gsource_funcs = {
    glib_qcontext_gsource_prepare,
    glib_qcontext_gsource_check,
    glib_qcontext_gsource_dispatch,
    glib_qcontext_gsource_finalize,
};

/* external interfaces */

static bool glib_qcontext_prepare(QContext *ctx, int *timeout)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);
    gint calculated_timeout = 0;
    gboolean ret = g_main_context_prepare(gctx->g_main_context,
                                          &gctx->max_priority);

    gctx->n_poll_fds = g_main_context_query(gctx->g_main_context,
                                            gctx->max_priority,
                                            &calculated_timeout,
                                            gctx->poll_fds,
                                            GLIB_QCONTEXT_MAX_POLL_FDS);
    if (timeout) {
        *timeout = calculated_timeout;
    }

    return ret;
}

static bool glib_qcontext_poll(QContext *ctx, int timeout)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);

    return g_poll(gctx->poll_fds, gctx->n_poll_fds, timeout) > 0;
}

static bool glib_qcontext_check(QContext *ctx)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);

    return g_main_context_check(gctx->g_main_context,
                                gctx->max_priority,
                                gctx->poll_fds,
                                gctx->n_poll_fds);
}

static void glib_qcontext_dispatch(QContext *ctx)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);
    g_main_context_dispatch(gctx->g_main_context);
}

static void glib_qcontext_notify(QContext *ctx)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);
    GlibQContextClass *gctxk = GLIB_QCONTEXT_GET_CLASS(gctx);
    g_main_context_wakeup(gctxk->get_context(gctx));
}


static void glib_qcontext_attach(QContext *ctx, QSource *qsource, Error **errp)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);
    GSource *source = g_source_new(&glib_gsource_funcs, sizeof(GlibQSource)+512);
    GlibQSource *gqsource = NULL, *new_gqsource = (GlibQSource *)source;
    guint i;

    if (qsource->name) {
        QTAILQ_FOREACH(gqsource, &gctx->sources, next) {
            if (strcmp(gqsource->name, qsource->name) == 0) {
                error_setg(errp, "duplicate name associated with source");
                g_source_destroy(source);
                return;
            }
        }
    }

    for (i = 0; i < qsource->poll_fds->len; i++) {
        GPollFD *pfd = g_array_index(qsource->poll_fds, GPollFD *, i);
        g_source_add_poll(source, pfd);
    }

    new_gqsource->qsource = qsource;
    new_gqsource->source_id = g_source_attach(source, gctx->g_main_context);
    new_gqsource->name = g_strdup(qsource->name);
    QTAILQ_INSERT_TAIL(&gctx->sources, new_gqsource, next);
    qsource->ctx = ctx;
}

static void glib_qcontext_detach(QContext *ctx, QSource *qsource, Error **errp)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);
    GlibQSource *gqsource = NULL;

    QTAILQ_FOREACH(gqsource, &gctx->sources, next) {
        if (gqsource->qsource == qsource) {
            break;
        }
    }

    if (gqsource) {
        g_free(gqsource->name);
        g_source_remove(gqsource->source_id);
        QTAILQ_REMOVE(&gctx->sources, gqsource, next);
    }

    qsource->ctx = NULL;
}

static QSource *glib_qcontext_find_source_by_name(QContext *ctx, const char *name)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);
    GlibQSource *gqsource = NULL;

    QTAILQ_FOREACH(gqsource, &gctx->sources, next) {
        if (strcmp(gqsource->name, name) == 0) {
            break;
        }
    }

    return gqsource->qsource;
}

static void glib_qcontext_set_id_hook(QContext *ctx, const char *id,
                                      Error **errp)
{
    GlibQContext *gctx = GLIB_QCONTEXT(ctx);

    if (strcmp(id, "main") != 0) {
        gctx->g_main_context = g_main_context_new();
    }
}

/* QOM-driven interfaces */
static void glib_qcontext_initfn(Object *obj)
{
    GlibQContext *gctx = GLIB_QCONTEXT(obj);

    /* TODO: this will be replaced with a new context if we set an ID
     * property other than "main". there's no guarantee we won't attempt
     * to spawn a main loop thread for this context (also done via a dynamic
     * property so we can be fully instantiated via -object) before this
     * happens though. This means we can accidentally execute a number of
     * iterations of the default glib context (bad, since that requires
     * special handling of BQL) before we switch over to the intended
     * context.
     *
     * We seem to need a realizefn for Objects...
     */
    gctx->g_main_context = g_main_context_default();
    QTAILQ_INIT(&gctx->sources);
}

static void glib_qcontext_class_initfn(ObjectClass *class, void *data)
{
    QContextClass *ctxk = QCONTEXT_CLASS(class);
    GlibQContextClass *gctxk = GLIB_QCONTEXT_CLASS(class);

    ctxk->prepare = glib_qcontext_prepare;
    ctxk->poll = glib_qcontext_poll;
    ctxk->check = glib_qcontext_check;
    ctxk->dispatch = glib_qcontext_dispatch;
    ctxk->notify = glib_qcontext_notify;

    ctxk->attach = glib_qcontext_attach;
    ctxk->detach = glib_qcontext_detach;
    ctxk->find_source_by_name = glib_qcontext_find_source_by_name;
    ctxk->set_id_hook = glib_qcontext_set_id_hook;

    gctxk->get_context = glib_qcontext_get_context;
}

static const TypeInfo glib_qcontext_info = {
    .name = TYPE_GLIB_QCONTEXT,
    .parent = TYPE_QCONTEXT,
    .instance_size = sizeof(GlibQContext),
    .class_size = sizeof(GlibQContextClass),
    .instance_init = glib_qcontext_initfn,
    .class_init = glib_qcontext_class_initfn,
    .interfaces = (InterfaceInfo[]) {
        { },
    },
};

static void glib_qcontext_register_types(void)
{
    type_register_static(&glib_qcontext_info);
}

type_init(glib_qcontext_register_types)
type_init(qcontext_register_types)

GlibQContext *glib_qcontext_new(const char *id, bool threaded, Error **errp)
{
    GlibQContext *gctx = GLIB_QCONTEXT(object_new(TYPE_GLIB_QCONTEXT));

    object_property_set_str(OBJECT(gctx), id, "id", errp);
    if (error_is_set(errp)) {
        object_unref(OBJECT(gctx));
        g_warning("marker 0");
        return NULL;
    }

    object_property_set_str(OBJECT(gctx),
                            threaded ? "yes" : "no",
                            "threaded", errp);
    if (error_is_set(errp)) {
        object_unref(OBJECT(gctx));
        g_warning("marker 1");
        return NULL;
    }

    object_init_completion(OBJECT(gctx));

    return gctx;
}

GMainContext *glib_qcontext_get_context(GlibQContext *gctx)
{
    return gctx->g_main_context;
}
