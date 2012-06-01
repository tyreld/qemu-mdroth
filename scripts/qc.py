#!/usr/bin/python

import sys
from ordereddict import OrderedDict
from qapi import *

marker = "qc_declaration"
marked = False

class Input(object):
    def __init__(self, fp):
        self.fp = fp
        self.buf = ''
        self.eof = False

    def pop(self):
        if len(self.buf) == 0:
            if self.eof:
                return ''

            data = self.fp.read(1024)
            if data == '':
                self.eof = True
                return ''

            self.buf += data

        ch = self.buf[0]
        self.buf = self.buf[1:]
        return ch

def in_range(ch, start, end):
    if ch >= start and ch <= end:
        return True
    return False

# D			[0-9]
# L			[a-zA-Z_]
# H			[a-fA-F0-9]
# E			[Ee][+-]?{D}+
# FS			(f|F|l|L)
# IS			(u|U|l|L)*

def is_D(ch):
    return in_range(ch, '0', '9')

def is_L(ch):
    return in_range(ch, 'a', 'z') or in_range(ch, 'A', 'Z') or ch == '_'

def is_H(ch):
    return in_range(ch, 'a', 'f') or in_range(ch, 'A', 'F') or is_D(ch)

def is_FS(ch):
    return ch in 'fFlL'

def is_IS(ch):
    return ch in 'uUlL'

def find_marker(ch, fp):
    global marked

    # scan for marker before full processing

    while not marked and not fp.eof:
        token = ''
        if is_L(ch):
            token += ch
            while True:
                ch = fp.pop()
                if not is_L(ch) and not is_D(ch):
                    break
                token += ch
            if token == marker:
                marked = True
                return
        ch = fp.pop()
    return

def lexer(fp):
    global marked
    ch = fp.pop()

    while not fp.eof:
        if not marked:
            find_marker(ch, fp)
            ch = fp.pop()
        token = ''

        if is_L(ch):
            token += ch

            ch = fp.pop()
            while is_L(ch) or is_D(ch):
                token += ch
                ch = fp.pop()
            if token in [ 'auto', 'break', 'case', 'const', 'continue',
                           'default', 'do', 'else', 'enum', 'extern',
                           'for', 'goto', 'if', 'register', 'return', 'signed',
                           'sizeof',
                           'static', 'struct', 'typedef', 'union', 'unsigned',
                           'void', 'volatile', 'while' ]:
                yield (token, token)
            else:
                yield ('symbol', token)
        elif ch == "'":
            token += ch

            ch = fp.pop()
            if ch == '\\':
                token += ch
                token += fp.pop()
            else:
                token += ch
            token += fp.pop()
            ch = fp.pop()
            yield ('literal', token)
        elif ch == '"':
            token += ch

            ch = fp.pop()
            while ch not in ['', '"']:
                token += ch
                if ch == '\\':
                    token += fp.pop()
                ch = fp.pop()
            token += ch
            yield ('literal', token)
            ch = fp.pop()
        elif ch in '.><+-*/%&^|!;{},:=()[]~?':
            token += ch
            ch = fp.pop()
            tmp_token = token + ch
            if tmp_token in ['<:']:
                yield ('operator', '[')
                ch = fp.pop()
            elif tmp_token in [':>']:
                yield ('operator', ']')
                ch = fp.pop()
            elif tmp_token in ['<%']:
                yield ('operator', '{')
                ch = fp.pop()
            elif tmp_token in ['%>']:
                yield ('operator', '}')
                ch = fp.pop()
            elif tmp_token == '//':
                token = tmp_token
                ch = fp.pop()
                while ch != '\n' and ch != '':
                    token += ch
                    ch = fp.pop()
                yield ('comment', token)
            elif tmp_token == '/*':
                token = tmp_token

                ch = fp.pop()
                while True:
                    while ch != '*':
                        token += ch
                        ch = fp.pop()
                    token += ch
                    ch = fp.pop()
                    if ch == '/':
                        token += ch
                        break
                yield ('comment', token)
                ch = fp.pop()
            elif tmp_token in [ '+=', '-=', '*=', '/=', '%=', '&=', '^=',
                                '|=', '>>', '<<', '++', '--', '->', '&&',
                                '||', '<=', '>=', '==', '!=' ]:
                yield ('operator', tmp_token)
                ch = fp.pop()
            else:
                yield ('operator', token)
        elif ch == '0':
            token += ch
            ch = fp.pop()
            if ch in 'xX':
                token += ch
                ch = fp.pop()
                while is_H(ch):
                    token += ch
                    ch = fp.pop()
                while is_IS(ch):
                    token += ch
                    ch = fp.pop()
            elif is_D(ch):
                token += ch
                ch = fp.pop()
                while is_D(ch):
                    token += ch
                    ch = fp.pop()
            yield ('literal', token)
        elif is_D(ch):
            token += ch
            ch = fp.pop()
            while is_D(ch):
                token += ch
                ch = fp.pop()
            yield ('literal', token)
        elif ch in ' \t\v\n\f':
            token += ch
            ch = fp.pop()
            while len(ch) and ch in ' \t\v\n\f':
                token += ch
                ch = fp.pop()
            yield ('whitespace', token)
        elif ch in '#':
            token += ch
            ch = fp.pop()
            while len(ch) and ch != '\n':
                token += ch
                ch = fp.pop()
            yield ('directive', token)
        else:
            yield ('unknown', ch)
            ch = fp.pop()

