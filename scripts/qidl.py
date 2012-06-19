#
# QIDL Code Generator
#
# Copyright IBM, Corp. 2012
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#  Michael Roth    <mdroth@linux.vnet.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2.
# See the COPYING file in the top-level directory.

from ordereddict import OrderedDict
from qidl_parser import parse_file
from qapi import parse_schema, mcgen, camel_case, de_camel_case
from qapi_visit import generate_visit_struct, push_indent, pop_indent
import sys
import json
import getopt
import os
import errno

def qapi_schema(node):
    schema = OrderedDict()
    data = OrderedDict()
    fields = None
    if node.has_key('typedef'):
        schema['type'] = node['typedef']
        fields = node['type']['fields']
    elif node.has_key('struct'):
        schema['type'] = node['struct']
        fields = node['fields']
    else:
        raise Exception("top-level neither typedef nor struct")

    for field in fields:
        if filter(lambda x: field.has_key(x), \
                ['is_derived', 'is_immutable', 'is_broken', 'is_function', 'is_nested_decl']):
            continue

        description = {}

        if field['type'].endswith('_t'):
            typename = field['type'][:-2]
        elif field['type'].startswith('struct '):
            typename = field['type'].split(" ")[1]
        elif field['type'].startswith('enum '):
            typename = 'int'
        elif field['type'] == "_Bool":
            typename = 'bool'
        elif field['type'].endswith("char") and field.has_key('is_pointer'):
            typename = 'str';
        elif field['type'] == 'int':
            typename = 'int32'
        elif field['type'] == 'unsigned int':
            typename = 'uint32'
        else:
            typename = field['type']

        if field.has_key('is_array') and field['is_array']:
            description['type'] = [typename]
            description['<annotated>'] = 'true'
            if field.has_key('array_size'):
                description['array_size'] = field['array_size']
            if field.has_key('array_capacity'):
                description['array_capacity'] = field['array_capacity']
        elif camel_case(de_camel_case(typename)) == typename and \
            (not field.has_key('is_pointer') or not field['is_pointer']):
            description['<annotated>'] = 'true'
            description['<embedded struct>'] = 'true'
            description['type'] = typename
        else:
            description = typename

        if field.has_key('is_optional') and field['is_optional']:
            data["*" + field['variable']] = description
        else:
            data[field['variable']] = description

    schema['data'] = data
    return schema


def parse_schema_file(filename):
    return parse_schema(open(filename, 'r'))

def write_file(output, filename):
    if filename:
        output_file = open(filename, 'w')
    else:
        output_file = sys.stdout
    output_file.write(output)
    if filename:
        output_file.close()

def property_list(node):
    prop_list = []
    fields = None
    if node.has_key('typedef'):
        state = node['typedef']
        fields = node['type']['fields']
    elif node.has_key('struct'):
        state = node['struct']
        fields = node['fields']
    else:
        raise Exception("top-level neither typedef nor struct")

    for field in fields:
        if not field.has_key('is_property'):
            continue

        prop = {}

        if field['type'].endswith('_t'):
            typename = field['type'][:-2]
        elif field['type'] == "_Bool":
            typename = 'bool'
        else:
            typename = field['type']

        prop['name'] = field['property_fields'][0]
        prop['state'] = state
        prop['field'] = field['variable']
        prop['name'] = field['property_fields'][0]
        prop['type'] = typename
        if len(field['property_fields']) == 2:
            prop['default'] = field['property_fields'][1]
        if field.has_key('is_pointer'):
            prop['is_pointer'] = True
        prop_list.append(prop)

    return prop_list

def generate_include(include_path):
    return mcgen('''
#include "%(include)s"
''',
                       include=include_path)

def generate_properties(type_name, prop_list=[]):
    output = ""

    for prop in prop_list:
        if prop.has_key('is_pointer'):
            if prop['type'] == "char" or prop['type'] == "const char":
                prop['type'] = "string"
            elif prop['type'] == "void":
                prop['type'] = "ptr"

        if prop.has_key("default"):
            output += mcgen('''
        DEFINE_PROP_%(type)s(%(name)s, %(state)s, %(field)s, %(default)s),
''',
                            type=prop['type'].upper(),  name=prop['name'],
                            state=prop['state'], field=prop['field'],
                            default=prop['default'])
        else:
            output += mcgen('''
        DEFINE_PROP_%(type)s(%(name)s, %(state)s, %(field)s),
''',
                            type=prop['type'].upper(),  name=prop['name'],
                            state=prop['state'], field=prop['field'])
    output += mcgen('''
        DEFINE_PROP_END_OF_LIST()
''')

    return output

def generate_qidl_registration(type_name, schema, do_state, prop_list=[]):
    schema_text = json.dumps("%s" % json.dumps(schema))
    visitor = "NULL"
    if do_state:
        visitor = "visit_type_%s" % type_name

    return mcgen('''
static char *%(type_name)s_get_schema(Object *obj, Error **errp)
{
    return g_strdup(qidl_data_%(type_name)s.schema_json_text);
}

static void %(type_name)s_register_qidl(void)
{
    static Property properties[] = {
%(properties)s
    };
    ObjectProperty *schema_link;

    qidl_data_%(type_name)s.properties = properties;
    qidl_data_%(type_name)s.visitor = %(visitor)s;
    qidl_data_%(type_name)s.schema_json_text = %(schema_text)s;

    schema_link = object_property_find(container_get(object_get_root(), "/qidl/schemas"),
                                       "%(type_name)s", NULL);
    qidl_data_%(type_name)s.schema_obj = container_get(object_get_root(), "/qidl/schemas/%(type_name)s");
    if (!schema_link) {
        object_property_add_str(qidl_data_%(type_name)s.schema_obj, "json_text",
                                %(type_name)s_get_schema, NULL, NULL);
    }
}

qidl_init(%(type_name)s_register_qidl)
''',
                 type_name=type_name, schema_text=schema_text, visitor=visitor,
                 properties=generate_properties(type_name, prop_list))

def main(argv=[]):
    try:
        opts, args = getopt.gnu_getopt(argv[1:], "o:cd:I:",
                                       ["output-filepath=", "include="])
    except getopt.GetoptError, err:
        print >> sys.stderr, err
        return 1

    output_filepath = None
    includes = []
    for o, a in opts:
        if o in ("-f", "--output-filepath"):
            output_filepath = a
        elif o in ("-I", "--include"):
            includes.append(a)

    nodes = parse_file(sys.stdin)
    if not nodes:
        return 2

    if os.path.dirname(output_filepath) != "":
        try:
            os.makedirs(os.path.dirname(output_filepath))
        except os.error, e:
            if e.errno != errno.EEXIST:
                raise
    output = ""
    for include in includes:
        output += generate_include(include)
    for node in nodes:
        do_state = False
        schema = qapi_schema(node)
        prop_list = []
        # qapi parser expects iteration to be line-by-line
        schema_text = json.dumps(schema, indent=4).replace("\"", "'").split("\n")
        expr = parse_schema(schema_text)[0]

        if node.has_key('do_state') and node['do_state']:
            do_state = True
            output += generate_visit_struct(expr['type'], expr['data'], True)
        if node.has_key('do_properties') and node['do_properties']:
            prop_list = property_list(node)

        output += generate_qidl_registration(expr['type'], schema, do_state, prop_list)

    write_file(output, output_filepath)

    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
