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

#ifndef TEST_QIDL_H
#define TEST_QIDL_H

#include "qidl.h"

#define TEST_QIDL_STRUCT_BODY               \
    int32_t q_immutable a;                  \
    int32_t b;                              \
    uint32_t q_immutable c;                 \
    uint32_t d;                             \
    uint64_t q_immutable e;                 \
    uint64_t q_property("f", 42) f;         \
    char q_property("g") *g;                \
    char q_property("h") q_immutable *h;

void fill_test_struct(void *opaque);
void check_test_struct(void *opaque);
void free_test_struct(void *opaque);
void check_test_struct_properties(const Property *props);

void test_linked_object_annotations(gconstpointer opaque);
void test_public_linked_object_annotations2(gconstpointer opaque);

#endif
