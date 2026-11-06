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
                if not typ or int(typ.group(1)) not in (2, 4, 21):
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
