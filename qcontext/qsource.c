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
#include <glib.h>
#include "qom/object.h"
#include "qcontext/qcontext.h"
#include "qemu/module.h"

/* FIXME: this basically causes us to destroy/rebuild an
 * attached QSource/GSource every time we modify. What we
 * should really have is an interface in the QContext for
 * modifying an already attached source to avoid so much
 * churn for simple action like adding poll fds to an
 * source. The alternative is to require users to
 * explicitly detach QSources before modifying them, but
 * updating poll FDs/callbacks etc is a common operation
 * for QSource/GSource callbacks so this limits functionality
 * substantially
 */
static void qsource_update(QSource *qsource)
{
    QContext *ctx = qsource->ctx;

    if (ctx) {
        qcontext_detach(ctx, qsource, NULL);
        qcontext_attach(ctx, qsource, NULL);
    }
}

static void qsource_add_poll(QSource *qsource, GPollFD *pfd)
{
    g_array_append_val(qsource->poll_fds, pfd);
    qsource_update(qsource);
}

static void qsource_remove_poll(QSource *qsource, GPollFD *pfd)
{
    bool done = false;

    while (!done) {
        done = true;
        guint i;
        for (i = 0; i < qsource->poll_fds->len; i++) {
            if (g_array_index(qsource->poll_fds, GPollFD *, i) == pfd) {
                g_array_remove_index(qsource->poll_fds, i);
                /* iterate again to make sure we get them all */
                done = false;
                break;
            }
        }
    }

    qsource_update(qsource);
}

static void qsource_set_source_funcs(QSource *qsource, QSourceFuncs funcs)
{
    qsource->source_funcs = funcs;
    qsource_update(qsource);
}

static QSourceCB qsource_get_callback_func(QSource *qsource)
{
    return qsource->callback_func;
}

static void qsource_set_callback_func(QSource *qsource, QSourceCB callback_func)
{
    qsource->callback_func = callback_func;
    qsource_update(qsource);
}

static void qsource_set_user_data(QSource *qsource, void *user_data)
{
    qsource->user_data = user_data;
    qsource_update(qsource);
}

static void *qsource_get_user_data(QSource *qsource)
{
    return qsource->user_data;
}

static void qsource_initfn(Object *obj)
{
    QSource *qsource = QSOURCE(obj);
    qsource->poll_fds = g_array_new(FALSE, FALSE, sizeof(GPollFD));
    qsource->ctx = NULL;
}

static void qsource_class_initfn(ObjectClass *class, void *data)
{
    QSourceClass *k = QSOURCE_CLASS(class);

    k->add_poll = qsource_add_poll;
    k->remove_poll = qsource_remove_poll;
    k->set_source_funcs = qsource_set_source_funcs;
    k->get_callback_func = qsource_get_callback_func;
    k->set_callback_func = qsource_set_callback_func;
    k->get_user_data = qsource_get_user_data;
    k->set_user_data = qsource_set_user_data;
}

TypeInfo qsource_info = {
    .name = TYPE_QSOURCE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(QSource),
    .class_size = sizeof(QSourceClass),
    .instance_init = qsource_initfn,
    .class_init = qsource_class_initfn,
    .interfaces = (InterfaceInfo[]) {
        { },
    },
};

static void qsource_register_types(void)
{
    type_register_static(&qsource_info);
}

type_init(qsource_register_types)

QSource *qsource_new(QSourceFuncs funcs, QSourceCB cb, const char *name, void *opaque)
{
    QSource *qsource = QSOURCE(object_new(TYPE_QSOURCE));
    QSourceClass *qsourcek = QSOURCE_GET_CLASS(qsource);

    qsource->name = g_strdup(name);

    qsourcek->set_source_funcs(qsource, funcs);
    qsourcek->set_callback_func(qsource, cb);
    qsourcek->set_user_data(qsource, opaque);

    return qsource;
}
