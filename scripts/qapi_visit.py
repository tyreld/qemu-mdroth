#
# QAPI visitor generator
#
# Copyright IBM, Corp. 2011
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#  Michael Roth    <mdroth@linux.vnet.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2.
# See the COPYING.LIB file in the top-level directory.

from ordereddict import OrderedDict
from qapi import *
import sys
import os
import getopt
import errno
import types

def generate_visit_carray_body(name, info):
    if info['array_size'][0].isdigit():
        array_size = info['array_size']
    elif info['array_size'][0] == '(' and info['array_size'][-1] == ')':
        array_size = info['array_size']
    else:
        array_size = "(*obj)->%s" % info['array_size']

    if info.has_key('array_capacity'):
        array_capacity = info['array_capacity']
    else:
        array_capacity = array_size

    # Visitors for "objects" (qidl-generated visitors for structs or
    # qapi-defined types) expect a <type>** argument so that, in some
    # cases, memory can be allocated for the object and a pointer to that
    # memory can be assigned to the location/pointer passed to the visitor
    # call.
    #
    # For other cases, "primitive" types, we don't do allocations and thus
    # expect a <type>*.
    #
    # So we use special handling for the former case, below, to get the
    # arguments right. We identify these cases by checking if the type
    # uses camel-case, which is reserved for typedef'd structs/objects as
    # per QEMU coding standards and QAPI convention.
    if camel_case(de_camel_case(info['type'][0])) == info['type'][0]:
        ret = mcgen('''

if (!error_is_set(errp)) {
    Error *%(local_err)s = NULL;
    visit_start_carray(m, (void **)obj, "%(name)s", %(array_capacity)s, sizeof(%(type)s), &%(local_err)s);
    if (!%(local_err)s) {
        int %(name)s_i;
        for (%(name)s_i = 0; %(name)s_i < %(array_size)s; %(name)s_i++) {
            %(type)s %(name)s_ptr = &(*obj)->%(name)s[%(name)s_i];
            visit_next_carray(m, &%(local_err)s);
            visit_type_%(type_short)s(m, &%(name)s_ptr, NULL, &%(local_err)s);
        }

        error_propagate(errp, %(local_err)s);
        %(local_err)s = NULL;

        /* Always call end_carry if start_carray succeeded.  */
        visit_end_carray(m, &%(local_err)s);
    }
    error_propagate(errp, %(local_err)s);
}
''',
                    name=name, type=c_type(info['type'][0]),
                    type_short=info['type'][0],
                    array_size=str(array_size),
                    array_capacity=str(array_capacity),
                    local_err=name + "_err")
    else:
        ret = mcgen('''
if (!error_is_set(errp)) {
    Error *%(local_err)s = NULL;
    visit_start_carray(m, (void **)obj, "%(name)s", %(array_capacity)s, sizeof(%(type)s), &%(local_err)s);
    if (!%(local_err)s) {
        int %(name)s_i;
        for (%(name)s_i = 0; %(name)s_i < %(array_size)s; %(name)s_i++) {
            %(type)s *%(name)s_ptr = (%(type)s *)&(*obj)->%(name)s[%(name)s_i];
            visit_next_carray(m, &%(local_err)s);
            visit_type_%(type_short)s(m, %(name)s_ptr, NULL, &%(local_err)s);
        }

        error_propagate(errp, %(local_err)s);
        %(local_err)s = NULL;

        /* Always call end_carry if start_carray succeeded.  */
        visit_end_carray(m, &%(local_err)s);
    }
    error_propagate(errp, %(local_err)s);
}
''',
                    name=name, type=c_type(info['type'][0]),
                    type_short=info['type'][0],
                    array_size=str(array_size),
                    array_capacity=str(array_capacity),
                    local_err=name + "_err")
    return ret

