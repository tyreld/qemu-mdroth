/*
 * Interfaces for tracking state associated with guest-file-* commands
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
#include "qga/guest-agent-core.h"
#include "qga/guest-file-command-state.h"
#include "qapi/qmp/qerror.h"
#include "qemu/queue.h"

typedef struct GuestFileHandle {
    uint64_t id;
    void *opaque;
    QTAILQ_ENTRY(GuestFileHandle) next;
} GuestFileHandle;

static struct {
    QTAILQ_HEAD(, GuestFileHandle) filehandles;
} guest_file_state;

void guest_file_init(void)
{
    QTAILQ_INIT(&guest_file_state.filehandles);
}

int64_t guest_file_handle_add(void *opaque, Error **errp)
{
    GuestFileHandle *gfh;
    int64_t handle;

    handle = ga_get_fd_handle(ga_state, errp);
    if (error_is_set(errp)) {
        return 0;
    }

    gfh = g_malloc0(sizeof(GuestFileHandle));
    gfh->id = handle;
    gfh->opaque = opaque;
    QTAILQ_INSERT_TAIL(&guest_file_state.filehandles, gfh, next);

    return handle;
}

void *guest_file_handle_find(int64_t id, Error **err)
{
    GuestFileHandle *gfh;

    QTAILQ_FOREACH(gfh, &guest_file_state.filehandles, next) {
        if (gfh->id == id) {
            return gfh->opaque;
        }
    }

    error_setg(err, "handle '%" PRId64 "' has not been found", id);
    return NULL;
}

void guest_file_handle_remove(int64_t id)
{
    GuestFileHandle *gfh = NULL;

    QTAILQ_FOREACH(gfh, &guest_file_state.filehandles, next) {
        if (gfh->id == id) {
            break;
        }
    }

    if (gfh) {
        QTAILQ_REMOVE(&guest_file_state.filehandles, gfh, next);
        g_free(gfh);
    }
}
