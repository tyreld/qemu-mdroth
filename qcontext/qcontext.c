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
#include <stdio.h>
#include "qom/object.h"
#include "qemu/module.h"
#include "qcontext/qcontext.h"
#include "qapi/error.h"
#include "string.h"

/* TODO: this is for compatibility with -object, but really these
 * should probably live in /qcontexts or something
 */
#define QCONTEXT_ROOT_CONTAINER "/objects"

/* QContext property accessors */

static char *qcontext_get_id(Object *obj, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);

    return ctx->id ? g_strdup(ctx->id) : NULL;
}

static void qcontext_set_id(Object *obj, const char *id, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);
    QContextClass *ctxk = QCONTEXT_GET_CLASS(ctx);
    Object *root_container = container_get(object_get_root(),
                                           QCONTEXT_ROOT_CONTAINER);

    if (id) {
        object_property_add_child(root_container, id, OBJECT(ctx), errp);
        ctx->id = g_strdup(id);
    } else {
        ctx->id = object_property_add_unnamed_child(root_container,
                                                    OBJECT(ctx), errp);
    }

    if (ctxk->set_id_hook) {
        ctxk->set_id_hook(ctx, id, errp);
    }
}

static char *qcontext_get_threaded(Object *obj, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);

    return ctx->threaded ? g_strdup("yes") : g_strdup("no");
}

static void qcontext_set_threaded(Object *obj, const char *threaded,
                                  Error **errp)
{
    QContext *ctx = QCONTEXT(obj);

    if (strcmp(threaded, "yes") == 0) {
        ctx->threaded = true;
        ctx->should_run = true;
    } else if (strcmp(threaded, "no") == 0) {
        ctx->threaded = false;
        ctx->should_run = false;
    } else {
        error_setg(errp,
                   "invalid value for \"threaded\","
                   " must specify \"yes\" or \"no\"");
    }
}

/* QOM interfaces */

static void qcontext_initfn(Object *obj)
{
    /* note: controlling these as properties is somewhat awkward. these are
     * really static initialization parameters, but we do it this way so we
     * can instantiate from the command-line via -object.
     */
    object_property_add_str(obj, "id",
                            qcontext_get_id,
                            qcontext_set_id,
                            NULL);
    object_property_add_str(obj, "threaded",
                            qcontext_get_threaded,
                            qcontext_set_threaded,
                            NULL);
}

static void qcontext_init_completionfn(Object *obj)
{
    QContext *ctx = QCONTEXT(obj);
    QContextClass *ctxk = QCONTEXT_GET_CLASS(ctx);
    gchar *path, *id;

    /* this means we were created via -object, and were added to
     * the qom tree outside of the "id" property setter. Update
     * our internal structures to reflect this, and execute the
     * set_id_hook() accordingly
     */
    if (!ctx->id) {
        path = object_get_canonical_path(obj);
        id = g_strrstr(path, "/") + 1;
        ctx->id = g_strdup(id);
        g_free(path);
        if (ctxk->set_id_hook) {
            ctxk->set_id_hook(ctx, ctx->id, NULL);
        }
    }

    if (ctx->threaded) {
        qcontext_create_thread(ctx);
    }
}

static void qcontext_finalizefn(Object *obj)
{
    QContext *ctx = QCONTEXT(obj);

    if (ctx->threaded) {
        qcontext_stop_thread(ctx);
    }
}

static void qcontext_class_initfn(ObjectClass *class, void *data)
{
}

static const TypeInfo qcontext_type_info = {
    .name = TYPE_QCONTEXT,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(QContext),
    .instance_init = qcontext_initfn,
    .instance_init_completion = qcontext_init_completionfn,
    .instance_finalize = qcontext_finalizefn,
    .class_size = sizeof(QContextClass),
    .class_init = qcontext_class_initfn,
    .abstract = true, /* this should be abstract, just for testing */
};

void qcontext_register_types(void)
{
    type_register_static(&qcontext_type_info);
}

/* FIXME: for some very strange reason, the constructor function this
 * generates doesn't get executed for qemu. it does for test-qcontext
 * though, and in both cases the constructor for glib-qcontext gets
 * executed okay, so for now just do registration for qcontext there
 * as well by making qcontext_register_types() a global and calling it
 * from there
 */
//type_init(qcontext_register_types)

/* QContext method wrappers. Somewhat redundant but it saves on typing */

bool qcontext_prepare(QContext *ctx, int *timeout)
{
    return QCONTEXT_GET_CLASS(ctx)->prepare(ctx, timeout);
}

bool qcontext_poll(QContext *ctx, int timeout)
{
    return QCONTEXT_GET_CLASS(ctx)->poll(ctx, timeout);
}

bool qcontext_check(QContext *ctx)
{
    return QCONTEXT_GET_CLASS(ctx)->check(ctx);
}

void qcontext_dispatch(QContext *ctx)
{
    QCONTEXT_GET_CLASS(ctx)->dispatch(ctx);
}

void qcontext_notify(QContext *ctx)
{
    QCONTEXT_GET_CLASS(ctx)->notify(ctx);
}

void qcontext_attach(QContext *ctx, QSource *source, Error **errp)
{
    QCONTEXT_GET_CLASS(ctx)->attach(ctx, source, errp);
}

void qcontext_detach(QContext *ctx, QSource *source, Error **errp)
{
    QCONTEXT_GET_CLASS(ctx)->detach(ctx, source, errp);
}

QSource *qcontext_find_source_by_name(QContext *ctx, const char *name)
{
    return QCONTEXT_GET_CLASS(ctx)->find_source_by_name(ctx, name);
}

/* Helper functions for working with QContexts */

QContext *qcontext_find_by_name(const char *name, Error **errp)
{
    char path[256];

    sprintf(path, "%s/%s", QCONTEXT_ROOT_CONTAINER, name);
    return QCONTEXT(object_resolve_path_type(path, TYPE_QCONTEXT, NULL));
}

bool qcontext_iterate(QContext *ctx, bool blocking)
{
    int timeout = 0; 

    if (qcontext_prepare(ctx, &timeout)) {
        qcontext_dispatch(ctx);
        return true;
    }

    if (qcontext_poll(ctx, blocking ? timeout : 0) &&
        qcontext_check(ctx)) {
        qcontext_dispatch(ctx);
        return true;
    }

    return false;
}

static void *qcontext_thread_fn(void *opaque)
{
    QContext *ctx = opaque;
    while (ctx->should_run) {
        qcontext_iterate(ctx, true);
    }
    return NULL;
}

void qcontext_create_thread(QContext *ctx)
{
    qemu_thread_create(&ctx->thread, qcontext_thread_fn,
                       ctx, QEMU_THREAD_JOINABLE);
}

void qcontext_stop_thread(QContext *ctx)
{
    ctx->should_run = false;
    qcontext_notify(ctx);
    qemu_thread_join(&ctx->thread);
    ctx->threaded = false;
}
