#!/usr/bin/env python3
"""Writes a Windows Runtime metadata file (.winmd) from a description in the format of native/guest/raven-winmd/windows.types.

The file is an ECMA-335 metadata module in a PE image without code, in the shape the Windows Runtime gives its metadata: every type flagged
as a Windows Runtime type, interfaces and delegates carrying their identifiers, runtime classes declaring the members of their interfaces,
and a version string that names the Windows Runtime.

Usage: generate-winmd.py DESCRIPTION OUTPUT
"""
import re
import struct
import sys
import uuid

PRIMITIVES = {'Void': 0x01, 'Boolean': 0x02, 'Char16': 0x03, 'Int8': 0x04, 'UInt8': 0x05, 'Int16': 0x06, 'UInt16': 0x07, 'Int32': 0x08,
              'UInt32': 0x09, 'Int64': 0x0a, 'UInt64': 0x0b, 'Single': 0x0c, 'Double': 0x0d, 'String': 0x0e, 'Object': 0x1c}

# Tables, by number.
MODULE, TYPEREF, TYPEDEF, FIELD, METHODDEF, PARAM, INTERFACEIMPL, MEMBERREF, CONSTANT, CUSTOMATTRIBUTE = 0, 1, 2, 4, 6, 8, 9, 10, 11, 12
PROPERTYMAP, PROPERTY, METHODSEMANTICS, METHODIMPL, TYPESPEC, ASSEMBLY, ASSEMBLYREF, GENERICPARAM = 0x15, 0x17, 0x18, 0x19, 0x1b, 0x20, 0x23, 0x2a

CODED = {
    'TypeDefOrRef': (2, [TYPEDEF, TYPEREF, TYPESPEC]),
    'HasConstant': (2, [FIELD, PARAM, PROPERTY]),
    'HasCustomAttribute': (5, [METHODDEF, FIELD, TYPEREF, TYPEDEF, PARAM, INTERFACEIMPL, MEMBERREF, MODULE, None, PROPERTY, None, None, None,
                               TYPESPEC, ASSEMBLY, ASSEMBLYREF, None, None, None, GENERICPARAM, None, None]),
    'CustomAttributeType': (3, [None, None, METHODDEF, MEMBERREF, None]),
    'HasSemantics': (1, [None, PROPERTY]),
    'MethodDefOrRef': (1, [METHODDEF, MEMBERREF]),
    'MemberRefParent': (3, [TYPEDEF, TYPEREF, None, METHODDEF, TYPESPEC]),
    'ResolutionScope': (2, [MODULE, None, ASSEMBLYREF, TYPEREF]),
    'TypeOrMethodDef': (1, [TYPEDEF, METHODDEF]),
}

# Row layouts: 'u1', 'u2' and 'u4' are fixed widths; 'S', 'G' and 'B' heap indexes; a table number an index into it; a name a coded index.
SCHEMA = {
    MODULE: ['u2', 'S', 'G', 'G', 'G'],
    TYPEREF: ['ResolutionScope', 'S', 'S'],
    TYPEDEF: ['u4', 'S', 'S', 'TypeDefOrRef', FIELD, METHODDEF],
    FIELD: ['u2', 'S', 'B'],
    METHODDEF: ['u4', 'u2', 'u2', 'S', 'B', PARAM],
    PARAM: ['u2', 'u2', 'S'],
    INTERFACEIMPL: [TYPEDEF, 'TypeDefOrRef'],
    MEMBERREF: ['MemberRefParent', 'S', 'B'],
    CONSTANT: ['u1', 'u1', 'HasConstant', 'B'],
    CUSTOMATTRIBUTE: ['HasCustomAttribute', 'CustomAttributeType', 'B'],
    PROPERTYMAP: [TYPEDEF, PROPERTY],
    PROPERTY: ['u2', 'S', 'B'],
    METHODSEMANTICS: ['u2', METHODDEF, 'HasSemantics'],
    METHODIMPL: [TYPEDEF, 'MethodDefOrRef', 'MethodDefOrRef'],
    TYPESPEC: ['B'],
    ASSEMBLY: ['u4', 'u2', 'u2', 'u2', 'u2', 'u4', 'B', 'S', 'S'],
    ASSEMBLYREF: ['u2', 'u2', 'u2', 'u2', 'u4', 'B', 'S', 'S', 'B'],
    GENERICPARAM: ['u2', 'u2', 'TypeOrMethodDef', 'S'],
}

