/*
 * WaitObject - event loop and registration functions w32 event handlers
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/main-loop.h"

typedef struct WaitObjectHandler {
    GSource source;
    WaitObjectFunc *cb;
    GPollFD pfd;
    void *opaque;
} WaitObjectHandler;

static gboolean wait_object_handler_prepare(GSource *source, gint *timeout)
{
    return false;
}

static gboolean wait_object_handler_check(GSource *source)
{
    WaitObjectHandler *handler = (WaitObjectHandler *)source;

    return !!handler->pfd.revents;
}

static gboolean wait_object_handler_dispatch(GSource *source, GSourceFunc cb,
                                        gpointer user_data)
{
    WaitObjectHandler *handler = (WaitObjectHandler *)source;

    if (handler->cb) {
        handler->cb(handler->opaque);
    }

    return true;
}

static void wait_object_handler_finalize(GSource *source)
{
    WaitObjectHandler *handler = (WaitObjectHandler *)source;

    g_source_remove_poll(source, &handler->pfd);
}

static GSourceFuncs wait_object_handler_funcs = {
    wait_object_handler_prepare,
    wait_object_handler_check,
    wait_object_handler_dispatch,
    wait_object_handler_finalize
};

int qemu_add_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    GSource *source;
    WaitObjectHandler *handler;
    GMainContext *ctx = g_main_context_default();

   source = g_main_context_find_source_by_funcs_user_data(
                ctx, &wait_object_handler_funcs, (gpointer)handle);
   handler = (WaitObjectHandler *)source;

   g_assert(!source);

    source = g_source_new(&wait_object_handler_funcs,
                          sizeof(WaitObjectHandler));
    handler = (WaitObjectHandler *)source;
    handler->cb = func;
    /* GLib's poll function maps all HANDLE events to G_IO_IN */
    handler->pfd.fd = (int)handle;
    handler->pfd.events = G_IO_IN;
    handler->opaque = opaque;

    g_source_attach(source, ctx);
    g_source_set_callback(source, NULL, (gpointer)handle, NULL);
    g_source_add_poll(source, &handler->pfd);

    return 0;
}

void qemu_del_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    GSource *source;
    GMainContext *ctx = g_main_context_default();

    source = g_main_context_find_source_by_funcs_user_data(
                 ctx, &wait_object_handler_funcs, (gpointer)handle);

    if (!source) {
        return;
    }

    g_source_destroy(source);
}
