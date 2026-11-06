"""Strict, independent scalar/reference validator for GTI mir-v120 text dumps.

This is intentionally a narrow second implementation, not a general MIR reader.
It executes scalar SSA/places and checks concrete accesses and loan endpoints on
normal and invocation-failure paths. Unsupported shapes never count as verified.
Compiler `valid`, reachability, ownership events and effect summaries are NOT
accepted as evidence. Numeric vocabulary is pinned to mir-v120; schema changes
require review. It does not establish frontend or native-code correspondence.
"""
from __future__ import annotations
import copy
import dataclasses
import re
from model import Fault, Result

VERSION = 'mir-v120'
INSTRUCTIONS = {'compute', 'initialize', 'borrow', 'load', 'assign', 'end-borrow', 'lifecycle'}


def field(text, key, default=None):
    found = re.search(r'(?:^| )' + re.escape(key) + r'=([^ ]+)', text)
    return found.group(1) if found else default


def number(text, key, default=None):
    value = field(text, key)
    if value is None:
        if default is not None:
            return default
        raise Fault('unsupported', 'MIR missing field ' + key)
    if value == '-':
        return 0
    if not re.fullmatch(r'\d+', value):
        raise Fault('unsupported', 'MIR nonnumeric ' + key)
    return int(value)


def operands(text):
    values = re.findall(r'operand\((\d+);value=(\d+);place=(\d+);loan=(\d+);literal=([^;]*);', text)
    if text.count('operand(') != len(values):
        raise Fault('unsupported', 'MIR unrecognized operand encoding')
    return [(int(k), int(v), int(p), int(l), c) for k, v, p, l, c in values]


def literal(text):
    match = re.fullmatch(r'(?:u64|i64):(-?\d+)', text)
    if match:
        return int(match.group(1))
    if text in ('bool:true', 'bool:1'):
        return 1
    if text in ('bool:false', 'bool:0'):
        return 0
    raise Fault('unsupported', 'MIR literal outside scalar profile: ' + str(text))


@dataclasses.dataclass
class Frame:
    memory: dict = dataclasses.field(default_factory=dict)  # binding -> int or ('ref', loan, activation)
    values: dict = dataclasses.field(default_factory=dict)
    loans: dict = dataclasses.field(default_factory=dict)  # id -> (owner-binding, parent, mode, activation)
    failed: bool = False
    next_activation: int = 1


