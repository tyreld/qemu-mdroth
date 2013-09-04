/*
 * QEMU Guest Agent host commands implementations
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *  Michal Privoznik  <mprivozn@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <glib.h>
#include "qga/guest-agent-core.h"
#include "qga-host-qmp-commands.h"
#include "qapi/qmp/qerror.h"

bool qmp_guest_host_ping(Error **errp)
{
    g_warning("hello world!");
    return true;
}
