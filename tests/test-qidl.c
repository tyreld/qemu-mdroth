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
#include "test-qidl.h"
#include "test-qidl-included.h"
#include "test-qidl-pub-linked.h"
#include "hw/qdev.h"
#include "qapi/qmp-input-visitor.h"
#include "qapi/qmp-output-visitor.h"
#include "qapi/qapi-dealloc-visitor.h"

QIDL_ENABLE()

PropertyInfo qdev_prop_uint64;
PropertyInfo qdev_prop_string;

typedef struct TestStructMain TestStructMain;

QIDL_DECLARE(TestStructMain) {
    TEST_QIDL_STRUCT_BODY
};

void fill_test_struct(void *opaque)
{
    TestStructMain *s = opaque;
    s->a = 42;
    s->b = INT32_MAX;
    s->c = 43;
    s->d = UINT32_MAX;
    s->e = 44;
    s->f = UINT64_MAX;
    s->g = g_strdup("test string g");
    s->h = g_strdup("test string h");
}

void check_test_struct(void *opaque)
{
    TestStructMain *s = opaque;
    g_assert_cmpint(s->a, ==, 0);
    g_assert_cmpint(s->b, ==, INT32_MAX);
    g_assert_cmpint(s->c, ==, 0);
    g_assert_cmpint(s->d, ==, UINT32_MAX);
    g_assert_cmpint(s->e, ==, 0);
    g_assert_cmpstr(s->g, ==, "test string g");
    g_assert(s->h == NULL);
}

void free_test_struct(void *opaque)
{
    TestStructMain *s = opaque;
    g_free(s->g);
    g_free(s->h);
    g_free(s);
}

void check_test_struct_properties(const Property *props)
{
    g_assert_cmpstr(props[0].name, ==, "f");
    g_assert_cmpint(props[0].defval, ==, 42);
    g_assert_cmpstr(props[1].name, ==, "h");
    g_assert_cmpint(props[1].defval, ==, 0);
    g_assert(props[2].name == NULL);
}

/* exercise generated code from annotations in main() object file */
static void test_main_object_annotations(gconstpointer opaque)
{
    TestStructMain *s1, *s2 = NULL;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructMain));
    fill_test_struct(s1);

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructMain, qmp_output_get_visitor(qov), &s1, NULL,
                    &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    free_test_struct(s1);

    s2 = g_malloc0(sizeof(TestStructMain));
    QIDL_VISIT_TYPE(TestStructMain, qmp_input_get_visitor(qiv), &s2, NULL,
                    &err);
    g_assert(err == NULL);
    check_test_struct(s2);

    qmp_input_visitor_cleanup(qiv);
    free_test_struct(s2);

    check_test_struct_properties(QIDL_PROPERTIES(TestStructMain));
}

/* exercise generated code from annotations in included header files */
static void test_header_file_annotations(gconstpointer opaque)
{
    TestStructIncluded *s1, *s2 = NULL;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructIncluded));
    fill_test_struct(s1);

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructIncluded, qmp_output_get_visitor(qov), &s1, NULL,
                    &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    free_test_struct(s1);

    s2 = g_malloc0(sizeof(TestStructIncluded));
    QIDL_VISIT_TYPE(TestStructIncluded, qmp_input_get_visitor(qiv), &s2, NULL,
                    &err);
    g_assert(err == NULL);
    check_test_struct(s2);

    qmp_input_visitor_cleanup(qiv);
    free_test_struct(s2);

    check_test_struct_properties(QIDL_PROPERTIES(TestStructIncluded));
}

/* exercise annotations in public header files who's generated code we
 * link against
 */
static void test_public_linked_object_annotations(gconstpointer opaque)
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

typedef struct TestStructComplex TestStructComplex;

QIDL_DECLARE(TestStructComplex) {
    int8_t q_size(2) array1[4];
    size_t array2_count;
    int32_t q_size(array2_count) array2[8];
    int16_t q_size((2*3)) array3[16];
    bool has_optional_array;
    int32_t optional_array_count;
    int8_t q_optional q_size(optional_array_count) optional_array[16];
    TestStructMain struct_array[32];
    int64_t struct_array2_count;
    TestStructMain q_size(struct_array2_count) struct_array2[32];
};

static void fill_test_struct_complex(TestStructComplex *s)
{
    int i;

    for (i = 0; i < 4; i++) {
        s->array1[i] = i*2;
    }

    s->array2_count = 6;
    for (i = 0; i < s->array2_count; i++) {
        s->array2[i] = i*2;
    }

    for (i = 0; i < 6; i++) {
        s->array3[i] = i*2;
    }

    s->has_optional_array = true;
    s->optional_array_count = 15;
    for (i = 0; i < s->optional_array_count; i++) {
        s->optional_array[i] = i*2;
    }

    for (i = 0; i < 32; i++) {
        fill_test_struct(&s->struct_array[i]);
    }

    s->struct_array2_count = 31;
    for (i = 0; i < s->struct_array2_count; i++) {
        fill_test_struct(&s->struct_array2[i]);
    }
}