class Dump:
    def __init__(self, text):
        if not text.startswith(VERSION + ' '):
            raise Fault('unsupported', 'MIR version: expected ' + VERSION)
        initialization = re.search(r'^program-initialization units=\d+ steps=(\d+)$', text, re.M)
        if not initialization or initialization.group(1) != '0':
            raise Fault('unsupported', 'MIR nonempty program initialization')
        declarations = re.findall(r'^function @\d+ .*$', text, re.M)
        entries = [d for d in declarations if field(d, 'entry') == '1:0']
        if len(entries) != 1 or field(entries[0], 'parameters') != '[]':
            raise Fault('unsupported', 'MIR profile requires one parameterless entry function')
        start = text.index(entries[0]) + len(entries[0])
        end = text.find('\nend-body', start)
        if end < 0:
            raise Fault('unsupported', 'MIR body terminator missing')
        body = text[start:end]
        self.entry = 0
        self.places, self.loan_rows, self.blocks = {}, {}, {}
        self.binding_places = {}
        block = None
        instruction_ids = set()
        for line in body.splitlines():
            if line.startswith('body '):
                match = re.fullmatch(r'bb(\d+)', field(line, 'entry', ''))
                if not match:
                    raise Fault('unsupported', 'MIR missing entry block')
                self.entry = int(match.group(1))
            elif re.match(r'  p\d+ ', line):
                pid = int(re.match(r'  p(\d+) ', line).group(1))
                if pid in self.places:
                    raise Fault('rejected', 'MIR duplicate place')
                type_text = field(line, 'type', '')
                typ = re.match(r'type\((\d+);', type_text)
                if not typ or int(typ.group(1)) not in (4, 12, 21):
                    raise Fault('unsupported', 'MIR nonscalar place type')
                reference_access = re.search(r';access=([01]);', type_text.split(';args=', 1)[0] + ';')
                if int(typ.group(1)) == 21 and not reference_access:
                    raise Fault('unsupported', 'MIR missing reference access mode')
                projection = field(line, 'projections')
                if projection not in ('[]', '[projection(2;field=0;index=0;constant=-;selection=0)]'):
                    raise Fault('unsupported', 'MIR non-dereference projection')
                if number(line, 'root') != 0 or number(line, 'initially-available') != 0:
                    raise Fault('unsupported', 'MIR nonlocal or initially available place')
                binding = number(line, 'binding')
                if not binding:
                    raise Fault('unsupported', 'MIR unbound place')
                place = {'binding': binding, 'deref': projection != '[]',
                         'type': int(typ.group(1)), 'access': number(line, 'access'),
                         'reference_access': int(reference_access.group(1)) if reference_access else None}
                self.places[pid] = place
                if not place['deref']:
                    if binding in self.binding_places and self.binding_places[binding] != pid:
                        raise Fault('unsupported', 'MIR duplicate binding place')
                    self.binding_places[binding] = pid
            elif re.match(r'  loan\d+ ', line):
                lid = int(re.match(r'  loan(\d+) ', line).group(1))
                if lid in self.loan_rows:
                    raise Fault('rejected', 'MIR duplicate loan')
                if number(line, 'kind') not in (0, 5) or field(line, 'additional-dependencies') != '[]' or \
                   field(line, 'conditions') != '[]' or number(line, 'two-phase') or number(line, 'escapes'):
                    raise Fault('unsupported', 'MIR loan outside local scalar profile')
                source = field(line, 'source', '')
                parent = field(line, 'parent', '')
                if not re.fullmatch(r'p\d+', source) or not re.fullmatch(r'loan\d+', parent):
                    raise Fault('unsupported', 'MIR unknown loan source/parent')
                self.loan_rows[lid] = {'source': int(source[1:]), 'parent': int(parent[4:]),
                                       'access': number(line, 'access')}
            elif re.match(r'  bb\d+ ', line):
                block = int(re.match(r'  bb(\d+) ', line).group(1))
                if block in self.blocks:
                    raise Fault('rejected', 'MIR duplicate block')
                if field(line, 'parameters') != '[]':
                    raise Fault('unsupported', 'MIR block parameters unsupported')
                self.blocks[block] = {'instructions': [], 'term': None}
            elif line.startswith('    i'):
                if block is None:
                    raise Fault('unsupported', 'MIR instruction outside block')
                match = re.match(r'    i(\d+) ([a-z-]+) ', line)
                if not match or match.group(2) not in INSTRUCTIONS:
                    raise Fault('unsupported', 'MIR instruction outside scalar profile')
                if int(match.group(1)) in instruction_ids:
                    raise Fault('rejected', 'MIR duplicate instruction id')
                instruction_ids.add(int(match.group(1)))
                for key in ('tombstone', 'unsafe-operation', 'raw-memory', 'object-allocation'):
                    if number(line, key, 0):
                        raise Fault('unsupported', 'MIR unsupported instruction flag ' + key)
                if field(line, 'lifecycle') != '[]':
                    raise Fault('unsupported', 'MIR executable lifecycle payload unsupported')
                if any(field(line, key) != '-' for key in ('function', 'constructor', 'receiver', 'lambda', 'element-cleanup')):
                    raise Fault('unsupported', 'MIR call/object operation unsupported')
                if field(line, 'intrinsic') not in (None, 'none'):
                    raise Fault('unsupported', 'MIR intrinsic unsupported')
                self.blocks[block]['instructions'].append((int(match.group(1)), match.group(2), line))
            elif line.startswith('    term '):
                if block is None or self.blocks[block]['term'] is not None:
                    raise Fault('rejected', 'MIR misplaced/duplicate terminator')
                kind = int(line.split()[1])
                if kind not in (1, 2, 4, 5, 6, 11):
                    raise Fault('unsupported', 'MIR terminator outside scalar profile')
                if any(field(line, key) != '[]' for key in ('target-arguments', 'else-arguments', 'success-lifecycle')):
                    raise Fault('unsupported', 'MIR successor payload unsupported')
                self.blocks[block]['term'] = kind, line
        if self.entry not in self.blocks or any(b['term'] is None for b in self.blocks.values()):
            raise Fault('rejected', 'MIR missing entry/terminator')

    def location(self, frame, pid, write=False, initialized=True, authorized_loan=0):
        if pid not in self.places:
            raise Fault('rejected', 'MIR unknown place')
        p = self.places[pid]
        owner, via = p['binding'], 0
        permission = p['access']
        if p['deref']:
            root = self.places.get(self.binding_places.get(owner))
            if not root or root['type'] != 21:
                raise Fault('rejected', 'MIR dereference without reference binding')
            # Projected access describes this expression, not referent capability.
            permission = root['reference_access']
            value = frame.memory.get(owner)
            if not isinstance(value, tuple) or value[0] != 'ref':
                raise Fault('rejected', 'MIR uninitialized reference')
            via = value[1]
            if via not in frame.loans or value[2] != frame.loans[via][3]:
                raise Fault('rejected', 'MIR ended reference used')
            owner = frame.loans[via][0]
        if authorized_loan:
            if authorized_loan not in frame.loans or frame.loans[authorized_loan][0] != owner:
                raise Fault('rejected', 'MIR forged loan address')
            via = authorized_loan
        if initialized and owner not in frame.memory:
            raise Fault('rejected', 'MIR uninitialized scalar read')
        if write and (permission != 1 or via and frame.loans[via][2] != 1):
            raise Fault('rejected', 'MIR read-only write')
        ancestors, parent = set(), via
        while parent:
            if parent in ancestors or parent not in frame.loans:
                raise Fault('rejected', 'MIR invalid loan ancestry')
            ancestors.add(parent)
            parent = frame.loans[parent][1]
        for lid, loan in frame.loans.items():
            if loan[0] == owner and lid not in ancestors and (write or loan[2] == 1):
                raise Fault('rejected', 'MIR conflicting scalar access')
        return owner, via

    def operand(self, frame, operand):
        kind, value, place, loan, constant = operand
        if kind == 0:
            if value not in frame.values:
                raise Fault('rejected', 'MIR SSA value not defined')
            return frame.values[value]
        if kind == 1:
            return literal(constant)
        if kind == 3:
            owner, _ = self.location(frame, place)
            return frame.memory[owner]
        if kind == 7:
            if loan not in frame.loans:
                raise Fault('rejected', 'MIR inactive loan operand')
            owner, _ = self.location(frame, place, authorized_loan=loan)
            if owner != frame.loans[loan][0]:
                raise Fault('rejected', 'MIR forged loan address')
            return ('ref', loan, frame.loans[loan][3])
        raise Fault('unsupported', 'MIR operand outside scalar profile')

    def instruction(self, frame, op, line):
        operands_text = re.search(r' operands=(.*?) parameters=', line)
        if not operands_text:
            raise Fault('unsupported', 'MIR operands missing')
        args = operands(operands_text.group(1))
        result, dest, loan = number(line, 'result'), number(line, 'destination'), number(line, 'loan')
        if op == 'lifecycle':
            if args or result or dest or loan:
                raise Fault('unsupported', 'MIR nonempty lifecycle marker')
        elif op == 'compute':
            operation = field(line, 'operation')
            vals = [self.operand(frame, a) for a in args]
            if not result or any(not isinstance(v, int) for v in vals):
                raise Fault('unsupported', 'MIR scalar computation required')
            if operation == 'literal' and not vals:
                value = literal(field(line, 'literal'))
            elif len(vals) == 2 and operation in ('add', 'subtract', 'multiply', 'equal', 'not-equal', 'less', 'less-equal', 'greater', 'greater-equal'):
                a, b = vals
                value = {'add': lambda: a + b, 'subtract': lambda: a - b,
                    'multiply': lambda: a * b, 'equal': lambda: int(a == b),
                    'not-equal': lambda: int(a != b), 'less': lambda: int(a < b),
                    'less-equal': lambda: int(a <= b), 'greater': lambda: int(a > b),
                    'greater-equal': lambda: int(a >= b)}[operation]()
            else:
                raise Fault('unsupported', 'MIR compute operation ' + str(operation))
            if not -(2**31) <= value < 2**31:
                raise Fault('unsupported', 'MIR overflow requires extended failure execution profile')
            frame.values[result] = value
        elif op in ('initialize', 'assign'):
            if len(args) != 1 or dest not in self.places:
                raise Fault('unsupported', 'MIR scalar assignment shape')
            p = self.places[dest]
            value = self.operand(frame, args[0])
            if op == 'initialize':
                if p['deref']:
                    raise Fault('rejected', 'MIR projected initialization unsupported')
                previous = frame.memory.get(p['binding'])
                if previous is not None:
                    # Re-entered loop locals reuse static MIR binding/loan IDs.
                    # Only a previous reference whose activation ended may be
                    # rebound; a live reference is never silently reseated.
                    if not isinstance(previous, tuple) or (previous[1] in frame.loans and
                            previous[2] == frame.loans[previous[1]][3]):
                        raise Fault('rejected', 'MIR double initialization')
                if (p['type'] == 21) != isinstance(value, tuple):
                    raise Fault('rejected', 'MIR reference/scalar type mismatch')
                if p['type'] == 21 and p['reference_access'] == 1 and frame.loans[value[1]][2] != 1:
                    raise Fault('rejected', 'MIR mutable reference from shared loan')
                frame.memory[p['binding']] = value
            else:
                owner, _ = self.location(frame, dest, write=True)
                if not isinstance(value, int):
                    raise Fault('unsupported', 'MIR reference reseating unsupported')
                frame.memory[owner] = value
        elif op == 'borrow':
            if len(args) != 1 or args[0][0] not in (5, 6) or loan not in self.loan_rows:
                raise Fault('unsupported', 'MIR borrow operand shape')
            if loan in frame.loans:
                raise Fault('rejected', 'MIR duplicate active loan')
            mode, pid = int(args[0][0] == 6), args[0][2]
            owner, parent = self.location(frame, pid, write=bool(mode))
            row = self.loan_rows[loan]
            if row != {'source': pid, 'parent': parent, 'access': mode}:
                raise Fault('rejected', 'MIR loan metadata disagrees with executable borrow')
            frame.loans[loan] = (owner, parent, mode, frame.next_activation)
            frame.next_activation += 1
        elif op == 'load':
            if len(args) != 1 or not result:
                raise Fault('unsupported', 'MIR load shape')
            frame.values[result] = self.operand(frame, args[0])
        elif op == 'end-borrow':
            if args or loan not in frame.loans:
                raise Fault('rejected', 'MIR ending inactive loan')
            if any(v[1] == loan for v in frame.loans.values()):
                raise Fault('rejected', 'MIR parent ended before child')
            del frame.loans[loan]


