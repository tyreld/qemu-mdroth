/*
 * Useful visitor type implementations for QAPI users
 * that we don't have any other logical place to stick
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Michael Roth   <mdroth@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include <time.h>
#include "qapi/qapi-visit-core.h"
#include "qapi/misc-qapi-visit.h"
#include "int128.h"

void visit_type_tm(Visitor *v, struct tm *obj, const char *name, Error **errp)
{
    visit_start_struct(v, NULL, "struct tm", name, 0, errp);
    visit_type_int32(v, &obj->tm_year, "tm_year", errp);
    visit_type_int32(v, &obj->tm_mon, "tm_mon", errp);
    visit_type_int32(v, &obj->tm_mday, "tm_mday", errp);
    visit_type_int32(v, &obj->tm_hour, "tm_hour", errp);
    visit_type_int32(v, &obj->tm_min, "tm_min", errp);
    visit_type_int32(v, &obj->tm_sec, "tm_sec", errp);
    visit_end_struct(v, errp);
}

void visit_type_Int128(Visitor *v, Int128 **obj, const char *name, Error **errp)
{
    visit_start_struct(v, NULL, "Int128", name, sizeof(Int128), errp);
    visit_type_uint64(v, &(*obj)->lo, "lo", errp);
    visit_type_int64(v, &(*obj)->hi, "hi", errp);
    visit_end_struct(v, errp);
}
