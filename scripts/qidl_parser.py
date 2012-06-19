#
# QEMU IDL Parser
#
# Copyright IBM, Corp. 2012
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#  Michael Roth    <mdroth@linux.vnet.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2.
# See the COPYING file in the top-level directory.
#
# The lexer code is based off of:
#   http://www.lysator.liu.se/c/ANSI-C-grammar-l.html

import sys, json

class Input(object):
    def __init__(self, text):
        self.fp = text
        self.buf = text
        self.eof = False

    def pop(self):
        if len(self.buf) == 0:
            self.eof = True
            return ''
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

def lexer(fp):
    ch = fp.pop()

    while not fp.eof:
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
                           'volatile', 'while' ]:
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

def process_marker(ret, params):
    marker_type = params[0]
    if marker_type == "derived":
        ret['is_derived'] = True
    elif marker_type == 'immutable':
        ret['is_immutable'] = True
    elif marker_type == 'broken':
        ret['is_broken'] = True
    elif marker_type == 'size_is':
        ret['is_array'] = True
        ret['array_size'] = params[1]

    return ret

def parse_markers(la, index, ret):
    next = index

    while choice(la, next, 'symbol', 'QIDL'):
        params = []
        next += 1

        next, _ = expect(la, next, 'operator', '(')
        while not choice(la, next, 'operator', ')'):
            next, param = expect(la, next, 'symbol')
            params.append(param)
            if choice(la, next, 'operator', ','):
                next += 1
        next, _ = expect(la, next, 'operator', ')')

        ret = process_marker(ret, params)

    return (next - index), ret

def parse_type(la, index):
    next = index

    typename = ''
    if choice(la, next, 'const', 'const'):
        typename += 'const '
        next += 1

    if choice(la, next, 'struct', 'struct'):
        typename += 'struct '
        next += 1

    next, rest = expect(la, next, 'symbol')
    typename += rest

    ret = { 'type': typename }

    off, ret = parse_markers(la, next, ret)
    next += off

    if choice(la, next, 'operator', '*'):
        next += 1
        ret['is_pointer'] = True

    return (next - index), ret

def parse_var_decl(la, index):
    next = index

    off, ret = parse_type(la, next)
    next += off

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

    off, ret = parse_markers(la, next, ret)
    next += off

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
        offset, node = parse_var_decl(la, next)
        next += offset
        nodes.append(node)

        next, _ = expect(la, next, 'operator', ';')

    next += 1

    return (next - index), { 'struct': name, 'fields': nodes }

def parse_typedef(la, index):
    next = index

    next, _ = expect(la, next, 'typedef', 'typedef')

    offset, node = parse_struct(la, next)
    next += offset

    next, typename = expect(la, next, 'symbol')

    return (next - index), { 'typedef': typename, 'type': node }

def parse(la, index=0):
    next = index

    if choice(la, next, 'typedef'):
        offset, node = parse_typedef(la, next)
    elif choice(la, next, 'struct'):
        offset, node = parse_struct(la, next)
    else:
        raise Exception("unsupported QIDL declaration")

    next, _ = expect(la, next + offset, 'operator', ';')

    return (next - index), node

def process_declaration_params(params, declaration={}):
    declaration['id'] = params[0]
    declaration['do_state'] = False
    if "state" in params:
        declaration['do_state'] = True
    return declaration

def get_declaration_params(line):
    params = []
    for param in line.split("(")[1][:-2].split(","):
        params.append(param.strip())
    return params

def get_declarations(f):
    in_declaration = False
    declaration = {}
    while True:
        line = f.readline()
        if line == '':
            raise StopIteration()
        elif line.startswith("QIDL_START("):
            params = get_declaration_params(line)
            declaration = process_declaration_params(params, declaration)
            declaration['text'] = ""
            in_declaration = True
        elif line.startswith("QIDL_END("):
            params = get_declaration_params(line)
            if declaration['id'] != params[0]:
                raise Exception("unterminated QIDL declaration: %s" % declaration['id'])
            in_declaration = False
            yield declaration
        elif in_declaration:
            declaration['text'] += line

def parse_file(f):
    nodes = []
    for declaration in get_declarations(f):
        la = LookAhead(skip(lexer(Input(declaration['text']))))
        _, node = parse(la)
        node['id'] = declaration['id']
        node['do_state'] = declaration['do_state']
        nodes.append(node)
    return nodes

def main():
    nodes = parse_file(sys.stdin)
    print json.dumps(nodes, sort_keys=True, indent=2)

if __name__ == '__main__':
    main()