def verify_mir_dump(text, *, max_steps=10000):
    """Return Result; observations contain reachable normal return integers.

    Explores both invoke edges to independently inspect failure cleanup, even
    when this concrete scalar computation does not fail. No `valid=1` trust.
    """
    steps, trace, returns, states = 0, [], [], 0
    try:
        if len(text) > 32 * 1024 * 1024:
            raise Fault('timeout', 'MIR input byte budget exceeded')
        dump = Dump(text)
        work, seen = [(dump.entry, Frame())], set()
        while work:
            block, frame = work.pop()
            key = (block, repr(frame))
            if key in seen:
                continue
            seen.add(key)
            states += 1
            if block not in dump.blocks:
                raise Fault('rejected', 'MIR missing successor')
            contents = dump.blocks[block]
            for iid, op, line in contents['instructions']:
                steps += 1
                trace.append(f'bb{block}:i{iid} {op}')
                trace = trace[-64:]
                if steps > max_steps:
                    raise Fault('timeout', 'MIR execution budget exceeded')
                dump.instruction(frame, op, line)
            kind, line = contents['term']
            steps += 1
            if steps > max_steps:
                raise Fault('timeout', 'MIR execution budget exceeded')
            def successor(key):
                value = field(line, key, '')
                if not re.fullmatch(r'bb[1-9]\d*', value):
                    raise Fault('rejected', 'MIR invalid successor')
                return int(value[2:])
            if kind in (1, 11):
                if kind == 11 and not frame.failed:
                    raise Fault('rejected', 'MIR failure continuation without failure')
                work.append((successor('target'), frame))
            elif kind == 2:
                term_values = operands(line)
                if len(term_values) != 1:
                    raise Fault('unsupported', 'MIR branch value shape')
                cond = dump.operand(frame, term_values[0])
                work.append((successor('target' if cond else 'else'), frame))
            elif kind == 4:
                invoke = field(line, 'invoke')
                if not contents['instructions'] or invoke != 'i' + str(contents['instructions'][-1][0]):
                    raise Fault('rejected', 'MIR invoke producer mismatch')
                producer = contents['instructions'][-1]
                if producer[1] != 'compute' or field(producer[2], 'operation') not in ('add', 'subtract', 'multiply'):
                    raise Fault('unsupported', 'MIR invocation producer outside arithmetic profile')
                failed = copy.deepcopy(frame)
                failed.failed = True
                producer_result = number(contents['instructions'][-1][2], 'result')
                failed.values.pop(producer_result, None)
                work.extend([(successor('target'), frame), (successor('else'), failed)])
            elif kind in (5, 6):
                if frame.loans:
                    raise Fault('rejected', 'MIR live loans at exit')
                if kind == 6:
                    if not frame.failed:
                        raise Fault('rejected', 'MIR propagates absent failure')
                else:
                    if frame.failed:
                        raise Fault('rejected', 'MIR failure path returns normally')
                    values = operands(line)
                    if len(values) != 1:
                        raise Fault('unsupported', 'MIR return value shape')
                    value = dump.operand(frame, values[0])
                    if not isinstance(value, int):
                        raise Fault('unsupported', 'MIR reference return outside profile')
                    returns.append(value)
        return Result('accepted', trace=trace, observations=sorted(set(returns)), steps=steps, states=states)
    except Fault as error:
        return Result(error.status, str(error), trace, returns, steps, states)
    except (KeyError, TypeError, ValueError, AttributeError) as error:
        return Result('unsupported', 'MIR malformed input: ' + str(error), trace, returns, steps, states)

# General finite concrete profile. Unlike the scalar adapter above, this executes
# actual nonrecursive call bodies and typed owned-allocation primitives. It does
# not assume that compiler origin summaries describe a callee's actual result.
@dataclasses.dataclass(frozen=True)
class CoreAddress:
    allocation: int
    generation: int
    path: tuple = ()


@dataclasses.dataclass(frozen=True)
class CoreReference:
    address: CoreAddress
    access: int
    token: int = 0


@dataclasses.dataclass(frozen=True)
class CoreOwner:
    address: CoreAddress | None


