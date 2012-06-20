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
from qapi import parse_schema
from qapi_visit import generate_visit_struct
import sys
import json
import getopt

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
        if field.has_key('is_derived') or field.has_key('is_immutable') or field.has_key('is_broken'):
            continue

        description = {}

        if field['type'].endswith('_t'):
            typename = field['type'][:-2]
        elif field['type'].startswith('struct '):
            typename = field['type'].split(" ")[1]
        else:
            typename = field['type']

        if field.has_key('is_array') and field['is_array']:
            description['type'] = [typename]
            description['<annotated>'] = 'true'
            if field.has_key('array_size'):
                description['array_size'] = field['array_size']
            if field.has_key('array_capacity'):
                description['array_capacity'] = field['array_capacity']
        else:
            description = typename

        data[field['variable']] = description

    schema['data'] = data
    return schema

def parse_schema_file(filename):
    return parse_schema(open(filename, 'r'))

def main(argv=[]):
    try:
        opts, args = getopt.gnu_getopt(argv[1:], "svpd:f:",
                                       ["state", "vmstate", "properties",
                                        "output-dir=", "output-file="])
    except getopt.GetoptError, err:
        print >> sys.stderr, err
        return 1

    output_file = None
    output_dir = ""
    do_state = False
    do_vmstate = False
    do_properties = False
    for o, a in opts:
        if o in ("-s", "--state"):
            do_state = True
        elif o in ("-v", "--vmstate"):
            do_vmstate = True
        elif o in ("-p", "--properties"):
            do_properties = True
        elif o in ("-d", "--output-dir"):
            output_dir = a
        elif o in ("-f", "--output-file"):
            output_file = a

    if not output_file:
        print >> sys.stderr, "must specify --output-file"
        return 1

    nodes = parse_file(sys.stdin)
    for node in nodes:
        if do_state:
            # create schema
            schema = qapi_schema(node)
            schema_file_path = output_dir + node['id'] + ".json"
            schema_file = open(schema_file_path, 'w')
            schema_file.write(json.dumps(schema).replace("\"", "'"))
            schema_file.close()
            # process schema and generate visitor
            expr = parse_schema_file(schema_file_path)[0]
            print generate_visit_struct(expr['type'], expr['data'])
            visit_file_path = output_dir + node['id'] + ".c"

    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