# Tables whose rows must be sorted, with the column they are sorted by; a stable sort keeps the order within one key.
SORTED = {INTERFACEIMPL: 0, CONSTANT: 2, CUSTOMATTRIBUTE: 0, METHODSEMANTICS: 2, METHODIMPL: 0, GENERICPARAM: 2}

TD_PUBLIC, TD_SEQUENTIAL, TD_INTERFACE, TD_ABSTRACT, TD_SEALED, TD_WINDOWS_RUNTIME = 0x1, 0x8, 0x20, 0x80, 0x100, 0x4000
MD_PUBLIC, MD_FINAL, MD_STATIC, MD_VIRTUAL, MD_HIDE_BY_SIG, MD_NEW_SLOT, MD_ABSTRACT, MD_SPECIAL, MD_RT_SPECIAL = (
    0x6, 0x20, 0x10, 0x40, 0x80, 0x100, 0x400, 0x800, 0x1000)
IMPL_RUNTIME = 0x3


def fail(line, message):
    sys.exit(f'generate-winmd: line {line}: {message}')


def compressed(value):
    if value < 0x80:
        return bytes([value])
    if value < 0x4000:
        return struct.pack('>H', value | 0x8000)
    return struct.pack('>I', value | 0xc0000000)


class TypeName:
    """A type as written: a name with generic arguments, or an array of one."""

    def __init__(self, name, arguments=(), array=False):
        self.name, self.arguments, self.array = name, list(arguments), array


def parse_type(text, at=0):
    match = re.compile(r'\s*([A-Za-z_][\w.]*(?:`\d+)?)').match(text, at)
    if not match:
        raise ValueError(f'no type at "{text[at:]}"')
    name, at = match.group(1), match.end()
    arguments = []
    if at < len(text) and text[at] == '<':
        at += 1
        while True:
            argument, at = parse_type(text, at)
            arguments.append(argument)
            while text[at] == ' ':
                at += 1
            if text[at] == '>':
                at += 1
                break
            if text[at] != ',':
                raise ValueError(f'expected , or > in "{text}"')
            at += 1
    result = TypeName(name, arguments)
    if text.startswith('[]', at):
        result, at = TypeName(None, [result], True), at + 2
    return result, at


class Member:
    def __init__(self, kind, name, returns, parameters, unique=None):
        self.kind, self.name, self.returns, self.parameters, self.unique = kind, name, returns, parameters, unique


class Definition:
    def __init__(self, kind, namespace, name, line):
        self.kind, self.namespace, self.name, self.line = kind, namespace, name, line
        self.generics, self.guid, self.requires, self.members, self.fields = [], None, [], [], []
        self.default, self.implements, self.statics = None, [], None
        self.row = 0

    @property
    def full(self):
        return f'{self.namespace}.{self.name}'


def parse_signature(text, line):
    """'Type Name(Type name, out Type name, fill Type[] name)' as the return type, the name and the parameters."""
    match = re.fullmatch(r'(.+?)\s+(\w+)\((.*)\)', text.strip())
    if not match:
        fail(line, f'not a method: {text}')
    returns, _ = parse_type(match.group(1))
    parameters = []
    if match.group(3).strip():
        depth, start, parts = 0, 0, []
        body = match.group(3)
        for index, character in enumerate(body):
            depth += character == '<'
            depth -= character == '>'
            if character == ',' and not depth:
                parts.append(body[start:index])
                start = index + 1
        parts.append(body[start:])
        for part in parts:
            part = part.strip()
            direction = 'in'
            for word in ('out', 'fill'):
                if part.startswith(word + ' '):
                    direction, part = word, part[len(word) + 1:]
            kind, at = parse_type(part)
            parameters.append((direction, kind, part[at:].strip()))
    return returns, match.group(2), parameters


