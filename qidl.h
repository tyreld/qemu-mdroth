/*
 * QEMU IDL Macros/stubs
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Michael Roth    <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPLv2.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QIDL_H
#define QIDL_H

#include "qapi/qapi-visit-core.h"

#define QIDL(...)
#define QIDL_START(name, ...)
#define QIDL_END(name, ...) \
    void visit_type_##name(Visitor *, struct name **, const char *, Error **);

#endif
