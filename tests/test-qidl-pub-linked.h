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

#ifndef TEST_QIDL_PUB_LINKED_H
#define TEST_QIDL_PUB_LINKED_H

typedef struct TestStructPublicLinked TestStructPublicLinked;

QIDL_DECLARE_PUBLIC(TestStructPublicLinked) {
    int32_t q_immutable a;
    int32_t b;
    uint32_t q_immutable c;
    uint32_t d;
    uint64_t q_immutable e;
    uint64_t q_property("f", 42) f;
    char *g;
    char q_property("h") *h;
};

#endif