@dataclasses.dataclass
class CoreRecord:
    cls: int
    fields: dict


@dataclasses.dataclass
class CorePack:
    values: list


_UNINITIALIZED = ('uninitialized',)


def core_type(text):
    match = re.match(r'type\((\d+);', text)
    if not match:
        raise Fault('unsupported', 'core MIR missing type')
    def attr(key):
        match = re.search(r';' + key + r'=(\d+);', text.split(';args=', 1)[0] + ';')
        return int(match.group(1)) if match else 0
    return int(match.group(1)), attr('class'), attr('access')


class CoreBody:
    def __init__(self, head, text):
        self.head = head
        self.entry = number(text.splitlines()[0].replace('entry=bb', 'entry='), 'entry')
        self.params = [int(x) for x in re.findall(r'\bb(\d+)\b', field(head, 'parameter-bindings', '[]'))]
        self.places, self.loans, self.blocks, self.types = {}, {}, {}, {}
        self.roots = {}
        block = None
        for line in text.splitlines()[1:]:
            match = re.match(r'  p(\d+) ', line)
            if match:
                pid = int(match.group(1))
                if pid in self.places:
                    raise Fault('rejected', 'core MIR duplicate place')
                p = {k: number(line, k, 0) for k in ('root', 'binding', 'temporary', 'value', 'loan', 'access', 'initially-available')}
                p['type'] = core_type(field(line, 'type', ''))
                p['projections'] = [(int(a), int(c) if int(a)==5 and c!='-' else int(b)) for a,b,c in re.findall(r'projection\((\d+);field=(\d+);index=0;constant=(-|\d+);selection=0\)', field(line, 'projections', ''))]
                if len(p['projections']) != field(line, 'projections', '').count('projection('):
                    p['unsupported'] = True
                self.places[pid] = p
                if not p['projections']:
                    self.roots[(p['root'], p['binding'] or p['value'] or p['loan'])] = pid
            elif (match := re.match(r'  loan(\d+) ', line)):
                self.loans[int(match.group(1))] = line
            elif (match := re.match(r'  %(\d+) .*?info\((type\(.*)', line)):
                self.types[int(match.group(1))] = core_type(match.group(2))
            elif (match := re.match(r'  bb(\d+) ', line)):
                block = int(match.group(1))
                if block in self.blocks or field(line, 'parameters') != '[]':
                    raise Fault('unsupported', 'core MIR duplicate/parameterized block')
                self.blocks[block] = [[], None]
            elif line.startswith('    i'):
                if block is None:
                    raise Fault('rejected', 'core MIR instruction outside block')
                self.blocks[block][0].append(line)
            elif line.startswith('    term '):
                self.blocks[block][1] = line
        if self.entry not in self.blocks:
            raise Fault('rejected', 'core MIR missing entry')


class CoreProgram:
    def __init__(self, text):
        if not text.startswith(VERSION + ' '):
            raise Fault('unsupported', 'core MIR version')
        initialization = re.search(r'^program-initialization units=\d+ steps=(\d+)$', text, re.M)
        if not initialization or initialization.group(1) != '0':
            raise Fault('unsupported', 'core MIR global initialization')
        self.functions, self.constructors, self.destructors, self.classes = {}, {}, {}, {}
        self.entry = None
        for m in re.finditer(r'^(function|constructor|destructor) @(\d+) ([^\n]*)\n(body [\s\S]*?)\nend-body', text, re.M):
            kind, identity, rest, body = m.groups()
            head = kind + ' @' + identity + ' ' + rest
            target = {'function': self.functions, 'constructor': self.constructors, 'destructor': self.destructors}[kind]
            target[int(identity)] = CoreBody(head, body)
            if kind == 'function' and field(head, 'entry') == '1:0':
                if self.entry is not None:
                    raise Fault('rejected', 'core MIR multiple entries')
                self.entry = int(identity)
        for m in re.finditer(r'^class @(\d+) (.*)$', text, re.M):
            cid, line = int(m.group(1)), m.group(2)
            fields = {}
            for item in re.findall(r'\{field=\d+,symbol=\d+,type=type\(.*?\),access=\d+,drop=\d+,active-cleanup=\d+\}', field(line, 'declared-fields', '[]')):
                symbol = int(re.search(r'symbol=(\d+)', item).group(1))
                typ = core_type(item.split('type=', 1)[1])
                access = int(re.search(r'\),access=(\d+)', item).group(1))
                fields[symbol] = (typ, access)
            if len(fields) != field(line, 'declared-fields', '').count('{field='):
                raise Fault('unsupported', 'core MIR class field schema')
            self.classes[cid] = {'fields': fields, 'destructor': number(line, 'destructor', 0),
                'declaration': number(line, 'declaration'), 'line': line}
        self.class_declarations = {v['declaration']: k for k,v in self.classes.items()}
        if self.entry is None:
            raise Fault('unsupported', 'core MIR no entry')


@dataclasses.dataclass
class CoreFrame:
    identity: int
    body: CoreBody
    storage: dict = dataclasses.field(default_factory=dict)
    values: dict = dataclasses.field(default_factory=dict)
    loans: dict = dataclasses.field(default_factory=dict)
    receiver: CoreReference | None = None
    input_tokens: list = dataclasses.field(default_factory=list)


