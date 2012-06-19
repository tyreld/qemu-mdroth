#
# QEMU IDL Parser
#
# Copyright IBM, Corp. 2012
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#  Michael Roth    <mdroth@linux.vnet.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2 or later.
# See the COPYING file in the top-level directory.

import sys, json
from lexer import Input, CLexer

def process_annotation(node, params):
    annotation_type = params[0]
    if annotation_type == "derived":
        node['is_derived'] = True
    elif annotation_type == 'immutable':
        node['is_immutable'] = True
    elif annotation_type == 'elsewhere':
        node['is_elsewhere'] = True
    elif annotation_type == 'broken':
        node['is_broken'] = True
    elif annotation_type == 'size_is':
        node['is_array'] = True
        node['array_size'] = params[1]
    elif annotation_type == 'optional':
        node['is_optional'] = True
    elif annotation_type == 'property':
        node['is_property'] = True
        if node.has_key('property_fields'):
            node['property_fields'].append(params[1:])
        else:
            node['property_fields'] = [params[1:]]

    return node

def parse_annotations(l, node):
    while l.check_token('symbol', 'QIDL'):
        params = []
        l.pop()

        l.pop_expected('operator', '(')
        open_parens = 1
        param = ""
        while open_parens:
            if l.check_token('operator', ','):
                params.append(param)
                param = ""
                l.pop()
                continue

            if l.check_token('operator', '('):
                open_parens += 1
            elif l.check_token('operator', ')'):
                open_parens -= 1

            if open_parens > 0:
                param += l.peek()

            l.pop()

        if param != "":
            params.append(param)

        node = process_annotation(node, params)

    return node

def parse_type(l):
    node = {}

    typename = ''
    if l.check_token('const', 'const'):
        typename += l.pop() + ' '

    if l.check_token('struct', 'struct'):
        typename += l.pop() + ' '

    if l.check_token('unsigned', 'unsigned'):
        typename += l.pop() + ' '

    if l.check_token('union', 'union'):
        typename += l.pop() + ' '

    if l.check_token('enum', 'enum'):
        typename += l.pop() + ' '

    # we don't currently handle embedded struct declarations, skip them for now
    if l.check_token('operator', '{'):
        open_braces = 1
        while open_braces:
            l.pop()
            if l.check_token('operator', '{'):
                open_braces += 1
            elif l.check_token('operator', '}'):
                open_braces -= 1
        l.pop()
        typename += "<anon>"
        node['is_nested_decl'] = True
    else:
        if l.check_token('operator', '*'):
            l.pop()
            node['is_pointer'] = True
        else:
            typename += l.pop_expected('symbol')

    node['type'] = typename

    node = parse_annotations(l, node)

    if l.check_token('operator', '*'):
        l.pop()
        node['is_pointer'] = True

    return node

def parse_var_decl(l, repeating_type=None):
    if repeating_type == None:
        node = parse_type(l)
    else:
        node = { 'type': repeating_type }

    if l.check_token('operator', '('):
        node['is_function'] = True
        l.pop()
        l.pop_expected('operator', '*')
        variable = l.pop_expected('symbol')
        l.pop_expected('operator', ')')

        # skip the param list since we don't use it currently
        l.pop_expected('operator', '(')
        open_parens = 1
        while open_parens:
            if l.check_token('operator', '('):
                open_parens += 1
            elif l.check_token('operator', ')'):
                open_parens -= 1
            l.pop()
    else:
        variable = l.pop_expected('symbol')
    node['variable'] = variable

    if l.check_token('operator', '['):
        l.pop()
        expression = ""
        while not l.check_token('operator', ']'):
            expression += l.pop()
        l.pop_expected('operator', ']')

        if not node.has_key('is_array'):
            node['is_array'] = True
            node['array_size'] = expression
        else:
            node['array_capacity'] = expression

    node = parse_annotations(l, node)

    return node

def parse_struct(l):
    l.pop_expected('struct', 'struct')

    name = None
    if l.check_token('symbol'):
        name = l.pop()

    l.pop_expected('operator', '{')

    nodes = []

    while not l.check_token('operator', '}'):
        node = parse_var_decl(l)
        nodes.append(node)
        while l.check_token('operator', ','):
            l.pop()
            node = parse_var_decl(l, node['type'])
            nodes.append(node)

        l.pop_expected('operator', ';')

    l.pop()

    return { 'struct': name, 'fields': nodes }

def parse_typedef(l):
    l.pop_expected('typedef', 'typedef')

    node = parse_struct(l)
    typename = l.pop_expected('symbol')

    return { 'typedef': typename, 'type': node }

def parse_declaration_params(l):
    declaration_info = {}
    params = []
    arg_string = ""
    parens = 0
    l.pop_expected('symbol', 'QIDL_START')
    while not l.eof():
        if l.check_token('operator', '('):
            parens += 1
        elif l.check_token('operator', ')'):
            parens -= 1
            if parens == 0:
                break
        elif parens > 0:
            if not l.check_token('operator', ','):
                params.append(l.peek())
        l.pop()
    l.pop_expected('operator', ')')
    if parens != 0:
        raise Exception("unmatched parenthesis in QIDL macro")

    declaration_info['id'] = params[0]
    declaration_info['do_state'] = True
    declaration_info['do_properties'] = True
    if "skip_state" in params:
        declaration_info['do_state'] = False
    if "skip_properties" in params:
        declaration_info['do_properties'] = False

    return declaration_info

def parse_declaration(l):
    declaration_info = parse_declaration_params(l)

    if l.check_token('typedef'):
        node = parse_typedef(l)
    elif l.check_token('struct'):
        node = parse_struct(l)
    else:
        raise Exception("unsupported QIDL declaration")

    l.pop_expected('operator', ';')
    node['id'] = declaration_info['id']
    node['do_state'] = declaration_info['do_state']
    node['do_properties'] = declaration_info['do_properties']

    return node

def parse_file(f):
    nodes = []
    filtered_tokens = ['whitespace', 'comment', 'directive']
    l = CLexer(Input(f), filtered_tokens)
    while not l.eof():
        line = l.peek_line()
        if line.startswith("QIDL_START("):
            node = parse_declaration(l)
            nodes.append(node)
        else:
            l.pop_line()
    return nodes

def main():
    nodes = parse_file(sys.stdin)
    print json.dumps(nodes, sort_keys=True, indent=2)

if __name__ == '__main__':
    main()
