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
 * This work is licensed under the terms of the GNU GPLv2.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QIDL_H
#define QIDL_H

#include <glib.h>
#include "qapi/qapi-visit-core.h"
#include "qemu/object.h"
#include "hw/qdev-properties.h"

#ifdef QIDL_GEN

/* we pass the code through the preprocessor with QIDL_GEN defined to parse
 * structures as they'd appear after preprocessing, and use the following
 * definitions mostly to re-insert the initial macros/annotations so they
 * stick around for the parser to process
 */
#define QIDL(...) QIDL(__VA_ARGS__)
#define QIDL_START(name, ...) QIDL_START(name, ##__VA_ARGS__)
#define QIDL_END(name) QIDL_END(name)

#define QIDL_VISIT_TYPE(name, v, s, f, e)
#define QIDL_SCHEMA_ADD_LINK(name, obj, path, errp)
#define QIDL_PROPERTIES(name)

#else /* !QIDL_GEN */

#define QIDL(...)
#define QIDL_START(name, ...)
#define QIDL_END(name) \
    static struct { \
        void (*visitor)(Visitor *, struct name **, const char *, Error **); \
        const char *schema_json_text; \
        Object *schema_obj; \
        Property *properties; \
    } qidl_data_##name;

#define QIDL_VISIT_TYPE(name, v, s, f, e) \
    g_assert(qidl_data_##name.visitor); \
    qidl_data_##name.visitor(v, s, f, e)
#define QIDL_SCHEMA_ADD_LINK(name, obj, path, errp) \
    g_assert(qidl_data_##name.schema_obj); \
    object_property_add_link(obj, path, "container", \
                             &qidl_data_##name.schema_obj, errp)
#define QIDL_PROPERTIES(name) \
    qidl_data_##name.properties

#endif /* QIDL_GEN */

#endif