class LookAhead(object):
    def __init__(self, container):
        self.i = container.__iter__()
        self.la = []
        self.full = False

    def at(self, i):
        if not marked:
            self.la = []
            self.full = False
        if i >= len(self.la):
            if self.full:
                raise StopIteration()
            else:
                try:
                    self.la.append(self.i.next())
                except StopIteration, e:
                    self.full = True
                    raise StopIteration()

        return self.la[i]

    def eof(self):
        try:
            self.at(len(self.la))
        except StopIteration, e:
            return True

        return False

def skip(c):
    for token, value in c:
        if token in ['whitespace', 'comment', 'directive']:
            continue
        yield (token, value)

def expect(la, index, first, second=None):
    if la.at(index)[0] != first:
        raise Exception("expected '%s', got %s %s" % (first, la.at(index)[0], la.at(index)[1]))
    if second != None:
        if la.at(index)[1] != second:
            raise Exception("expected '%s', got %s" % (second, la.at(index)[1]))
    return index + 1, la.at(index)[1]

def choice(la, index, first, second=None):
    if la.at(index)[0] != first:
        return False
    if second != None:
        if la.at(index)[1] != second:
            return False
    return True

def parse_type(la, index):
    next = index

    typename = ''
    if choice(la, next, 'struct', 'struct'):
        typename = 'struct '
        next += 1

    next, rest = expect(la, next, 'symbol')
    typename += rest

    ret = { 'type': typename }

    if choice(la, next, 'symbol', '_derived'):
        next += 1
        ret['is_derived'] = True
    elif choice(la, next, 'symbol', '_immutable'):
        next += 1
        ret['is_immutable'] = True
    elif choice(la, next, 'symbol', '_broken'):
        next += 1
        ret['is_broken'] = True
    elif choice(la, next, 'symbol', '_version'):
        next += 1

        next, _ = expect(la, next, 'operator', '(')
        next, version = expect(la, next, 'literal')
        next, _ = expect(la, next, 'operator', ')')

        ret['version'] = version
    elif choice(la, next, 'symbol', '_size_is'):
        next += 1

        next, _ = expect(la, next, 'operator', '(')
        next, array_size = expect(la, next, 'symbol')
        next, _ = expect(la, next, 'operator', ')')

        ret['is_array'] = True
        ret['array_size'] = array_size

    if choice(la, next, 'operator', '*'):
        next += 1
        ret['is_pointer'] = True

    next, variable = expect(la, next, 'symbol')
    ret['variable'] = variable

    if choice(la, next, 'operator', '['):
        next += 1

        if not ret.has_key('is_array'):
            ret['is_array'] = True
            ret['array_size'] = la.at(next)[1]
        else:
            ret['array_capacity'] = la.at(next)[1]
        next += 1

        next, _ = expect(la, next, 'operator', ']')

    if choice(la, next, 'symbol', '_default'):
        next += 1

        next, _ = expect(la, next, 'operator', '(')
        next, default = expect(la, next, 'literal')
        next, _ = expect(la, next, 'operator', ')')

        ret['default'] = default

    next, _ = expect(la, next, 'operator', ';')

    return (next - index), ret

