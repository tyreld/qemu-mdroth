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

#include "qidl.h"
#include "test-qidl.h"
#include "test-qidl-pub-linked.h"
#include "hw/qdev-properties.h"
#include "qapi/qmp-input-visitor.h"
#include "qapi/qmp-output-visitor.h"
#include "qapi/qapi-dealloc-visitor.h"

QIDL_ENABLE()

typedef struct TestStructLinked TestStructLinked;

QIDL_DECLARE(TestStructLinked) {
    TEST_QIDL_STRUCT_BODY
};

/* exercise generated code from annotations in objects we link against */
void test_linked_object_annotations(gconstpointer opaque)
{
    TestStructLinked *s1, *s2 = NULL;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructLinked));
    fill_test_struct(s1);

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructLinked, qmp_output_get_visitor(qov), &s1, NULL,
                    &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    free_test_struct(s1);

    s2 = g_malloc0(sizeof(TestStructLinked));
    QIDL_VISIT_TYPE(TestStructLinked, qmp_input_get_visitor(qiv), &s2, NULL,
                    &err);
    g_assert(err == NULL);
    check_test_struct(s2);

    qmp_input_visitor_cleanup(qiv);
    free_test_struct(s2);

    check_test_struct_properties(QIDL_PROPERTIES(TestStructLinked));
}

/* exercise annotations in public header files who's generated code we
 * link against (second user of generated code)
 */
void test_public_linked_object_annotations2(gconstpointer opaque)
{
    TestStructPublicLinked *s1, *s2 = NULL;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructPublicLinked));
    fill_test_struct(s1);

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructPublicLinked, qmp_output_get_visitor(qov), &s1,
                    NULL, &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    free_test_struct(s1);

    s2 = g_malloc0(sizeof(TestStructPublicLinked));
    QIDL_VISIT_TYPE(TestStructPublicLinked, qmp_input_get_visitor(qiv), &s2,
                    NULL, &err);
    g_assert(err == NULL);
    check_test_struct(s2);

    qmp_input_visitor_cleanup(qiv);
    free_test_struct(s2);

    check_test_struct_properties(QIDL_PROPERTIES(TestStructPublicLinked));
}
