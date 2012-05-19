/*
 * Useful visitor type declarations for QAPI users
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
#include "int128.h"

void visit_type_tm(Visitor *m, struct tm *obj, const char *name, Error **errp);
void visit_type_Int128(Visitor *v, Int128 **obj, const char *name, Error **errp);