def parse_struct(la, index):
    next = index

    next, _ = expect(la, next, 'struct', 'struct')

    name = None
    if choice(la, next, 'symbol'):
        name = la.at(next)[1]
        next += 1

    next, _ = expect(la, next, 'operator', '{')

    nodes = []

    while not choice(la, next, 'operator', '}'):
        offset, node = parse_type(la, next)
        next += offset
        nodes.append(node)

    next += 1

    return (next - index), { 'struct': name, 'fields': nodes }

def parse_typedef(la, index):
    next = index

    next, _ = expect(la, next, 'typedef', 'typedef')

    offset, node = parse_struct(la, next)
    next += offset

    next, typename = expect(la, next, 'symbol')

    return (next - index), { 'typedef': typename, 'type': node }

def qapi_format(node, is_save=True):
    if node.has_key('typedef'):
        dtype = node['typedef']
        fields = node['type']['fields']
    else:
        dtype = node['struct']
        fields = node['fields']

    if is_save:
        print 'void qc_save_%s(Visitor *v, %s *s, const char *name, Error **errp)' % (dtype, dtype)
    else:
        print 'void qc_load_%s(Visitor *v, %s *s, const char *name, Error **errp)' % (dtype, dtype)
    print '{'
    print '    visit_start_struct(v, "%s", name, errp);' % (dtype)
    for field in fields:
        if field.has_key('is_derived') or field.has_key('is_immutable') or field.has_key('is_broken'):
            continue

        if field['type'].endswith('_t'):
            typename = field['type'][:-2]
        else:
            typename = field['type']

        if field.has_key('is_array'):
            if field.has_key('array_capacity'):
                print '    if (%(array_size)s > %(array_capacity)s) {' % field
                print '        error_set(errp, QERR_FAULT, "Array size greater than capacity.");'
                print '    }'
                print '    %(array_size)s = MIN(%(array_size)s, %(array_capacity)s);' % field
            print '    visit_start_array(v, "%s", errp);' % (field['variable'])
            print '    for (size_t i = 0; i < %s; i++) {' % (field['array_size'])
            print '        visit_type_%s(v, &s->%s[i], NULL, errp);' % (typename, field['variable'])
            print '    }'
            print '    visit_end_array(v, errp);'
        elif field.has_key('default'):
            if is_save:
                print '    if (s->%s != %s) {' % (field['variable'], field['default'])
                print '        visit_type_%s(v, &s->%s, "%s", errp);' % (typename, field['variable'], field['variable'])
                print '    }'
            else:
                print '    s->%s = %s;' % (field['variable'], field['default'])
                print '    visit_type_%s(v, &s->%s, "%s", NULL);' % (typename, field['variable'], field['variable'])
        else:
            print '    visit_type_%s(v, &s->%s, "%s", errp);' % (typename, field['variable'], field['variable'])
    print '    visit_end_struct(v, errp);'
    print '}'
    print

import json

def type_dump(node):
    print json.dumps(node, sort_keys=True, indent=4)

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
            #description['type'] = typename
            description = typename

        data[field['variable']] = description

    schema['data'] = data
    print json.dumps(schema).replace("\"", "'")

