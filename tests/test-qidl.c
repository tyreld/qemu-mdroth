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
#include "test-qidl-included.h"
#include "test-qidl-linked.h"
#include "hw/qdev-properties.h"
#include "qapi/qmp-input-visitor.h"
#include "qapi/qmp-output-visitor.h"
#include "qapi/qapi-dealloc-visitor.h"

PropertyInfo qdev_prop_uint64;
PropertyInfo qdev_prop_string;

QIDL_START(TestStructMain, state, properties)
typedef struct TestStructMain {
    int32_t a QIDL(immutable);
    int32_t b;
    uint32_t c QIDL(immutable);
    uint32_t d;
    uint64_t e QIDL(immutable);
    uint64_t f QIDL(property, "f", 42);
    char *g QIDL(property, "g");
    char *h QIDL(immutable) QIDL(property, "h");
} TestStructMain;
QIDL_END(TestStructMain)

/* exercise generated code from annotations in main() object file */
static void test_main_object_annotations(gconstpointer opaque)
{
    TestStructMain *s1, *s2 = NULL;
    Property *props;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructMain));
    s1->a = 42;
    s1->b = INT32_MAX;
    s1->c = 43;
    s1->d = UINT32_MAX;
    s1->e = 44;
    s1->f = UINT64_MAX;
    s1->g = g_strdup("test string g");
    s1->h = g_strdup("test string h");

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructMain, qmp_output_get_visitor(qov), &s1, NULL, &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    g_free(s1->g);
    g_free(s1->h);
    g_free(s1);

    s2 = g_malloc0(sizeof(TestStructMain));
    QIDL_VISIT_TYPE(TestStructMain, qmp_input_get_visitor(qiv), &s2, NULL, &err);
    g_assert(err == NULL);

    g_assert_cmpint(s2->a, ==, 0);
    g_assert_cmpint(s2->b, ==, INT32_MAX);
    g_assert_cmpint(s2->c, ==, 0);
    g_assert_cmpint(s2->d, ==, UINT32_MAX);
    g_assert_cmpint(s2->e, ==, 0);
    g_assert_cmpint(s2->f, ==, UINT64_MAX);
    g_assert_cmpstr(s2->g, ==, "test string g");
    g_assert(s2->h == NULL);

    qmp_input_visitor_cleanup(qiv);
    g_free(s2->g);
    g_free(s2);

    props = QIDL_PROPERTIES(TestStructMain);
    g_assert_cmpstr(props[0].name, ==, "f");
    g_assert_cmpint(props[0].defval, ==, 42);
    g_assert_cmpstr(props[1].name, ==, "g");
    g_assert_cmpint(props[1].defval, ==, 0);
    g_assert_cmpstr(props[2].name, ==, "h");
    g_assert_cmpint(props[2].defval, ==, 0);
    g_assert(props[3].name == NULL);
}

/* exercise generated code from annotations in included header files */
static void test_header_file_annotations(gconstpointer opaque)
{
    TestStructIncluded *s1, *s2 = NULL;
    Property *props;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructIncluded));
    s1->a = 42;
    s1->b = INT32_MAX;
    s1->c = 43;
    s1->d = UINT32_MAX;
    s1->e = 44;
    s1->f = UINT64_MAX;
    s1->g = g_strdup("test string g");
    s1->h = g_strdup("test string h");

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructIncluded, qmp_output_get_visitor(qov), &s1, NULL, &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    g_free(s1->g);
    g_free(s1->h);
    g_free(s1);

    s2 = g_malloc0(sizeof(TestStructIncluded));
    QIDL_VISIT_TYPE(TestStructIncluded, qmp_input_get_visitor(qiv), &s2, NULL, &err);
    g_assert(err == NULL);

    g_assert_cmpint(s2->a, ==, 0);
    g_assert_cmpint(s2->b, ==, INT32_MAX);
    g_assert_cmpint(s2->c, ==, 0);
    g_assert_cmpint(s2->d, ==, UINT32_MAX);
    g_assert_cmpint(s2->e, ==, 0);
    g_assert_cmpint(s2->f, ==, UINT64_MAX);
    g_assert_cmpstr(s2->g, ==, "test string g");
    g_assert(s2->h == NULL);

    qmp_input_visitor_cleanup(qiv);
    g_free(s2->g);
    g_free(s2);

    props = QIDL_PROPERTIES(TestStructIncluded);
    g_assert_cmpstr(props[0].name, ==, "f");
    g_assert_cmpint(props[0].defval, ==, 42);
    g_assert_cmpstr(props[1].name, ==, "g");
    g_assert_cmpint(props[1].defval, ==, 0);
    g_assert_cmpstr(props[2].name, ==, "h");
    g_assert_cmpint(props[2].defval, ==, 0);
    g_assert(props[3].name == NULL);
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);
    module_call_init(MODULE_INIT_QIDL);

    g_test_init(&argc, &argv, NULL);

    g_test_add_data_func("/qidl/build_test/main_object_annotations", NULL,
                         test_main_object_annotations);
    g_test_add_data_func("/qidl/build_test/linked_object_annotations", NULL,
                         test_linked_object_annotations);
    g_test_add_data_func("/qidl/build_test/header_file_annotations", NULL,
                         test_header_file_annotations);

    g_test_run();

    return 0;
}