static void check_test_struct_complex(TestStructComplex *s)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (i < 2) {
            g_assert_cmpint(s->array1[i], ==, i*2);
        } else {
            g_assert_cmpint(s->array1[i], ==, 0);
        }
    }

    g_assert_cmpint(s->array2_count, ==, 6);
    for (i = 0; i < sizeof(s->array2)/sizeof(int32_t); i++) {
        if (i < s->array2_count) {
            g_assert_cmpint(s->array2[i], ==, i*2);
        } else {
            g_assert_cmpint(s->array2[i], ==, 0);
        }
    }

    g_assert(s->has_optional_array);
    g_assert_cmpint(s->optional_array_count, ==, 15);
    for (i = 0; i < sizeof(s->optional_array)/sizeof(int16_t); i++) {
        if (i < s->optional_array_count) {
            g_assert_cmpint(s->optional_array[i], ==, i*2);
        } else {
            g_assert_cmpint(s->optional_array[i], ==, 0);
        }
    }

    for (i = 0; i < 32; i++) {
        check_test_struct(&s->struct_array[i]);
    }

    g_assert_cmpint(s->struct_array2_count, ==, 31);
    for (i = 0; i < sizeof(s->struct_array2)/sizeof(TestStructMain); i++) {
        if (i < s->struct_array2_count) {
            check_test_struct(&s->struct_array2[i]);
        } else {
            int j;
            uint8_t *ptr = (uint8_t *)&s->struct_array2[i];
            for (j = 0; j < sizeof(TestStructMain); j++) {
                g_assert_cmpint(ptr[0], ==, 0);
            }
        }
    }
}

static void test_array_annotations(gconstpointer opaque)
{
    TestStructComplex *s1, *s2 = NULL;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructComplex));
    fill_test_struct_complex(s1);

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructComplex, qmp_output_get_visitor(qov), &s1, NULL,
                    &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    g_free(s1);

    s2 = g_malloc0(sizeof(TestStructComplex));
    QIDL_VISIT_TYPE(TestStructComplex, qmp_input_get_visitor(qiv), &s2, NULL,
                    &err);
    g_assert(err == NULL);
    check_test_struct_complex(s2);

    qmp_input_visitor_cleanup(qiv);
    g_free(s2);
}

typedef struct TestStructComplex2 TestStructComplex2;

QIDL_DECLARE(TestStructComplex2) {
    bool has_struct1;
    TestStructMain q_optional *struct1;
    TestStructMain embedded_struct1;
};

static void test_complex_annotations(gconstpointer opaque)
{
    TestStructComplex2 *s1, *s2 = NULL;
    QmpInputVisitor *qiv;
    QmpOutputVisitor *qov;
    QObject *s1_obj;
    Error *err = NULL;

    s1 = g_malloc0(sizeof(TestStructComplex));
    s1->has_struct1 = true;
    s1->struct1 = g_malloc0(sizeof(TestStructMain));
    fill_test_struct(s1->struct1);
    fill_test_struct(&s1->embedded_struct1);

    qov = qmp_output_visitor_new();
    QIDL_VISIT_TYPE(TestStructComplex2, qmp_output_get_visitor(qov), &s1, NULL,
                    &err);
    g_assert(err == NULL);

    s1_obj = qmp_output_get_qobject(qov);
    qiv = qmp_input_visitor_new(s1_obj);

    qobject_decref(s1_obj);
    qmp_output_visitor_cleanup(qov);
    free_test_struct(s1->struct1);
    g_free(s1);

    s2 = g_malloc0(sizeof(TestStructComplex2));
    QIDL_VISIT_TYPE(TestStructComplex2, qmp_input_get_visitor(qiv), &s2, NULL,
                    &err);
    g_assert(err == NULL);
    check_test_struct(s2->struct1);
    check_test_struct(&s2->embedded_struct1);

    qmp_input_visitor_cleanup(qiv);
    free_test_struct(s2->struct1);
    g_free(s2);
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
    g_test_add_data_func("/qidl/build_test/public_linked_object_annotations",
                         NULL, test_public_linked_object_annotations);
    g_test_add_data_func("/qidl/build_test/public_linked_object_annotations2",
                         NULL, test_public_linked_object_annotations2);
    g_test_add_data_func("/qidl/build_test/header_file_annotations", NULL,
                         test_header_file_annotations);
    g_test_add_data_func("/qidl/build_test/array_annotations", NULL,
                         test_array_annotations);
    g_test_add_data_func("/qidl/build_test/complex_annotations", NULL,
                         test_complex_annotations);

    g_test_run();

    return 0;
}
