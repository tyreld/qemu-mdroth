#
# QEMU Lexer Library
#
# Copyright IBM, Corp. 2012
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#  Michael Roth    <mdroth@linux.vnet.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2 or later.
# See the COPYING file in the top-level directory.
#
# The lexer code is based off of:
#   http://www.lysator.liu.se/c/ANSI-C-grammar-l.html

class Input(object):
    def __init__(self, fp):
        self.fp = fp
        self.line = None
        self.offset = 0
        self.is_eof = False
        self.__fill_buf()

    def __fill_buf(self):
        if not self.line and not self.is_eof:
            self.line = self.fp.readline()
            if not self.line:
                self.is_eof = True

    def peek(self):
        if self.is_eof:
            return ""
        return self.line[self.offset]

    def pop(self):
        if self.is_eof:
            return ""
        ch = self.line[self.offset]
        self.offset += 1
        if self.offset == len(self.line):
            self.offset = 0
            self.line = None
            self.__fill_buf()
        return ch

    def peek_line(self):
        return self.line

    def pop_line(self):
        line = self.line
        self.line = None
        self.offset = 0
        self.__fill_buf()
        return line

    def eof(self):
        return self.is_eof

class Lexer(object):
    def __init__(self, input, ignored_types=[]):
        self.input = input
        self.ignored_types = ignored_types
        self.current_type = None
        self.current_value = None

    def get_token(self):
        raise NotImplemented("derived classes must implement this method")

    def __ensure_token(self):
        while self.current_type == None and not self.input.eof():
            t, v = self.get_token()
            if t not in self.ignored_types:
                self.current_type = t
                self.current_value = v

    def peek(self):
        self.__ensure_token()
        return self.current_value

    def peek_line(self):
        self.__ensure_token()
        return self.input.peek_line()

    def peek_type(self):
        self.__ensure_token()
        return self.current_type

    def pop(self):
        self.__ensure_token()
        v = self.current_value
        self.current_type = None
        self.current_value = None
        return v

    def pop_line(self):
        self.__ensure_token()
        self.current_type = None
        self.current_value = None
        return self.input.pop_line()

    def pop_expected(self, type_expected=None, value_expected=None):
        self.__ensure_token()
        if self.current_type != type_expected:
            raise Exception("expected '%s', got %s %s" %
                (type_expected, self.current_type, self.current_value))
        if value_expected != None:
            if self.current_value != value_expected:
                raise Exception("expected '%s', got %s" %
                    (value_expected, self.current_value))
        return self.pop()
    
    def check_token(self, type_expected, value_expected=None):
        self.__ensure_token()
        if self.current_type != type_expected:
            return False
        if value_expected != None:
            if self.current_value != value_expected:
                return False
        return True

    def eof(self):
        self.__ensure_token()
        return self.current_type == None

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

class CLexer(Lexer):
    def __init__(self, input, ignored_types=[]):
        super(CLexer, self).__init__(input, ignored_types)

    # used internally, external users should use
    # CLexer.peek()/peek_type()/pop() instead
    def get_token(self):
        token = ''
        while not self.input.eof():
            ch = self.input.peek()

            if is_L(ch):
                token += ch
                self.input.pop()
                ch = self.input.peek()
                while is_L(ch) or is_D(ch):
                    token += ch
                    self.input.pop()
                    ch = self.input.peek()
                if token in [ 'auto', 'break', 'case', 'const', 'continue',
                               'default', 'do', 'else', 'enum', 'extern',
                               'for', 'goto', 'if', 'register', 'return',
                               'signed', 'sizeof',
                               'static', 'struct', 'typedef', 'union',
                               'unsigned', 'volatile', 'while' ]:
                    return (token, token)
                else:
                    return ('symbol', token)
            elif ch == "'":
                token += ch
                self.input.pop()
                
                ch = self.input.peek()
                if ch == '\\':
                    token += ch
                    self.input.pop()
                    token += self.input.pop()
                else:
                    token += ch
                token += self.input.pop()
                return ('literal', token)
            elif ch == '"':
                token += ch
                self.input.pop()

                ch = self.input.peek()
                while ch not in ['', '"']:
                    token += ch
                    self.input.pop()
                    if ch == '\\':
                        token += self.input.pop()
                    ch = self.input.peek()
                token += ch
                self.input.pop()
                return ('literal', token)
            elif ch in '.><+-*/%&^|!;{},:=()[]~?':
                token += ch
                self.input.pop()
                ch = self.input.peek()
                tmp_token = token + ch
                if tmp_token in ['<:']:
                    return ('operator', '[')
                elif tmp_token in [':>']:
                    return ('operator', ']')
                elif tmp_token in ['<%']:
                    return ('operator', '{')
                elif tmp_token in ['%>']:
                    return ('operator', '}')
                elif tmp_token == '//':
                    token = tmp_token
                    ch = self.input.peek()
                    while ch != '\n' and ch != '':
                        token += ch
                        self.input.pop()
                        ch = self.input.peek()
                    return ('comment', token)
                elif tmp_token == '/*':
                    token = tmp_token
                    self.input.pop()

                    ch = self.input.peek()
                    while True:
                        while ch != '*':
                            token += ch
                            self.input.pop()
                            ch = self.input.peek()
                        token += ch
                        self.input.pop()
                        ch = self.input.peek()
                        if ch == '/':
                            token += ch
                            self.input.pop()
                            break
                    return ('comment', token)
                elif tmp_token in [ '+=', '-=', '*=', '/=', '%=', '&=', '^=',
                                    '|=', '>>', '<<', '++', '--', '->', '&&',
                                    '||', '<=', '>=', '==', '!=' ]:
                    return ('operator', tmp_token)
                else:
                    return ('operator', token)
            elif ch == '0':
                token += ch
                self.input.pop()
                ch = self.input.peek()
                if ch in 'xX':
                    token += ch
                    self.input.pop()
                    ch = self.input.peek()
                    while is_H(ch):
                        token += ch
                        self.input.pop()
                        ch = self.input.peek()
                    while is_IS(ch):
                        token += ch
                        self.input.pop()
                        ch = self.input.peek()
                elif is_D(ch):
                    token += ch
                    self.input.pop()
                    ch = self.input.peek()
                    while is_D(ch):
                        token += ch
                        self.input.pop()
                        ch = self.input.peek()
                return ('literal', token)
            elif is_D(ch):
                token += ch
                self.input.pop()
                ch = self.input.peek()
                while is_D(ch):
                    token += ch
                    self.input.pop()
                    ch = self.input.peek()
                return ('literal', token)
            elif ch in ' \t\v\n\f':
                token += ch
                self.input.pop()
                ch = self.input.peek()
                while len(ch) and ch in ' \t\v\n\f':
                    token += ch
                    self.input.pop()
                    ch = self.input.peek()
                return ('whitespace', token)
            elif ch in '#':
                token += ch
                self.input.pop()
                ch = self.input.peek()
                while len(ch) and ch != '\n':
                    token += ch
                    self.input.pop()
                    ch = self.input.peek()
                return ('directive', token)
            else:
                return ('unknown', ch)
        return (None, None)