def vmstate_field_hacks(node, field):
    # yes, this is gonna get hairy. may want to move to a seperate file at
    # some point
    push_indent()
    vms_field=""
    if node.has_key('typedef') and node['typedef'] == 'RTCState':
        if field['type'] == 'struct tm':
            vms_field += mcgen('''
VMSTATE_INT32(%(variable)s.tm_sec, RTCState),
VMSTATE_INT32(%(variable)s.tm_min, RTCState),
VMSTATE_INT32(%(variable)s.tm_hour, RTCState),
VMSTATE_INT32(%(variable)s.tm_wday, RTCState),
VMSTATE_INT32(%(variable)s.tm_mday, RTCState),
VMSTATE_INT32(%(variable)s.tm_mon, RTC  State),
VMSTATE_INT32(%(variable)s.tm_year, RTCState),
''',
                               variable=field['variable']).rstrip()
    pop_indent()
    return vms_field

def vmstate_fields(node):
    fields = None
    state_type = ""
    vms_primitives = ['INT8', 'INT16', 'INT32', 'INT64', 'UINT8', 'UINT16',
                      'UINT32', 'UINT64', 'BOOL', 'TIMER']
    if node.has_key('typedef'):
        fields = node['type']['fields']
        state_type = node['typedef']
    elif node.has_key('struct'):
        fields = node['fields']
        state_type = node['struct']
    else:
        raise Exception("top-level neither typedef nor struct")

    print mcgen('''
VMStateField vmstate_%s[] = {
''' % state_type.lower()).rstrip()

    for field in fields:
        if field.has_key('is_derived') or field.has_key('is_immutable') or field.has_key('is_broken'):
            continue

        vms_field_hack = vmstate_field_hacks(node, field)
        if vms_field_hack:
            print vms_field_hack
            continue

        vms_field = "VMSTATE"

        if field['type'].endswith('_t'):
            vms_type = field['type'][:-2].upper()
        elif field['type'] == 'int':
            vms_type = 'INT32'
        elif field['type'] == 'bool':
            vms_type = 'BOOL'
        elif field['type'] == 'QEMUTimer':
            vms_type = 'TIMER'
        else:
            raise Exception("unable to process field:\n%s\nfrom node:\n%s" % (field, node))

        if field.has_key('is_array'):
            if field.has_key('array_size'):
                vms_field += '_VARRAY'
            else:
                if vms_type == 'UINT8':
                    vms_field += '_BUFFER'
                else:
                    vms_field += '_ARRAY'

        if not vms_field.endswith("_BUFFER"):
            vms_field += "_" + vms_type

        if field.has_key('version'):
            vms_field += "_V"
        else:
            field['version'] = "0"

        if vms_field in map(lambda x: "VMSTATE_" + x, vms_primitives):
            vms_field += "(%s, %s)," % (field['variable'], state_type)
        elif vms_field in map(lambda x: "VMSTATE_" + x + "_V", vms_primitives):
            vms_field += "(%s, %s, %s)," % (field['variable'], state_type,
                                            field['version'])
        elif vms_field == 'VMSTATE_BUFFER':
            vms_field += "(%s, %s)," % (field['variable'], state_type)
        elif vms_field in map(lambda x: "VMSTATE_VARRAY_" + x, vms_primitives):
            args = ", ".join([field['variable'], state_type,
                             field['array_size'], field['version'],
                             "vmstate_info_" + field['type'].strip("_t"),
                             field['type']])
            vms_field += "(%s)," % args
        else:
            raise Exception("unable to process field:\n%s\nfrom node:\n%s" % (field, node))

        push_indent()
        print cgen(vms_field).rstrip()
        pop_indent()

    print mcgen('''
    VMSTATE_END_OF_LIST()
}
''',).rstrip()

if __name__ == '__main__':
    la = LookAhead(skip(lexer(Input(sys.stdin))))

    index = 0
    while True:
        try:
            if choice(la, index, 'typedef'):
                offset, node = parse_typedef(la, index)
            elif choice(la, index, 'struct'):
                offset, node = parse_struct(la, index)
            else:
                continue

            index, _ = expect(la, index + offset, 'operator', ';')
            marked = False
            index = 0
        except StopIteration, e:
            break

        #qapi_format(node, True)
        #qapi_format(node, False)
        #type_dump(node)
        if '--vmstate' in sys.argv[1:]:
            vmstate_fields(node)
        else:
            qapi_schema(node)