class CoreMachine:
    """Concrete storage machine, independent of source-model transitions.

    Only the named built-in allocation/borrow primitives have external meaning.
    Every ordinary function/constructor/destructor is executed, not summarized.
    Results are one finite concrete execution; failure is taken when an executed
    operation actually fails. The older scalar adapter additionally explores
    hypothetical arithmetic failure edges.
    """
    def __init__(self, program, max_steps):
        self.program, self.max_steps = program, max_steps
        self.steps = self.frames = self.next_token = 0
        self.cells, self.tags, self.generations, self.free = {}, {}, {}, []
        self.trace, self.cleanup, self.allocations = [], [], []
        self.observations=[]
        self.failure_cause=None
        self.active_calls = []
        self.features = set()

    def tick(self, message):
        self.steps += 1
        self.trace.append(message)
        self.trace = self.trace[-64:]
        if self.steps > self.max_steps:
            raise Fault('timeout', 'core MIR step budget')

    def allocate(self, value=_UNINITIALIZED, heap=False):
        aid = self.free.pop() if self.free else len(self.generations) + 1
        generation = self.generations.get(aid, 0) + 1
        self.generations[aid] = generation
        self.cells[aid] = {'generation': generation, 'value': value, 'heap': heap}
        self.allocations.append((aid, generation, heap))
        return CoreAddress(aid, generation)

    def cell(self, address):
        cell = self.cells.get(address.allocation)
        if cell is None or cell['generation'] != address.generation:
            raise Fault('rejected', 'core MIR stale allocation generation')
        return cell

    def get(self, address, initialized=True):
        value = self.cell(address)['value']
        for symbol in address.path:
            if isinstance(value, CorePack) and symbol<0:
                value=value.values[-symbol-1]
                continue
            if not isinstance(value, CoreRecord) or symbol not in value.fields:
                raise Fault('rejected', 'core MIR invalid record projection')
            value = value.fields[symbol]
        if initialized:
            def complete(item):
                if item==_UNINITIALIZED:return False
                if isinstance(item,CoreRecord):return all(complete(v) for v in item.fields.values())
                if isinstance(item,CorePack):return all(complete(v) for v in item.values)
                return True
            if not complete(value):raise Fault('rejected', 'core MIR uninitialized read')
        return value

    def set(self, address, value):
        cell = self.cell(address)
        if not address.path:
            cell['value'] = value
        else:
            parent = self.get(dataclasses.replace(address, path=address.path[:-1]),False)
            if not isinstance(parent, CoreRecord) or address.path[-1] not in parent.fields:
                raise Fault('rejected', 'core MIR unknown assigned field')
            parent.fields[address.path[-1]] = value

    def overlap(self, left, right):
        return (left.allocation, left.generation) == (right.allocation, right.generation) and \
            left.path[:min(len(left.path), len(right.path))] == right.path[:min(len(left.path), len(right.path))]

    def access(self, ref, write=False, initialized=True):
        self.cell(ref.address)
        if write and ref.access != 1:
            raise Fault('rejected', 'core MIR read-only write')
        ancestors, current = set(), ref.token
        while current:
            if current in ancestors or current not in self.tags:
                raise Fault('rejected', 'core MIR expired reference')
            ancestors.add(current)
            current = self.tags[current]['parent']
        for token, tag in self.tags.items():
            if token not in ancestors and self.overlap(tag['address'], ref.address) and (write or tag['access'] == 1):
                raise Fault('rejected', 'core MIR overlapping access')
        return self.get(ref.address, initialized)

    def borrow(self, ref, access, escape=False, initialized=True):
        self.access(ref, write=bool(access),initialized=initialized)
        self.next_token += 1
        self.tags[self.next_token] = {'address': ref.address, 'access': access, 'parent': ref.token, 'escape': escape}
        return CoreReference(ref.address, access, self.next_token)

    def end(self, token):
        if token not in self.tags:
            raise Fault('rejected', 'core MIR duplicate or inactive endpoint')
        parent = self.tags[token]['parent']
        for child, tag in self.tags.items():
            if tag['parent'] == token:
                if not tag['escape']:
                    raise Fault('rejected', 'core MIR parent ended before child')
                tag['parent'] = parent
        del self.tags[token]

    def place(self, frame, pid, create=False):
        p = frame.body.places.get(pid)
        if p is None or p.get('unsupported'):
            raise Fault('unsupported', 'core MIR place/projection schema')
        if p['type'][0] not in (4,12,19,21,22,25):
            raise Fault('unsupported','core MIR type outside scalar/record/reference/owner/pack')
        kind = p['root']
        if kind == 5:
            ref = frame.loans.get(p['loan'])
            if ref is None:
                raise Fault('rejected', 'core MIR missing result loan')
        elif kind == 2:
            ref = frame.receiver
            if ref is None:
                raise Fault('rejected', 'core MIR missing receiver')
        elif kind in (0, 3, 4):
            key = (kind, p['binding'] if kind == 0 else p['temporary'] if kind==3 else p['value'])
            if key not in frame.storage:
                value = frame.values.get(p['value'], _UNINITIALIZED) if kind == 4 else _UNINITIALIZED
                if not create and value == _UNINITIALIZED:
                    raise Fault('rejected', 'core MIR storage before initialization')
                frame.storage[key] = self.allocate(value)
            root_place=frame.body.places.get(frame.body.roots.get(key),p)
            ref = CoreReference(frame.storage[key], root_place['access'])
        else:
            raise Fault('unsupported', 'core MIR root outside local/receiver/value/loan')
        for projection, symbol in p['projections']:
            if projection == 2:
                value = self.get(ref.address)
                if not isinstance(value, CoreReference):
                    raise Fault('rejected', 'core MIR dereference of nonreference')
                root = frame.body.places.get(frame.body.roots.get((kind,p['binding'] or p['value'] or p['loan'])))
                permission = root['type'][2] if root and root['type'][0] == 21 else value.access
                ref = CoreReference(value.address, min(value.access, permission), value.token)
            elif projection == 0:
                obj = self.get(ref.address,False)
                if not isinstance(obj, CoreRecord):
                    raise Fault('rejected', 'core MIR field of nonrecord')
                layout = self.program.classes[obj.cls]['fields'].get(symbol)
                if layout is None:
                    raise Fault('rejected', 'core MIR unknown field')
                ref = CoreReference(dataclasses.replace(ref.address, path=ref.address.path + (symbol,)), min(ref.access,layout[1]), ref.token)
            elif projection == 5:
                data=self.get(ref.address)
                if not isinstance(data,CorePack) or not 0<=symbol<len(data.values):
                    raise Fault('rejected','core MIR invalid concrete pack element')
                ref=dataclasses.replace(ref,address=dataclasses.replace(ref.address,path=ref.address.path+(-symbol-1,)))
            else:
                raise Fault('unsupported', 'core MIR dynamic/raw/base projection')
        return ref

    def operand(self, frame, encoded, move=False):
        kind, value, place, loan, constant = encoded
        if kind == 0:
            if value not in frame.values:
                raise Fault('rejected', 'core MIR undefined SSA value')
            return frame.values[value]
        if kind == 1:
            if constant in ('null', 'nullptr'):
                return CoreOwner(None)
            return literal(constant)
        if kind in (3,4,5,6):
            ref = self.place(frame, place)
            if kind in (5,6):
                self.access(ref, write=kind == 6)
                return dataclasses.replace(ref, access=int(kind == 6))
            data = self.access(dataclasses.replace(ref,access=1) if kind==4 and not ref.token else ref, write=kind == 4)
            if kind == 4:
                self.set(ref.address, _UNINITIALIZED)
            return copy.deepcopy(data)
        if kind == 7:
            ref = frame.loans.get(loan)
            if ref is None or ref.token not in self.tags:
                raise Fault('rejected', 'core MIR missing/ended loan operand')
            if place:
                actual = self.place(frame, place)
                if actual.address != ref.address:
                    raise Fault('rejected', 'core MIR loan address disagreement')
            return ref
        raise Fault('unsupported', 'core MIR operand kind')

    def install_loan(self, frame, lid, ref):
        row = frame.body.loans.get(lid)
        if row is None or lid in frame.loans and frame.loans[lid].token in self.tags:
            raise Fault('rejected', 'core MIR missing/duplicate loan metadata')
        if field(row, 'conditions') != '[]' or field(row, 'additional-dependencies') != '[]' or number(row, 'two-phase'):
            raise Fault('unsupported', 'core MIR conditional/multi-origin/two-phase loan')
        mode = number(row, 'access')
        if mode > ref.access:
            raise Fault('rejected', 'core MIR loan permission escalation')
        created = self.borrow(ref, mode, escape=bool(number(row,'escapes')))
        frame.loans[lid] = created
        return created

    def destroy(self, value, owner_ref=None):
        if isinstance(value, CoreOwner) and value.address is not None:
            self.features.add('owned-allocation-free')
            ref = CoreReference(value.address,1)
            stored = self.access(ref,write=True,initialized=False)
            self.destroy(stored, ref)
            self.cleanup.append(('free', value.address.allocation, value.address.generation))
            del self.cells[value.address.allocation]
            self.free.append(value.address.allocation)
        elif isinstance(value, CoreRecord):
            cls = self.program.classes[value.cls]
            if cls['destructor']:
                if owner_ref is None:
                    raise Fault('unsupported','core MIR detached user destructor')
                _, failed = self.run(self.program.destructors[cls['destructor']], [], owner_ref)
                if failed:
                    raise Fault('unsupported','core MIR throwing destructor')
            for member in reversed(list(value.fields)):
                data = value.fields[member]
                if data != _UNINITIALIZED:
                    self.destroy(data, dataclasses.replace(owner_ref,address=dataclasses.replace(owner_ref.address,path=owner_ref.address.path+(member,))) if owner_ref else None)

    def instruction(self, frame, line):
        op = line.split()[1]
        args = operands(re.search(r' operands=(.*?) parameters=',line).group(1))
        result,dest,lid = number(line,'result'),number(line,'destination'),number(line,'loan')
        intrinsic = field(line,'intrinsic','none')
        if any(number(line,k,0) for k in ('tombstone','raw-memory','object-allocation')):
            raise Fault('unsupported','core MIR raw/tombstoned/allocation operation')
        value, failed = None,False
        if op in ('load','move','call-input'):
            if len(args)!=1: raise Fault('unsupported','core MIR unary operand shape')
            value = self.operand(frame,args[0])
            if lid:
                if not isinstance(value,CoreReference): raise Fault('rejected','core MIR loan input is not reference')
                value = self.install_loan(frame,lid,value)
            if dest:
                address = self.place(frame,dest,create=True).address
                self.set(address,value)
        elif op == 'compute':
            operation=field(line,'operation')
            result_type=frame.body.types.get(result)
            if result_type is None or result_type[0] not in (4,12,16):
                raise Fault('unsupported','core MIR compute result type/width outside int32/bool/null')
            argument_types=[int(k) for k in re.findall(r'operand\(\d+;value=\d+;place=\d+;loan=\d+;literal=[^;]*;type=type\((\d+);',re.search(r' operands=(.*?) parameters=',line).group(1))]
            if len(argument_types)!=len(args) or any(k not in (4,12) for k in argument_types):
                raise Fault('unsupported','core MIR compute operand type/width outside int32/bool')
            vals=[self.operand(frame,a) for a in args]
            if operation=='literal':
                literal_text=field(line,'literal')
                value=CoreOwner(None) if literal_text in ('nullptr','null') else literal(literal_text)
                if result_type[0]==4 and (type(value)is not int or not -(2**31)<=value<2**31):
                    raise Fault('unsupported','core MIR literal outside int32')
                if result_type[0]==12 and (type(value)is not int or value not in (0,1)):
                    raise Fault('rejected','core MIR invalid bool literal')
                if result_type[0]==16 and not isinstance(value,CoreOwner):
                    raise Fault('rejected','core MIR invalid null literal')
            elif operation in ('identity','convert') and len(vals)==1:
                if result_type[0] not in (4,12) or type(vals[0])is not int:
                    raise Fault('unsupported','core MIR conversion outside int32/bool')
                if operation=='identity' and result_type[0]!=argument_types[0]:
                    raise Fault('rejected','core MIR identity changes type')
                value=int(bool(vals[0])) if result_type[0]==12 else vals[0]
            elif all(type(v)is int for v in vals):
                if any(not -(2**31)<=v<2**31 for v in vals):
                    raise Fault('unsupported','core MIR integer operand outside int32')
                binary={'add':lambda a,b:a+b,'subtract':lambda a,b:a-b,'multiply':lambda a,b:a*b,
                    'divide':lambda a,b:int(a/b),'remainder':lambda a,b:a-int(a/b)*b,
                    'equal':lambda a,b:int(a==b),'not-equal':lambda a,b:int(a!=b),
                    'less':lambda a,b:int(a<b),'less-equal':lambda a,b:int(a<=b),
                    'greater':lambda a,b:int(a>b),'greater-equal':lambda a,b:int(a>=b)}
                if operation in binary and len(vals)==2:
                    arithmetic=operation in ('add','subtract','multiply','divide','remainder')
                    if arithmetic and (argument_types != [4,4] or result_type[0]!=4):
                        raise Fault('unsupported','core MIR arithmetic requires int32')
                    if operation in ('divide','remainder') and vals[1]==0:
                        failed=True
                        self.failure_cause='modulo_by_zero' if operation=='remainder' else 'division_by_zero'
                    else: value=binary[operation](*vals)
                elif operation=='negate' and len(vals)==1: value=-vals[0]
                elif operation=='logical-not' and len(vals)==1:value=int(not vals[0])
                else:raise Fault('unsupported','core MIR compute '+str(operation))
                if type(value)is int and not -(2**31)<=value<2**31:
                    failed=True
                    self.failure_cause='integer_overflow'
            else: raise Fault('unsupported','core MIR nonscalar compute')
        elif op in ('initialize','assign'):
            if len(args)!=1:raise Fault('unsupported','core MIR initialization shape')
            value=self.operand(frame,args[0])
            ref=self.place(frame,dest,create=op=='initialize')
            if op=='assign':self.access(ref,write=True,initialized=False)
            else:
                self.access(dataclasses.replace(ref,access=1) if not ref.token else ref,write=True,initialized=False)
            if op=='initialize' and self.get(ref.address,False)!=_UNINITIALIZED:
                previous=self.get(ref.address)
                if not isinstance(previous,CoreReference) or previous.token in self.tags:
                    raise Fault('rejected','core MIR double initialization')
            destination_type=frame.body.places[dest]['type']
            if destination_type[0] in (4,12):
                if type(value)is not int or not -(2**31)<=value<2**31:
                    raise Fault('rejected','core MIR scalar initialization type/range mismatch')
                if destination_type[0]==12 and value not in (0,1):
                    raise Fault('rejected','core MIR bool initialization range')
            if destination_type[0]==22 and not isinstance(value,CoreOwner):
                raise Fault('rejected','core MIR owner capability type mismatch')
            if destination_type[0]==19 and (not isinstance(value,CoreRecord) or self.program.classes[value.cls]['declaration']!=destination_type[1]):
                raise Fault('rejected','core MIR record initialization type mismatch')
            if frame.body.places[dest]['type'][0]==21:
                if not isinstance(value,CoreReference):raise Fault('rejected','core MIR initialized reference from scalar')
                if frame.body.places[dest]['type'][2]>value.access:raise Fault('rejected','core MIR reference permission upgrade')
            self.set(ref.address,value)
        elif op=='borrow':
            if len(args)!=1:raise Fault('unsupported','core MIR borrow shape')
            ref=self.operand(frame,args[0])
            if not isinstance(ref,CoreReference):raise Fault('rejected','core MIR borrowed value not address')
            value=self.install_loan(frame,lid,ref)
        elif op=='end-borrow':
            if lid not in frame.loans:raise Fault('rejected','core MIR unknown ended loan')
            self.end(frame.loans[lid].token)
        elif op=='call':
            vals=[self.operand(frame,a) for a in args]
            if intrinsic=='allocate-unique-owner':
                if len(vals)!=1 or type(vals[0])is not int:raise Fault('unsupported','core MIR allocation limited to scalar')
                self.features.add('owned-allocation')
                value=CoreOwner(self.allocate(vals[0],heap=True))
            elif intrinsic in ('unique-owner-borrow','unique-owner-borrow-mut'):
                if len(vals)!=1 or not isinstance(vals[0],CoreOwner):raise Fault('rejected','core MIR owner capability required')
                if vals[0].address is None:
                    failed=True
                    self.failure_cause='null_owner'
                else:value=self.install_loan(frame,lid,CoreReference(vals[0].address,int(intrinsic.endswith('-mut'))))
                lid=0
            elif intrinsic!='none':raise Fault('unsupported','core MIR unknown intrinsic '+str(intrinsic))
            else:
                fn=number(line,'function')
                if fn not in self.program.functions:raise Fault('unsupported','core MIR unknown call effects')
                receiver=field(line,'receiver')
                receiver=None if receiver=='-' else self.operand(frame,operands(receiver)[0])
                value,failed=self.run(self.program.functions[fn],vals,receiver)
            if lid and not failed:
                if not isinstance(value,CoreReference):raise Fault('rejected','core MIR scalar returned as reference')
                # Returned callee activation is transported, not recreated from
                # the caller's published origin row.
                row=frame.body.loans.get(lid)
                if row is None or number(row,'access')>value.access:
                    raise Fault('rejected','core MIR invalid returned loan mode')
                frame.loans[lid]=value
                self.tags[value.token]['escape']=bool(number(row,'escapes'))
        elif op=='construct':
            cid=number(line,'constructor')
            if cid not in self.program.constructors:raise Fault('unsupported','core MIR unknown constructor')
            body=self.program.constructors[cid]
            cls=number(body.head,'owner')
            layout=self.program.classes[cls]
            if any(t[0] not in (4,12,19,22) for t,a in layout['fields'].values()):
                raise Fault('unsupported','core MIR record field type outside owned core')
            if field(layout['line'],'bases')!='[]' or number(layout['line'],'polymorphic'):
                raise Fault('unsupported','core MIR polymorphic/base record')
            address=self.allocate(CoreRecord(cls,{k:_UNINITIALIZED for k in layout['fields']}))
            ref=CoreReference(address,1)
            _,failed=self.run(body,[self.operand(frame,a) for a in args],ref)
            value=self.get(address,not failed)
            del self.cells[address.allocation]
            self.free.append(address.allocation)
            self.features.add('record')
        elif op=='drop':
            ref=self.place(frame,dest)
            value=self.get(ref.address,False)
            if value!=_UNINITIALIZED:
                self.access(dataclasses.replace(ref,access=1) if not ref.token else ref,write=True,initialized=False)
                self.destroy(value,ref)
                self.set(ref.address,_UNINITIALIZED)
                self.cleanup.append(('drop',ref.address.allocation,ref.address.generation,ref.address.path))
        elif op=='lifecycle':
            if args or dest or result or lid:raise Fault('unsupported','core MIR lifecycle payload')
            # Lifecycle rows are accounting metadata. Actual move/drop/free
            # operations above decide storage validity; rows cannot justify it.
        else:raise Fault('unsupported','core MIR instruction '+op)
        if result and not failed:frame.values[result]=value
        return failed

    def run(self, body, args, receiver=None):
        identity=body.head.split(' declaration=')[0].split(' owner=')[0]
        if identity in self.active_calls:raise Fault('unsupported','core MIR recursion')
        if field(body.head,'definition')=='declaration':raise Fault('unsupported','core MIR external call')
        if len(args)!=len(body.params):raise Fault('rejected','core MIR call arity')
        self.active_calls.append(identity)
        # CoreRenderer reserves this structural observer signature: two mutable
        # scalar state references plus a by-value observed integer. Its actual
        # body still executes; this hook supplies an independent read sequence
        # so removing/misrouting observer calls cannot pass just via exit zero.
        signature=[body.places[body.roots[(0,b)]]['type'] for b in body.params]
        if body.head.startswith('function ') and receiver is None and len(signature)==3 and \
                [(t[0],t[2]) for t in signature]==[(21,1),(21,1),(4,0)]:
            if type(args[2])is not int:raise Fault('rejected','core MIR observation not integer')
            self.observations.append(args[2])
        self.frames+=1
        frame=CoreFrame(self.frames,body,receiver=receiver)
        if receiver is not None:
            frame.receiver=self.borrow(receiver,receiver.access,initialized=not body.head.startswith('constructor '))
            frame.input_tokens.append(frame.receiver.token)
        for binding,arg in zip(body.params,args):
            pid=body.roots.get((0,binding))
            if pid is None:raise Fault('rejected','core MIR missing parameter binding')
            typ=body.places[pid]['type']
            if typ[0] not in (4,12,19,21,22,25):raise Fault('unsupported','core MIR parameter type outside profile')
            if typ[0]==21:
                if not isinstance(arg,CoreReference):raise Fault('rejected','core MIR reference argument mismatch')
                if typ[2]>arg.access:raise Fault('rejected','core MIR mutable parameter from shared argument')
                arg=self.borrow(arg,typ[2])
                frame.input_tokens.append(arg.token)
            if typ[0]==25:arg=CorePack([arg])
            frame.storage[(0,binding)]=self.allocate(copy.deepcopy(arg))
        for lid,row in body.loans.items():
            if number(row,'entry',0):
                source=int(field(row,'source')[1:])
                p=body.places[source]
                value=self.get(frame.storage[(0,p['binding'])])
                if not isinstance(value,CoreReference):raise Fault('rejected','core MIR entry loan not parameter')
                borrowed=self.install_loan(frame,lid,value)
                self.set(frame.storage[(0,p['binding'])],borrowed)
        block,failed=body.entry,False
        pending_failure=False
        returned=None
        while True:
            if block not in body.blocks:raise Fault('rejected','core MIR missing block')
            instructions,term=body.blocks[block]
            for line in instructions:
                self.tick(identity+':bb'+str(block)+':'+line.split()[0])
                failed=self.instruction(frame,line)
                pending_failure=pending_failure or failed
                if failed and line is not instructions[-1]:raise Fault('rejected','core MIR failure without invocation edge')
            self.tick(identity+':bb'+str(block)+':term')
            kind=int(term.split()[1])
            successor=lambda name:int(field(term,name,'bb0')[2:])
            if kind==4:
                if not instructions or field(term,'invoke')!=instructions[-1].split()[0]:raise Fault('rejected','core MIR invocation producer mismatch')
                block=successor('else' if failed else 'target')
            elif kind in (1,11):
                if kind==11 and not pending_failure:raise Fault('rejected','core MIR failure continuation without failure')
                block=successor('target')
            elif kind==2:
                cond=operands(term)
                if len(cond)!=1:raise Fault('unsupported','core MIR branch operand')
                block=successor('target' if self.operand(frame,cond[0]) else 'else')
            elif kind in (5,6):
                if kind==6:
                    if not pending_failure:raise Fault('rejected','core MIR propagation without failure')
                    failed=True
                    self.features.add('failure-cleanup')
                else:
                    if pending_failure:raise Fault('rejected','core MIR failure returned normally')
                    values=operands(term)
                    returned=self.operand(frame,values[0]) if values else None
                break
            else:raise Fault('unsupported','core MIR terminator '+str(kind))
        # Callee local storage cannot escape, even with forged origin summaries.
        local_allocations={a.allocation for a in frame.storage.values()}
        if isinstance(returned,CoreReference):
            if returned.token not in self.tags:raise Fault('rejected','core MIR returned ended reference')
            self.cell(returned.address)
        if isinstance(returned,CoreReference) and returned.address.allocation in local_allocations:
            raise Fault('rejected','core MIR returned local storage')
        for ref in frame.loans.values():
            if ref.token in self.tags and (not isinstance(returned,CoreReference) or ref.token!=returned.token):
                # Parameter read borrows without entry-loan rows are represented
                # by the caller's argument token; explicit local rows must end.
                raise Fault('rejected','core MIR loan leaked from frame')
        for token in reversed(frame.input_tokens):
            if token in self.tags:self.end(token)
        for address in frame.storage.values():
            self.cell(address)
            if any(self.overlap(address,t['address']) for t in self.tags.values()):
                raise Fault('rejected','core MIR frame retired while borrowed')
            del self.cells[address.allocation]
            self.free.append(address.allocation)
        self.active_calls.pop()
        return returned,failed