def parse(path):
    definitions, namespace, current = [], None, None
    for number, raw in enumerate(open(path, encoding='utf-8'), 1):
        text = raw.split('#', 1)[0].rstrip()
        if not text.strip():
            continue
        if raw[0] == '\t':
            if not current or current.kind != 'interface':
                fail(number, 'a member outside an interface')
            text = text.strip()
            unique = None
            if text.startswith('overload '):
                unique, text = text.split(' ', 2)[1], text.split(' ', 2)[2]
            if text.startswith(('get ', 'set ')):
                kind, rest = text.split(' ', 1)
                returns, at = parse_type(rest)
                current.members.append(Member(kind, rest[at:].strip(), returns, []))
            else:
                returns, name, parameters = parse_signature(text, number)
                current.members.append(Member('method', name, returns, parameters, unique))
            continue
        words = text.split()
        keyword = words[0]
        if keyword == 'namespace':
            namespace = words[1]
            continue
        current = None
        if keyword == 'struct':
            match = re.fullmatch(r'struct (\w+) \{(.*)\}', text)
            definition = Definition('struct', namespace, match.group(1), number)
            for field in match.group(2).split(','):
                kind, at = parse_type(field.strip())
                definition.fields.append((field.strip()[at:].strip(), kind))
        elif keyword == 'enum':
            match = re.fullmatch(r'enum (\w+) \{(.*)\}', text)
            definition = Definition('enum', namespace, match.group(1), number)
            for value in match.group(2).split(','):
                name, number_text = value.split('=')
                definition.fields.append((name.strip(), int(number_text, 0)))
        elif keyword == 'attribute':
            match = re.fullmatch(r'attribute (\w+)\((.*)\)', text)
            definition = Definition('attribute', namespace, match.group(1), number)
            if match.group(2).strip():
                for parameter in match.group(2).split(','):
                    kind, at = parse_type(parameter.strip())
                    definition.fields.append((parameter.strip()[at:].strip(), kind))
        elif keyword in ('interface', 'delegate'):
            match = re.fullmatch(r'(interface|delegate) ([\w`]+)(?:<([\w, ]+)>)? \{([0-9a-f-]+)\}(.*)', text)
            if not match:
                fail(number, f'not a {keyword}: {text}')
            definition = Definition(keyword, namespace, match.group(2), number)
            definition.generics = [name.strip() for name in match.group(3).split(',')] if match.group(3) else []
            definition.guid = uuid.UUID(match.group(4))
            rest = match.group(5).strip()
            if keyword == 'delegate':
                returns, name, parameters = parse_signature(rest, number)
                definition.members.append(Member('method', name, returns, parameters))
            elif rest:
                if not rest.startswith('requires '):
                    fail(number, f'unexpected "{rest}"')
                at, body = 0, rest[len('requires '):]
                while at < len(body):
                    kind, at = parse_type(body, at)
                    definition.requires.append(kind)
                    while at < len(body) and body[at] in ', ':
                        at += 1
            current = definition
        elif keyword == 'class':
            definition = Definition('class', namespace, words[1], number)
            at = 2
            while at < len(words):
                if words[at] == 'default':
                    definition.default = words[at + 1]
                    at += 2
                elif words[at] == 'statics':
                    definition.statics = words[at + 1]
                    at += 2
                elif words[at] == 'implements':
                    at += 1
                    while at < len(words) and words[at] not in ('default', 'statics'):
                        definition.implements.append(words[at].rstrip(','))
                        at += 1
                else:
                    fail(number, f'unexpected "{words[at]}"')
        else:
            fail(number, f'unknown "{keyword}"')
        definitions.append(definition)
    return definitions


