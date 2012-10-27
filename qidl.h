/*
 * QEMU IDL Macros/stubs
 *
 * See docs/qidl.txt for usage information.
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Michael Roth    <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QIDL_H
#define QIDL_H

#include <glib.h>
#include "qapi/qapi-visit-core.h"
#include "qapi/misc-qapi-visit.h"
#include "qemu/object.h"
#include "hw/qdev-properties.h"

/* must be "called" in any C files that make use of QIDL-generated code */
#define QIDL_ENABLE()

#define QIDL_DATA_DECLARE(name) \
    typedef struct QIDLData##name { \
        void (*visitor)(Visitor *, struct name **, const char *, Error **); \
        const char *schema_json_text; \
        Object *schema_obj; \
        Property *properties; \
    } QIDLData##name;

/* we pass the code through the preprocessor with QIDL_GEN defined to parse
 * structures as they'd appear after preprocessing, and use the following
 * definitions mostly to re-insert the initial macros/annotations so they
 * stick around for the parser to process
 */
#ifdef QIDL_GEN

#define QIDL(...) QIDL(__VA_ARGS__)
#define QIDL_START(name, ...) QIDL_START(name, ##__VA_ARGS__)
#define QIDL_IMMUTABLE_TYPES(...) QIDL_IMMUTABLE_TYPES(__VA_ARGS__)

#else

#define QIDL(...)
#define QIDL_IMMUTABLE_TYPES(...)
#ifdef QIDL_ENABLED
#define QIDL_START(name, ...) \
    QIDL_DATA_DECLARE(name) \
    static QIDLData##name qidl_data_##name; \
    static void visit_type_##name(Visitor *, struct name **, const char *, Error **);
#define QIDL_START_PUBLIC(name, ...) \
    QIDL_DATA_DECLARE(name) \
    extern QIDLData##name qidl_data_##name; \
    void visit_type_##name(Visitor *, struct name **, const char *, Error **);
#define QIDL_START_IMPLEMENTATION(name, ...) \
    QIDLData##name qidl_data_##name;
#else
#define QIDL_START(name,  ...)
#define QIDL_START_PUBLIC(name,  ...)
#define QIDL_START_IMPLEMENTATION(name,  ...)
#endif /* QIDL_ENABLED */

#endif /* QIDL_GEN */

/* QIDL serialization annotations/markers
 *
 * q_standard: default/standard serialization handling for this type. this marker
 *   does not need to be specified explicitly, and should in fact be left out if
 *   the default handling is sufficient. in general, the default is to serialize
 *   the field, except in case where we determine it does not need to be serialized
 *   (such as by analyzing any serialization annotations provided as part of the
 *   field's type declaration to determine if there is any guest-volatile state)
 *
 * q_immutable: state is fully restorable via device
 *   [re-]initialization/realization
 *
 * q_derived: state can be fully reconstructed from other fields (and will be,
 *   via [re-]initialization of the device or a separate hook)
 *
 * q_broken: state should (or possibly should) be saved, but isn't. mostly an aid
 *   for device developers having issues with serialization of a particular
 *   field, committed code should contain these except in special circumstances
 *
 * q_optional: <field> should only be serialized if the field by the name of
 *   has_<field> is true
 *
 * q_elsewhere: state should be serialized, but is done so elsewhere (for
 *   instance, by another device with a pointer to the same data)
 *
 * q_size(field): for static/dynamically-allocated arrays. specifies the field
 *   in the structure containing the number of elements that should be
 *   serialized. if argument is wrapped in parenthesis it is instead interpreted
 *   as an expression that should be invaluated to determine the size. annotating
 *   a field in this implies we wish to serialize it, so we also set q_standard
 *   explicitly to override any default serialization policy for a struct's fields
 */
#define q_standard QIDL(serialize, standard)
#define q_immutable QIDL(serialize, immutable)
#define q_derived QIDL(serialize, derived)
#define q_broken QIDL(serialize, broken)
#define q_optional QIDL(serialize, optional)
#define q_elsewhere QIDL(serialize, elsewhere)
#define q_size(...) q_standard QIDL(serialize, size_is, ##__VA_ARGS__)

/* QIDL property annotations/markers
 *
 * q_property(<property name> [, <default value>]): specifies that field is a
 *   qdev-style property. all properties of the struct are then accessible via
 *   QIDL_PROPERTIES(<device name>) macro. this also implies q_immutable.
 */
#define q_property(name, ...) q_immutable QIDL(property, name, ##__VA_ARGS__)

/* declare a QIDL-annotated structure, private to source file.
 * generated code will be injected into the source file's compiled
 * code
 */
#define QIDL_DECLARE(name, ...) \
    QIDL_START(name, ##__VA_ARGS__) \
    struct name

/* declare a QIDL-annotated structure, publically accessible.
 * source files that use/include this must be linked against the object
 * containing the corresponding QIDL_IMPLEMENT_PUBLIC(name, ...) for the
 * structure in question to have access to the generated code
 */
#define QIDL_DECLARE_PUBLIC(name, ...) \
    QIDL_START_PUBLIC(name, ##__VA_ARGS__) \
    struct name

/* implement the QIDL structure that QIDL_DECLARE_PUBLIC(name) will
 * reference. this should be done in a source file that
 * QIDL_DECLARE_PUBLIC(name) users will link against.
 */
#define QIDL_IMPLEMENT_PUBLIC(name, ...) \
    QIDL_START_IMPLEMENTATION(name, ##__VA_ARGS__)

#define QIDL_VISIT_TYPE(name, v, s, f, e) \
    g_assert(qidl_data_##name.visitor); \
    qidl_data_##name.visitor(v, s, NULL, e)

#define QIDL_SCHEMA_ADD_LINK(name, obj, path, errp) \
    g_assert(qidl_data_##name.schema_obj); \
    object_property_add_link(obj, path, "container", \
                             &qidl_data_##name.schema_obj, errp)

#define QIDL_PROPERTIES(name) \
    qidl_data_##name.properties

QIDL_IMMUTABLE_TYPES(Notifier, MemoryRegion, MemoryRegionIoeventfd)

#endif