def verify_core_mir_dump(text, *, max_steps=100000):
    """Execute the actual parameterless entry and all reached supported callees.

    Returns normal entry integer in observations. Additional attributes retain
    concrete allocation/cleanup traces and operation coverage. Unsupported is
    never reported as independently checked. Heap allocator success is a stated
    primitive assumption; null dereference and arithmetic failures are defined
    outcomes. Nonrecursive constructors/destructors run their actual MIR bodies.
    """
    machine=None
    try:
        if len(text)>64*1024*1024:raise Fault('timeout','core MIR byte budget')
        program=CoreProgram(text)
        machine=CoreMachine(program,max_steps)
        value,failed=machine.run(program.functions[program.entry],[])
        if machine.tags:raise Fault('rejected','core MIR active loan at program exit')
        if any(c['heap'] for c in machine.cells.values()):raise Fault('rejected','core MIR owned allocation leaked')
        if not failed and type(value)is not int:raise Fault('unsupported','core MIR nonscalar entry result')
        result=Result('accepted','defined-failure' if failed else None,machine.trace,[] if failed else [value],machine.steps,machine.frames)
    except Fault as error:
        result=Result(error.status,str(error),machine.trace if machine else [],[],machine.steps if machine else 0,machine.frames if machine else 0)
    except (KeyError,ValueError,TypeError,AttributeError,IndexError) as error:
        result=Result('unsupported','core MIR unsupported/malformed shape: '+str(error))
    # Keep stable Result API; callers can request the detailed profile evidence.
    result.core_evidence={'profile':'concrete-core-v1','features':sorted(machine.features) if machine else [],
        'allocations':machine.allocations if machine else [],'cleanup':machine.cleanup if machine else [],
        'observations':machine.observations if machine else [],
        'failure_cause':machine.failure_cause if machine else None}
    return result