def generate_visit_struct_body(field_prefix, name, members):
    ret = mcgen('''
if (!error_is_set(errp)) {
''')
    push_indent()

    if len(field_prefix):
        field_prefix = field_prefix + "."
        ret += mcgen('''
Error **errp = &err; /* from outer scope */
Error *err = NULL;
visit_start_struct(m, NULL, "", "%(name)s", 0, &err);
''',
                name=name)
    else:
        ret += mcgen('''
Error *err = NULL;
visit_start_struct(m, (void **)obj, "%(name)s", name, sizeof(%(name)s), &err);
''',
                name=name)

    ret += mcgen('''
if (!err) {
    if (!obj || *obj) {
''')

    push_indent()
    push_indent()
    for argname, argentry, optional, structured, annotated in parse_args(members):
        if optional:
            ret += mcgen('''
visit_start_optional(m, obj ? &(*obj)->%(c_prefix)shas_%(c_name)s : NULL, "%(name)s", errp);
if (obj && (*obj)->%(prefix)shas_%(c_name)s) {
''',
                         c_prefix=c_var(field_prefix), prefix=field_prefix,
                         c_name=c_var(argname), name=argname)
            push_indent()

        if structured:
            ret += generate_visit_struct_body(field_prefix + argname, argname, argentry)
        else:
            if annotated:
                if isinstance(argentry['type'], types.ListType):
                    ret += generate_visit_carray_body(argname, argentry)
                elif argentry.has_key('embedded') and argentry['embedded'] == 'true':
                    tmp_ptr_name = "%s_%s_ptr" % (c_var(field_prefix).replace(".", ""), c_var(argname))
                    ret += mcgen('''
%(type)s *%(tmp_ptr)s = &(*obj)->%(c_prefix)s%(c_name)s;
visit_type_%(type)s(m, (obj && *obj) ? &%(tmp_ptr)s : NULL, "%(name)s", errp);
''',
                                 c_prefix=c_var(field_prefix), prefix=field_prefix,
                                 type=type_name(argentry['type']), c_name=c_var(argname),
                                 name=argname, tmp_ptr=tmp_ptr_name)
            else:
                ret += mcgen('''
visit_type_%(type)s(m, obj ? &(*obj)->%(c_prefix)s%(c_name)s : NULL, "%(name)s", errp);
''',
                             c_prefix=c_var(field_prefix), prefix=field_prefix,
                             type=type_name(argentry), c_name=c_var(argname),
                             name=argname)

        if optional:
            pop_indent()
            ret += mcgen('''
}
visit_end_optional(m, &err);
''')

    pop_indent()
    ret += mcgen('''

    error_propagate(errp, err);
    err = NULL;
}
''')

    pop_indent()
    pop_indent()
    ret += mcgen('''
        /* Always call end_struct if start_struct succeeded.  */
        visit_end_struct(m, &err);
    }
    error_propagate(errp, err);
}
''')
    return ret

def generate_visit_struct(name, members, static=False):
    ret_type = "void"
    if static:
        ret_type = "static " + ret_type
    ret = mcgen('''

%(ret_type)s visit_type_%(name)s(Visitor *m, %(name)s ** obj, const char *name, Error **errp)
{
''',
                name=name, ret_type=ret_type)

    push_indent()
    ret += generate_visit_struct_body("", name, members)
    pop_indent()

    ret += mcgen('''
}
''')
    return ret

def generate_visit_list(name, members, static=False):
    ret_type = "void"
    if static:
        ret_type = "static " + ret_type
    return mcgen('''

%(ret_type)s visit_type_%(name)sList(Visitor *m, %(name)sList ** obj, const char *name, Error **errp)
{
    GenericList *i, **prev = (GenericList **)obj;
    Error *err = NULL;

    if (!error_is_set(errp)) {
        visit_start_list(m, name, &err);
        if (!err) {
            for (; (i = visit_next_list(m, prev, &err)) != NULL; prev = &i) {
                %(name)sList *native_i = (%(name)sList *)i;
                visit_type_%(name)s(m, &native_i->value, NULL, &err);
            }
            error_propagate(errp, err);
            err = NULL;

            /* Always call end_list if start_list succeeded.  */
            visit_end_list(m, &err);
        }
        error_propagate(errp, err);
    }
}
''',
                name=name, ret_type=ret_type)

def generate_visit_enum(name, members, static=False):
    ret_type = "void"
    if static:
        ret_type = "static " + ret_type
    return mcgen('''

%(ret_type)s visit_type_%(name)s(Visitor *m, %(name)s * obj, const char *name, Error **errp)
{
    visit_type_enum(m, (int *)obj, %(name)s_lookup, "%(name)s", name, errp);
}
''',
                 name=name, ret_type=ret_type)

def generate_visit_union(name, members, static=False):
    ret = generate_visit_enum('%sKind' % name, members.keys())

    ret += mcgen('''

void visit_type_%(name)s(Visitor *m, %(name)s ** obj, const char *name, Error **errp)
{
    Error *err = NULL;

    if (!error_is_set(errp)) {
        visit_start_struct(m, (void **)obj, "%(name)s", name, sizeof(%(name)s), &err);
        if (!err) {
            if (obj && *obj) {
                visit_type_%(name)sKind(m, &(*obj)->kind, "type", &err);
                if (!err) {
                    switch ((*obj)->kind) {
''',
                 name=name)

    push_indent()
    push_indent()
    for key in members:
        ret += mcgen('''
            case %(abbrev)s_KIND_%(enum)s:
                visit_type_%(c_type)s(m, &(*obj)->%(c_name)s, "data", &err);
                break;
''',
                abbrev = de_camel_case(name).upper(),
                enum = c_fun(de_camel_case(key),False).upper(),
                c_type=members[key],
                c_name=c_fun(key))

    ret += mcgen('''
            default:
                abort();
            }
        }
        error_propagate(errp, err);
        err = NULL;
    }
''')
    pop_indent()
    ret += mcgen('''
        /* Always call end_struct if start_struct succeeded.  */
        visit_end_struct(m, &err);
    }
    error_propagate(errp, err);
}
''')

    pop_indent();
    ret += mcgen('''
}
''')

    return ret

