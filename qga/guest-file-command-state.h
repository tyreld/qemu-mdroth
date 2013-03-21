#ifndef GUEST_AGENT_COMMAND_STATE_H
#define GUEST_AGENT_COMMAND_STATE_H
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

#include "qga/guest-agent-core.h"
#include "qapi/qmp/qerror.h"

void guest_file_init(void);
int64_t guest_file_handle_add_fd(int fd, const char *mode, Error **errp);
int64_t guest_file_handle_add(void *opaque, Error **errp);
void *guest_file_handle_find(int64_t id, Error **err);
void guest_file_handle_remove(int64_t id);

#endif
