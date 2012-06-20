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

#include <glib.h>
#include <stdlib.h>
#include <stdint.h>
#include "qidl.h"

QIDL_START(TestStruct, state, vmstate, properties)
typedef struct TestStruct {
    int QIDL(one, two, three) foo;
    int bar;
    int QIDL(size_is, bar) feh;
    const char QIDL(type_of, Fox) *moo;
} TestStruct;
QIDL_END(TestStruct)

QIDL_START(TestStructTwo, state, vmstate, properties)
struct TestStructTwo {
    int QIDL(one, two, three) foo;
    int bar;
    int QIDL(size_is, bar) feh;
    const char QIDL(type_of, Fox) *moo;
};
QIDL_END(TestStructTwo)

static void test_struct(gconstpointer opaque)
{
    g_assert(true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_data_func("/qidl/serialization/basic_struct", NULL, test_struct);

    g_test_run();

    return 0;
}