class Writer:
    def __init__(self, definitions, module_name):
        self.definitions = definitions
        self.by_full = {definition.full: definition for definition in definitions}
        self.strings, self.string_index = bytearray(b'\0'), {'': 0}
        self.blobs, self.blob_index = bytearray(b'\0'), {b'': 0}
        self.guids = bytearray()
        self.tables = {table: [] for table in SCHEMA}
        self.typerefs, self.typespecs, self.memberrefs = {}, {}, {}
        self.module_name = module_name

    # Heaps.
    def string(self, text):
        if text not in self.string_index:
            self.string_index[text] = len(self.strings)
            self.strings += text.encode('utf-8') + b'\0'
        return self.string_index[text]

    def blob(self, data):
        data = bytes(data)
        if data not in self.blob_index:
            self.blob_index[data] = len(self.blobs)
            self.blobs += compressed(len(data)) + data
        return self.blob_index[data]

    def add(self, table, *row):
        self.tables[table].append(list(row))
        return len(self.tables[table])

    # Types.
    def typeref(self, namespace, name):
        key = (namespace, name)
        if key not in self.typerefs:
            self.typerefs[key] = self.add(TYPEREF, ('ResolutionScope', ASSEMBLYREF, self.mscorlib), self.string(name), self.string(namespace))
        return self.typerefs[key]

    def resolve(self, name, context):
        if name in self.by_full:
            return self.by_full[name]
        if f'{context.namespace}.{name}' in self.by_full:
            return self.by_full[f'{context.namespace}.{name}']
        found = [definition for definition in self.definitions if definition.name == name]
        if len(found) != 1:
            fail(context.line, f'no single type named {name}')
        return found[0]

    def encode(self, kind, context, generics=()):
        """The signature bytes of a type as written in context, whose generic parameters are listed in generics."""
        if kind.array:
            return bytes([0x1d]) + self.encode(kind.arguments[0], context, generics)
        if kind.name in generics:
            return bytes([0x13]) + compressed(generics.index(kind.name))
        if kind.name in PRIMITIVES:
            return bytes([PRIMITIVES[kind.name]])
        if kind.name == 'Guid':
            return bytes([0x11]) + compressed(self.typeref('System', 'Guid') << 2 | 1)
        if kind.name == 'Type':
            return bytes([0x12]) + compressed(self.typeref('System', 'Type') << 2 | 1)
        target = self.resolve(kind.name, context)
        element = 0x11 if target.kind in ('struct', 'enum') else 0x12
        coded = compressed(target.row << 2)
        if not kind.arguments:
            if target.generics:
                fail(context.line, f'{kind.name} needs its type arguments')
            return bytes([element]) + coded
        if len(kind.arguments) != len(target.generics):
            fail(context.line, f'{kind.name} takes {len(target.generics)} type arguments')
        data = bytes([0x15, element]) + coded + compressed(len(kind.arguments))
        for argument in kind.arguments:
            data += self.encode(argument, context, generics)
        return data

    def type_index(self, kind, context, generics=()):
        """A TypeDefOrRef coded index for a type: its definition, or a type specification for an instantiation."""
        if kind.arguments:
            signature = self.encode(kind, context, generics)
            if signature not in self.typespecs:
                self.typespecs[signature] = self.add(TYPESPEC, self.blob(signature))
            return ('TypeDefOrRef', TYPESPEC, self.typespecs[signature])
        return ('TypeDefOrRef', TYPEDEF, self.resolve(kind.name, context).row)

    def method_signature(self, member, context, generics, instance=True):
        data = bytes([0x20 if instance else 0x00]) + compressed(len(member.parameters))
        data += self.encode(member.returns, context, generics)
        for direction, kind, _ in member.parameters:
            data += (b'\x10' if direction == 'out' else b'') + self.encode(kind, context, generics)
        return data

    # Attributes.
    def attribute(self, parent, name, arguments):
        definition = self.by_full[f'Windows.Foundation.Metadata.{name}']
        data = bytearray(b'\x01\x00')
        for (_, kind), value in zip(definition.fields, arguments):
            if kind.name in ('UInt32', 'Int32'):
                data += struct.pack('<I', value & 0xffffffff)
            elif kind.name == 'UInt16':
                data += struct.pack('<H', value)
            elif kind.name == 'UInt8':
                data += bytes([value])
            else:
                encoded = value.encode('utf-8')
                data += compressed(len(encoded)) + encoded
        data += b'\0\0'
        self.add(CUSTOMATTRIBUTE, ('HasCustomAttribute',) + parent, ('CustomAttributeType', METHODDEF, definition.constructor), self.blob(data))

    def guid_attribute(self, parent, value):
        fields = value.fields
        self.attribute(parent, 'GuidAttribute', [fields[0], fields[1], fields[2]] + list(value.bytes[8:]))

    def write_tables(self):
        self.add(MODULE, 0, self.string(self.module_name), 1, 0, 0)
        self.guids += uuid.uuid5(uuid.NAMESPACE_URL, 'raven:' + self.module_name).bytes_le
        self.add(ASSEMBLY, 0x8004, 255, 255, 255, 255, 0x200, 0, self.string('Windows'), 0)
        self.mscorlib = self.add(ASSEMBLYREF, 255, 255, 255, 255, 0, self.blob(bytes.fromhex('b77a5c561934e089')), self.string('mscorlib'), 0, 0)
        self.add(TYPEDEF, 0, self.string('<Module>'), 0, ('TypeDefOrRef', TYPEDEF, 0), 1, 1)
        # Rows of the definitions first, so signatures can name any of them.
        for index, definition in enumerate(self.definitions):
            definition.row = index + 2
        for definition in self.definitions:
            getattr(self, 'write_' + definition.kind)(definition)
        # Types that declare nothing point past the end of the lists.
        for row in self.tables[TYPEDEF]:
            row[4] = row[4] or len(self.tables[FIELD]) + 1
            row[5] = row[5] or len(self.tables[METHODDEF]) + 1

    def typedef(self, definition, flags, extends):
        self.add(TYPEDEF, flags, self.string(definition.name), self.string(definition.namespace), extends,
                 len(self.tables[FIELD]) + 1, len(self.tables[METHODDEF]) + 1)
        assert len(self.tables[TYPEDEF]) == definition.row
        return definition.row

    def method(self, name, flags, signature, parameters, implementation=0):
        row = self.add(METHODDEF, 0, implementation, flags, self.string(name), self.blob(signature), len(self.tables[PARAM]) + 1)
        for sequence, (direction, _, parameter) in enumerate(parameters, 1):
            self.add(PARAM, 2 if direction in ('out', 'fill') else 1, sequence, self.string(parameter))
        return row

    def write_struct(self, definition):
        self.typedef(definition, TD_PUBLIC | TD_SEQUENTIAL | TD_SEALED | TD_WINDOWS_RUNTIME, self.system('ValueType'))
        for name, kind in definition.fields:
            self.add(FIELD, MD_PUBLIC, self.string(name), self.blob(b'\x06' + self.encode(kind, definition)))

    def write_enum(self, definition):
        row = self.typedef(definition, TD_PUBLIC | TD_SEALED | TD_WINDOWS_RUNTIME, self.system('Enum'))
        self.add(FIELD, 0x0606, self.string('value__'), self.blob(b'\x06\x08'))
        for name, value in definition.fields:
            field = self.add(FIELD, 0x8056, self.string(name), self.blob(b'\x06\x11' + compressed(row << 2)))
            self.add(CONSTANT, 0x08, 0, ('HasConstant', FIELD, field), self.blob(struct.pack('<i', value)))

    def write_attribute(self, definition):
        self.typedef(definition, TD_PUBLIC | TD_SEALED | TD_WINDOWS_RUNTIME, self.system('Attribute'))
        member = Member('method', '.ctor', TypeName('Void'), [('in', kind, name) for name, kind in definition.fields])
        definition.constructor = self.method('.ctor', MD_PUBLIC | MD_HIDE_BY_SIG | MD_SPECIAL | MD_RT_SPECIAL,
                                             self.method_signature(member, definition, ()), member.parameters, IMPL_RUNTIME)

    def generic_parameters(self, definition):
        for number, name in enumerate(definition.generics):
            self.add(GENERICPARAM, number, 0, ('TypeOrMethodDef', TYPEDEF, definition.row), self.string(name))

    def write_delegate(self, definition):
        self.typedef(definition, TD_PUBLIC | TD_SEALED | TD_WINDOWS_RUNTIME, self.system('MulticastDelegate'))
        self.generic_parameters(definition)
        self.method('.ctor', MD_PUBLIC | MD_HIDE_BY_SIG | MD_SPECIAL | MD_RT_SPECIAL, b'\x20\x02\x01\x1c\x18',
                    [('in', None, 'object'), ('in', None, 'method')], IMPL_RUNTIME)
        member = definition.members[0]
        self.method(member.name, MD_PUBLIC | MD_VIRTUAL | MD_HIDE_BY_SIG | MD_NEW_SLOT,
                    self.method_signature(member, definition, definition.generics), member.parameters, IMPL_RUNTIME)
        self.guid_attribute((TYPEDEF, definition.row), definition.guid)

    def accessor(self, member):
        return ('get_' if member.kind == 'get' else 'put_') + member.name

    def accessor_signature(self, member, context, generics, instance=True):
        if member.kind == 'get':
            return self.method_signature(Member('method', '', member.returns, []), context, generics, instance)
        return self.method_signature(Member('method', '', TypeName('Void'), [('in', member.returns, 'value')]), context, generics, instance)

    def members(self, definition, interface, flags, implementation, instance, generics):
        """Declares an interface's members on definition; returns the methods, in order, with their properties recorded."""
        rows, properties = [], {}
        for member in interface.members:
            if member.kind == 'method':
                row = self.method(member.name, flags, self.method_signature(member, interface, generics, instance), member.parameters,
                                  implementation)
                if member.unique:
                    self.attribute((METHODDEF, row), 'OverloadAttribute', [member.unique])
            else:
                parameters = [] if member.kind == 'get' else [('in', member.returns, 'value')]
                row = self.method(self.accessor(member), flags | MD_SPECIAL, self.accessor_signature(member, interface, generics, instance),
                                  parameters, implementation)
                entry = properties.setdefault(member.name, [member, [], instance])
                entry[1].append((1 if member.kind == 'set' else 2, row))
            rows.append(row)
        return rows, properties

    def write_properties(self, definition, properties, context, generics):
        """properties: name to (member, accessors, instance), all of one type, which a single property map lists."""
        if not properties:
            return
        self.add(PROPERTYMAP, definition.row, len(self.tables[PROPERTY]) + 1)
        for name, (member, accessors, instance) in properties.items():
            signature = bytes([0x28 if instance else 0x08, 0]) + self.encode(member.returns, context, generics)
            row = self.add(PROPERTY, 0, self.string(name), self.blob(signature))
            for semantics, method in accessors:
                self.add(METHODSEMANTICS, semantics, method, ('HasSemantics', PROPERTY, row))

    def write_interface(self, definition):
        self.typedef(definition, TD_PUBLIC | TD_INTERFACE | TD_ABSTRACT | TD_WINDOWS_RUNTIME, ('TypeDefOrRef', TYPEDEF, 0))
        self.generic_parameters(definition)
        for required in definition.requires:
            self.add(INTERFACEIMPL, definition.row, self.type_index(required, definition, definition.generics))
        definition.methods, properties = self.members(definition, definition, MD_PUBLIC | MD_VIRTUAL | MD_HIDE_BY_SIG | MD_NEW_SLOT | MD_ABSTRACT,
                                                      0, True, definition.generics)
        self.write_properties(definition, properties, definition, definition.generics)
        self.guid_attribute((TYPEDEF, definition.row), definition.guid)

    def write_class(self, definition):
        self.typedef(definition, TD_PUBLIC | TD_SEALED | TD_WINDOWS_RUNTIME, self.system('Object'))
        definition.pending, properties = [], {}
        for name in ([definition.default] if definition.default else []) + definition.implements:
            interface = self.resolve(name, definition)
            implementation = self.add(INTERFACEIMPL, definition.row, ('TypeDefOrRef', TYPEDEF, interface.row))
            if name == definition.default:
                self.attribute((INTERFACEIMPL, implementation), 'DefaultAttribute', [])
            rows, declared = self.members(definition, interface, MD_PUBLIC | MD_FINAL | MD_VIRTUAL | MD_HIDE_BY_SIG | MD_NEW_SLOT,
                                          IMPL_RUNTIME, True, ())
            definition.pending.append((rows, interface))
            properties.update(declared)
        if definition.statics:
            interface = self.resolve(definition.statics, definition)
            _, declared = self.members(definition, interface, MD_PUBLIC | MD_STATIC | MD_HIDE_BY_SIG, IMPL_RUNTIME, False, ())
            properties.update(declared)
            self.attribute((TYPEDEF, definition.row), 'StaticAttribute', [interface.full, 1])
        self.write_properties(definition, properties, definition, ())

    def method_impls(self):
        """Each class method implements its interface's method, which this file defines, as no class here takes a generic interface."""
        for definition in self.definitions:
            for rows, interface in getattr(definition, 'pending', []):
                for row, declared in zip(rows, interface.methods):
                    self.add(METHODIMPL, definition.row, ('MethodDefOrRef', METHODDEF, row), ('MethodDefOrRef', METHODDEF, declared))

    def system(self, name):
        return ('TypeDefOrRef', TYPEREF, self.typeref('System', name))

    # Serialisation.
    def index_width(self, table):
        return 2 if len(self.tables[table]) < 0x10000 else 4

    def coded_width(self, name):
        bits, tables = CODED[name]
        largest = max(len(self.tables[table]) for table in tables if table is not None)
        return 2 if largest < (1 << (16 - bits)) else 4

    def heap_width(self, heap):
        return 2 if len(heap) < 0x10000 else 4

    def encode_row(self, table, row):
        data = bytearray()
        for column, value in zip(SCHEMA[table], row):
            if column in ('u1', 'u2', 'u4'):
                data += value.to_bytes(int(column[1]), 'little')
                continue
            if column == 'S':
                width, number = self.heap_width(self.strings), value
            elif column == 'B':
                width, number = self.heap_width(self.blobs), value
            elif column == 'G':
                width, number = self.heap_width(self.guids), value
            elif isinstance(column, int):
                width, number = self.index_width(column), value
            else:
                name, target, index = value
                bits, tables = CODED[name]
                width, number = self.coded_width(name), index << bits | tables.index(target)
            data += number.to_bytes(width, 'little')
        return data

    def table_stream(self):
        for table, column in SORTED.items():
            self.tables[table].sort(key=lambda row: self.sort_key(table, row[column]))
        present = [table for table in sorted(SCHEMA) if self.tables[table]]
        heap_sizes = (self.heap_width(self.strings) == 4) | (self.heap_width(self.guids) == 4) << 1 | (self.heap_width(self.blobs) == 4) << 2
        data = bytearray(struct.pack('<IBBBB', 0, 2, 0, heap_sizes, 1))
        data += struct.pack('<QQ', sum(1 << table for table in present), sum(1 << table for table in SORTED))
        for table in present:
            data += struct.pack('<I', len(self.tables[table]))
        for table in present:
            for row in self.tables[table]:
                data += self.encode_row(table, row)
        while len(data) % 4:
            data.append(0)
        return bytes(data)

    def sort_key(self, table, column):
        if isinstance(column, tuple):
            name, target, index = column
            bits, tables = CODED[name]
            return index << bits | tables.index(target)
        return column

    def metadata(self):
        self.write_tables()
        self.method_impls()
        streams = [('#~', self.table_stream()), ('#Strings', bytes(self.strings)), ('#US', b'\0\0\0\0'), ('#GUID', bytes(self.guids)),
                   ('#Blob', bytes(self.blobs))]
        version = b'WindowsRuntime 1.4'
        version += b'\0' * (4 - len(version) % 4)
        header = struct.pack('<IHHII', 0x424a5342, 1, 1, 0, len(version)) + version + struct.pack('<HH', 0, len(streams))
        headers_size = len(header) + sum(8 + (len(name) + 4) // 4 * 4 for name, _ in streams)
        offset, body, directory = headers_size, bytearray(), bytearray()
        for name, data in streams:
            data = data + b'\0' * (-len(data) % 4)
            directory += struct.pack('<II', offset, len(data)) + name.encode() + b'\0' * ((len(name) + 4) // 4 * 4 - len(name))
            body += data
            offset += len(data)
        return header + directory + body


def pe_image(metadata):
    """A PE32 library holding only a CLI header and its metadata."""
    section_rva, file_offset = 0x2000, 0x200
    cli = struct.pack('<IHHIIII', 72, 2, 5, section_rva + 72, len(metadata), 1, 0) + b'\0' * 48
    text = cli + metadata
    raw_size = (len(text) + 0x1ff) // 0x200 * 0x200
    image_size = section_rva + (len(text) + 0x1fff) // 0x2000 * 0x2000
    dos = bytearray(0x80)
    dos[0:2] = b'MZ'
    dos[0x3c:0x40] = struct.pack('<I', 0x80)
    coff = struct.pack('<4sHHIIIHH', b'PE\0\0', 0x14c, 1, 0, 0, 0, 0xe0, 0x2102)
    optional = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', 0x10b, 8, 0, raw_size, 0, 0, 0, section_rva, 0, 0x400000, 0x2000, 0x200,
                           4, 0, 0, 0, 4, 0, 0, image_size, file_offset, 0, 3, 0x8540, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    directories = bytearray(16 * 8)
    directories[14 * 8:15 * 8] = struct.pack('<II', section_rva, 72)
    section = struct.pack('<8sIIIIIIHHI', b'.text', len(text), section_rva, raw_size, file_offset, 0, 0, 0, 0, 0x60000020)
    headers = bytes(dos) + coff + optional + bytes(directories) + section
    return headers + b'\0' * (file_offset - len(headers)) + text + b'\0' * (raw_size - len(text))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    definitions = parse(sys.argv[1])
    output = sys.argv[2]
    writer = Writer(definitions, output.replace('\\', '/').rsplit('/', 1)[-1])
    image = pe_image(writer.metadata())
    with open(output, 'wb') as file:
        file.write(image)


if __name__ == '__main__':
    main()
