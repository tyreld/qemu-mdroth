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
#include "test-qidl.h"

typedef struct TestStructIncluded TestStructIncluded;

QIDL_DECLARE(TestStructIncluded) {
    TEST_QIDL_STRUCT_BODY
};

#endif