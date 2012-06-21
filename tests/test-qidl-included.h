/*
 * Unit-tests for QIDL-generated visitors/code
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Michael Roth <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TEST_QIDL_INCLUDED_H
#define TEST_QIDL_INCLUDED_H

#include "qidl.h"

QIDL_START(TestStructIncluded, state, properties)
typedef struct TestStructIncluded {
    int32_t a QIDL(immutable);
    int32_t b;
    uint32_t c QIDL(immutable);
    uint32_t d;
    uint64_t e QIDL(immutable);
    uint64_t f QIDL(property, "f", 42);
    char *g QIDL(property, "g");
    char *h QIDL(immutable) QIDL(property, "h");
} TestStructIncluded;
QIDL_END(TestStructIncluded)

#endif
