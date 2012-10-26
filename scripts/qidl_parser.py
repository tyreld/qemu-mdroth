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

def process_annotation(node, params, set_use_default_tag=False):
    annotation_group = params[0]
    if annotation_group == 'serialize':
        annotation_type = params[1]
        if annotation_type != 'size_is' and set_use_default_tag:
            node['use_default_tag'] = False

        if annotation_type == 'derived':
            node['is_derived'] = True
        elif annotation_type == 'immutable':
            node['is_immutable'] = True
        elif annotation_type == 'elsewhere':
            node['is_elsewhere'] = True
        elif annotation_type == 'broken':
            node['is_broken'] = True
        elif annotation_type == 'size_is':
            node['is_array'] = True
            expression = params[2]
            if expression.isdigit():
                expression = int(expression)
            node['array_size'] = expression
        elif annotation_type == 'optional':
            node['is_optional'] = True
    elif annotation_group == 'property':
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

def parse_modifiers(l, node):
    modifiers = ['const', 'volatile']
    while True:
        if l.peek_type() in modifiers:
            if l.check_token('const', 'const'):
                node['is_const'] = True
            l.pop()
        else:
            break

def parse_type(l):
    node = {}
    node['use_default_tag'] = True
    unsigned_types = ['char', 'short', 'int', 'long']
    type_complete = False
    typename = ''

    parse_modifiers(l, node)

    if l.check_token('struct', 'struct') or l.check_token('union', 'union'):
        if l.check_token('union', 'union'):
            node['is_union'] = True
        typename += l.pop() + ' '
        if not l.check_token('operator', '{'):
            typename += l.pop_expected('symbol')
            type_complete = True

    if l.check_token('union', 'union'):
        typename += l.pop() + ' '
        if not l.check_token('operator', '{'):
            typename += l.pop_expected('symbol')
            type_complete = True

    if l.check_token('unsigned', 'unsigned'):
        typename += l.pop()
        if filter(lambda t: l.check_token('symbol', t), unsigned_types):
            typename += ' '
        else:
            typename += ' int'
            type_complete = True

    if l.check_token('enum', 'enum'):
        typename += l.pop() + ' '

    # we don't currently handle embedded struct/union declarations, skip them
    # for now
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
        elif type_complete == False:
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
        node['use_default_tag'] = True

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
    elif l.check_token('operator', ';'):
        if node.has_key('is_union') and node['is_union']:
            variable = None
        else:
            raise Exception("%s: unnamed, non-union struct member", l)
    elif l.check_token('operator', '*'):
        node['is_pointer'] = True
        l.pop()
        while l.check_token('operator', '*'):
            if node.has_key('pointer_nesting'):
                node['pointer_nesting'] += 1
            else:
                node['pointer_nested'] = 1
            l.pop()
        variable = l.pop_expected('symbol')
    else:
        variable = l.pop_expected('symbol')
    node['variable'] = variable

    dim_expressions = []
    is_array = False
    while l.check_token('operator', '['):
        is_array = True
        l.pop()
        dim_expression = ''
        while not l.check_token('operator', ']'):
            dim_expression += l.pop()
        l.pop_expected('operator', ']')
        dim_expressions.append(dim_expression)

    if is_array:
        if not node.has_key('is_array'):
            node['is_array'] = True
            for i in range(0, len(dim_expressions)):
                if dim_expressions[i].isdigit():
                    dim_expressions[i] = int(dim_expressions[i])
            if len(dim_expressions) == 1:
                node['array_size'] = dim_expressions[0]
            else:
                node['array_size'] = dim_expressions
        else:
            for i in range(0, len(dim_expressions)):
                if dim_expressions[i].isdigit():
                    dim_expressions[i] = int(dim_expressions[i])
            if len(dim_expressions) == 1:
                node['array_capacity'] = dim_expressions[0]
            else:
                node['array_capacity'] = dim_expressions

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

def get_default_tag(params):
    default_tag = "standard"
    try:
        index = params.index('serialize')
        default_tag = params[index + 1]
    except Exception:
        pass
    finally:
        return default_tag

def parse_declaration_params(l):
    declaration_info = {}
    params = []
    arg_string = ""
    parens = 0
    implement = True
    is_public = False
    if l.check_token('symbol', 'QIDL_START_IMPLEMENTATION'):
        l.pop()
    elif l.check_token('symbol', 'QIDL_START_PUBLIC'):
        implement = False
        is_public = True
        l.pop()
    else:
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
        raise Exception("%s: unmatched parenthesis in QIDL macro", l)

    declaration_info['id'] = params[0]
    declaration_info['do_state'] = True
    declaration_info['do_properties'] = True
    declaration_info['implement'] = implement
    declaration_info['public'] = is_public
    if "skip_state" in params:
        declaration_info['do_state'] = False
    if "skip_properties" in params:
        declaration_info['do_properties'] = False
    declaration_info['default_tag'] = get_default_tag(params)

    return declaration_info