def generate_declaration(name, members, genlist=True, static=False):
    ret_type = "void"
    if static:
        ret_type = "static " + ret_type
    ret = mcgen('''

%(ret_type)s visit_type_%(name)s(Visitor *m, %(name)s ** obj, const char *name, Error **errp);
''',
                name=name, ret_type=ret_type)

    if genlist:
        ret += mcgen('''
%(ret_type)s visit_type_%(name)sList(Visitor *m, %(name)sList ** obj, const char *name, Error **errp);
''',
                 name=name, ret_type=ret_type)

    return ret

def generate_enum_declaration(name, members, genlist=True):
    ret = ""
    if genlist:
        ret += mcgen('''
void visit_type_%(name)sList(Visitor *m, %(name)sList ** obj, const char *name, Error **errp);
''',
                     name=name)

    return ret

def generate_decl_enum(name, members, genlist=True, static=False):
    ret_type = "void"
    if static:
        ret_type = "static " + ret_type
    return mcgen('''

%(ret_type)s visit_type_%(name)s(Visitor *m, %(name)s * obj, const char *name, Error **errp);
''',
                name=name, ret_type=ret_type)

def main(argv=[]):
    try:
        opts, args = getopt.gnu_getopt(sys.argv[1:], "chp:o:",
                                       ["source", "header", "prefix=",
                                        "output-dir="])
    except getopt.GetoptError, err:
        print str(err)
        sys.exit(1)

    output_dir = ""
    prefix = ""
    c_file = 'qapi-visit.c'
    h_file = 'qapi-visit.h'

    do_c = False
    do_h = False

    for o, a in opts:
        if o in ("-p", "--prefix"):
            prefix = a
        elif o in ("-o", "--output-dir"):
            output_dir = a + "/"
        elif o in ("-c", "--source"):
            do_c = True
        elif o in ("-h", "--header"):
            do_h = True

    if not do_c and not do_h:
        do_c = True
        do_h = True

    c_file = output_dir + prefix + c_file
    h_file = output_dir + prefix + h_file

    try:
        os.makedirs(output_dir)
    except os.error, e:
        if e.errno != errno.EEXIST:
            raise

    def maybe_open(really, name, opt):
        if really:
            return open(name, opt)
        else:
            import StringIO
            return StringIO.StringIO()

    fdef = maybe_open(do_c, c_file, 'w')
    fdecl = maybe_open(do_h, h_file, 'w')

    fdef.write(mcgen('''
/* THIS FILE IS AUTOMATICALLY GENERATED, DO NOT MODIFY */

/*
 * schema-defined QAPI visitor functions
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "%(header)s"
''',
                 header=basename(h_file)))

    fdecl.write(mcgen('''
/* THIS FILE IS AUTOMATICALLY GENERATED, DO NOT MODIFY */

/*
 * schema-defined QAPI visitor function
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef %(guard)s
#define %(guard)s

#include "qapi/qapi-visit-core.h"
#include "%(prefix)sqapi-types.h"
''',
                  prefix=prefix, guard=guardname(h_file)))

    exprs = parse_schema(sys.stdin)

    for expr in exprs:
        if expr.has_key('type'):
            ret = generate_visit_struct(expr['type'], expr['data'])
            ret += generate_visit_list(expr['type'], expr['data'])
            fdef.write(ret)

            ret = generate_declaration(expr['type'], expr['data'])
            fdecl.write(ret)
        elif expr.has_key('union'):
            ret = generate_visit_union(expr['union'], expr['data'])
            ret += generate_visit_list(expr['union'], expr['data'])
            fdef.write(ret)

            ret = generate_decl_enum('%sKind' % expr['union'], expr['data'].keys())
            ret += generate_declaration(expr['union'], expr['data'])
            fdecl.write(ret)
        elif expr.has_key('enum'):
            ret = generate_visit_list(expr['enum'], expr['data'])
            ret += generate_visit_enum(expr['enum'], expr['data'])
            fdef.write(ret)

            ret = generate_decl_enum(expr['enum'], expr['data'])
            ret += generate_enum_declaration(expr['enum'], expr['data'])
            fdecl.write(ret)

    fdecl.write('''
#endif
    ''')

    fdecl.flush()
    fdecl.close()

    fdef.flush()
    fdef.close()

if __name__ == '__main__':
    sys.exit(main(sys.argv))