def immutable_by_default(field):
    immutable = False
    if field['type'] == 'void' and field.has_key('is_pointer') \
            and field['is_pointer']:
        immutable = True
    elif field.has_key('is_const') and field['is_const']:
        immutable = True
    elif field.has_key('is_function') and field['is_function']:
        immutable = True
    return immutable

def parse_declaration(l):
    declaration_info = parse_declaration_params(l)

    if l.check_token('typedef'):
        node = parse_typedef(l)
    elif l.check_token('struct'):
        node = parse_struct(l)
    else:
        raise Exception("%s: unsupported QIDL declaration", l)

    default_tag = declaration_info['default_tag']
    for field in node['fields']:
        if field['use_default_tag'] == True:
            if immutable_by_default(field):
                process_annotation(field, ['serialize', 'immutable'], False)
            else:
                process_annotation(field, ['serialize', default_tag], False)

    l.pop_expected('operator', ';')
    node['id'] = declaration_info['id']
    node['do_state'] = declaration_info['do_state']
    node['do_properties'] = declaration_info['do_properties']
    node['implement'] = declaration_info['implement']
    node['public'] = declaration_info['public']

    return node

def find_node(nodes, name):
    for node in nodes:
        if node['id'] == name:
            return node
    return None

def is_whitelisted(field, nodes=[], existing_visitors=None):
    supported_native_types=['bool', '_Bool', 'unsigned', 'short', 'int',
                            'long', 'float', 'double' ]
    supported_pointer_types=['char']
    if not (field.has_key('use_default_tag') and field['use_default_tag']):
        return True
    if find_node(nodes, field['type']):
        return True
    if existing_visitors:
        if field['type'].rsplit('_t')[0] in existing_visitors:
            return True
    if field.has_key('is_pointer') and field['is_pointer']:
        if field['type'] in supported_pointer_types:
            return True
    else:
        if field['type'] in supported_native_types:
            return True
    return False

# For fields for which an explicit serialization tag was not
# provided, we attempt to serialize them only if one of the
# following conditions hold:
# a) a visitor function visit_type_<name> has already been
#    implemented and is accessible to the the compilation
#    unit (i.e. we've pulled in a declaration for such a
#    function via a header file or declared it somewhere
#    locally
# b) we've QIDL_DECLARE()'d the type in question, and that
#    declaration is visible to the compilation unit
# c) the field is for a primitive type that we know how to
#    serialize unambiguously
#
# If any of these conditions do not hold, we will mark the
# field as q_broken so that we can whine about them elsewhere
# and address them at some point in the future.
def whitelist_process(nodes, existing_visitors=None):
    for node in nodes:
        typename = None
        fields = []
        if node.has_key('typedef'):
            typename = node['typedef']
            fields = node['type']['fields']
        elif node.has_key('struct'):
            typename = node['struct']
            fields = node['fields']
        else:
            raise Exception("top-level neither typedef nor struct")
        for field in fields:
            if not is_whitelisted(field, nodes, existing_visitors):
                field['is_broken'] = True

def parse_file(f):
    nodes = []
    filtered_tokens = ['whitespace', 'comment', 'directive']
    existing_visitors = set()
    l = CLexer(Input(f), filtered_tokens)
    while not l.eof():
        line = l.peek_line()
        if line.startswith("QIDL_START_IMPLEMENTATION("):
            info = parse_declaration_params(l)
            node = find_node(nodes, info['id'])
            node['implement'] = info['implement']
            node['do_state'] = info['do_state']
            node['do_properties'] = info['do_properties']
        elif line.startswith("QIDL_START"):
            node = parse_declaration(l)
            nodes.append(node)
        elif 'visit_type_' in line:
            words = line.split()
            for word in words:
                if 'visit_type_' in word:
                    typename = word.split('_')[-1].split('(')[0]
                    existing_visitors.add(typename)
            l.pop_line()
        else:
            l.pop_line()
    whitelist_process(nodes, existing_visitors)
    return nodes

def main():
    nodes = parse_file(sys.stdin)
    print json.dumps(nodes, sort_keys=True, indent=2)

if __name__ == '__main__':
    main()
