"""Restricted independent borrow analysis and concrete memory machine.

The analysis and interpreter have separate states and transition code. No GTI
compiler decision routines are imported. This is not a formal proof.
"""
from __future__ import annotations
import copy
import dataclasses
import json
import re

SCHEMA = 1
SUPPORTED_OPS = frozenset({'alloc', 'borrow', 'end', 'read', 'write', 'move',
                           'init', 'drop', 'if', 'loop', 'scope', 'fail', 'call', 'return'})
MUTATIONS = frozenset({'overlap', 'owner_liveness', 'escape', 'init', 'cleanup'})


@dataclasses.dataclass
class Result:
    status: str
    violation: str | None = None
    trace: list[str] = dataclasses.field(default_factory=list)
    observations: list[int] = dataclasses.field(default_factory=list)
    steps: int = 0
    states: int = 0

    def to_dict(self):
        return dataclasses.asdict(self)


class Fault(Exception):
    def __init__(self, status, text):
        super().__init__(text)
        self.status = status


def parts(text):
    if not isinstance(text, str) or not text or any(not p for p in text.split('.')):
        raise Fault('unsupported', 'place: expected static dotted name')
    path = text.split('.')
    if len(path) > 2:
        raise Fault('unsupported', 'place: one record projection supported')
    return path[0], tuple(path[1:])


def validate(program):
    if not isinstance(program, dict) or program.get('schema') != SCHEMA:
        raise Fault('unsupported', 'schema: expected version 1')
    if set(program) - {'schema', 'body', 'functions', 'name', 'metadata'}:
        raise Fault('unsupported', 'schema: unknown program fields')
    functions = program.get('functions', {})
    if not isinstance(functions, dict):
        raise Fault('unsupported', 'schema: functions must be an object')
    allowed = {
        'alloc': {'op', 'name', 'fields', 'mutable'},
        'borrow': {'op', 'name', 'target', 'mode'}, 'end': {'op', 'name'},
        'read': {'op', 'target'}, 'write': {'op', 'target', 'value'},
        'init': {'op', 'target', 'value'}, 'move': {'op', 'source', 'name'},
        'drop': {'op', 'target'}, 'if': {'op', 'condition', 'then', 'else'},
        'loop': {'op', 'condition', 'iterations', 'body'},
        'scope': {'op', 'body', 'cleanup'}, 'fail': {'op'},
        'call': {'op', 'function', 'args', 'name'}, 'return': {'op', 'target'},
    }
    def identifier(text):
        return isinstance(text, str) and re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', text)
    def body(nodes):
        if not isinstance(nodes, list):
            raise Fault('unsupported', 'schema: statement list required')
        for node in nodes:
            if not isinstance(node, dict) or node.get('op') not in SUPPORTED_OPS:
                raise Fault('unsupported', 'operation: unknown statement')
            if set(node) - allowed[node['op']]:
                raise Fault('unsupported', 'schema: unknown operation fields')
            for key in ('name', 'function'):
                if key in node and not identifier(node[key]):
                    raise Fault('unsupported', 'schema: invalid binding name')
            for key in ('target', 'source'):
                if key in node and (not isinstance(node[key], str) or
                    not all(identifier(x) for x in node[key].split('.'))):
                    raise Fault('unsupported', 'schema: invalid place')
            if 'condition' in node and not isinstance(node['condition'], (bool, str)):
                raise Fault('unsupported', 'condition: boolean or input name required')
            if 'iterations' in node and (not isinstance(node['iterations'], int) or node['iterations'] < 0):
                raise Fault('unsupported', 'loop: nonnegative iterations required')
            if 'value' in node and not isinstance(node['value'], int):
                raise Fault('unsupported', 'value: integer required')
            if 'mutable' in node and not isinstance(node['mutable'], bool):
                raise Fault('unsupported', 'mutable: boolean required')
            if 'args' in node and (not isinstance(node['args'], list) or any(
                not isinstance(a, (str, int)) for a in node['args'])):
                raise Fault('unsupported', 'call: integer or place arguments required')
            for key in ('body', 'then', 'else', 'cleanup'):
                if key in node:
                    body(node[key])
            if node['op'] == 'call' and node.get('function') not in functions:
                raise Fault('unsupported', 'unknown-call: ' + str(node.get('function')))
            if node['op'] == 'borrow' and node.get('mode') not in ('shared', 'mut'):
                raise Fault('unsupported', 'permission: expected shared or mut')
            if node['op'] == 'alloc':
                fields = node.get('fields', {'value': 0})
                if not isinstance(fields, dict) or not fields or not all(
                    identifier(k) and isinstance(v, int)
                    for k, v in fields.items()
                ):
                    raise Fault('unsupported', 'allocation: nonempty scalar fields required')
    body(program.get('body'))
    for fn_name, fn in functions.items():
        if not identifier(fn_name):
            raise Fault('unsupported', 'function: invalid name')
        if not isinstance(fn, dict):
            raise Fault('unsupported', 'function declaration required')
        if set(fn) - {'params', 'body'}:
            raise Fault('unsupported', 'function: unknown declaration fields')
        params = fn.get('params', [])
        if not isinstance(params, list) or any(not isinstance(p, dict) or
            not identifier(p.get('name')) or set(p) - {'name', 'mode'} or
            p.get('mode') not in ('shared', 'mut') for p in params):
            raise Fault('unsupported', 'function: named reference parameters required')
        if len({p['name'] for p in params}) != len(params):
            raise Fault('unsupported', 'function: duplicate parameter')
        body(fn.get('body', []))
    def callees(nodes):
        for node in nodes:
            if node['op'] == 'call':
                yield node['function']
            for key in ('body', 'then', 'else', 'cleanup'):
                yield from callees(node.get(key, []))
    visited = set()
    def visit(name, active):
        if name in active:
            raise Fault('unsupported', 'recursive-call: ' + name)
        if name in visited:
            return
        for callee in callees(functions[name].get('body', [])):
            visit(callee, active | {name})
        visited.add(name)
    for name in functions:
        visit(name, set())


# Static abstract state: sets of definitely initialized fields, owner liveness,
# and loans. Unknown control flow is explored to a finite powerset fixed point.
@dataclasses.dataclass
class State:
    owners: dict = dataclasses.field(default_factory=dict)
    loans: dict = dataclasses.field(default_factory=dict)
    outcome: str = 'normal'
    returned: str | None = None

    def key(self):
        return json.dumps(dataclasses.asdict(self), sort_keys=True)


class Analysis:
    def __init__(self, program, max_states, max_steps, mutation):
        self.program, self.max_states, self.max_steps = program, max_states, max_steps
        self.mutation = mutation
        self.steps = self.states = 0
        self.trace, self.calls = [], []
        self.prefix, self.aliases = '', {}

    def name(self, raw):
        name, path = parts(raw)
        return self.aliases.get(name, self.prefix + name) + ('.' + path[0] if path else '')

    def tick(self, node):
        self.steps += 1
        self.trace.append(node['op'] + ' ' + str(node.get('target', node.get('name', ''))))
        self.trace = self.trace[-64:]
        if self.steps > self.max_steps:
            raise Fault('timeout', 'analysis transition budget exceeded')

    def place(self, state, raw, qualified=False):
        name, path = parts(raw if qualified else self.name(raw))
        parent = None
        if name in state.loans:
            loan = state.loans[name]
            if not loan['active']:
                raise Fault('rejected', 'ended-loan: ' + raw)
            parent, name, path = name, loan['owner'], tuple(loan['path']) + path
        obj = state.owners.get(name)
        if not obj or not obj['alive']:
            raise Fault('rejected', 'dead-storage: ' + raw)
        if len(path) > 1 or (path and path[0] not in obj['fields']):
            raise Fault('unsupported', 'place: unknown or nested field ' + raw)
        return name, path, parent

    @staticmethod
    def overlap(a, p, b, q):
        return a == b and (not p or not q or p == q)

    def access(self, state, raw, write=False, initialized=True):
        owner, path, via = self.place(state, raw)
        obj = state.owners[owner]
        wanted = set(path) if path else set(obj['fields'])
        if initialized and self.mutation != 'init' and not wanted.issubset(obj['initialized']):
            raise Fault('rejected', 'uninitialized: ' + raw)
        if write and (not obj['mutable'] or via and state.loans[via]['mode'] != 'mut'):
            raise Fault('rejected', 'read-only: ' + raw)
        ancestors, parent = set(), via
        while parent:
            if parent in ancestors:
                raise Fault('rejected', 'cyclic-parent')
            ancestors.add(parent)
            parent = state.loans[parent]['parent']
        if self.mutation != 'overlap':
            for name, loan in state.loans.items():
                if loan['active'] and name not in ancestors and self.overlap(
                    owner, path, loan['owner'], tuple(loan['path'])
                ) and (write or loan['mode'] == 'mut'):
                    raise Fault('rejected', 'borrow-conflict: ' + raw)
        return owner, path, via

    def unique(self, states):
        unique = {s.key(): s for s in states}
        self.states = max(self.states, len(unique))
        if len(unique) > self.max_states:
            raise Fault('timeout', 'analysis state budget exceeded')
        return list(unique.values())

    def sequence(self, nodes, states):
        for node in nodes:
            states = self.unique([out for state in states for out in (
                [state] if state.outcome != 'normal' else self.step(node, state))])
        return states

    def step(self, node, state):
        self.tick(node)
        op = node['op']
        if op == 'if':
            cond = node.get('condition', '*')
            arms = ['then'] if cond is True else ['else'] if cond is False else ['then', 'else']
            return self.unique([out for arm in arms for out in self.sequence(
                node.get(arm, []), [copy.deepcopy(state)])])
        if op == 'loop':
            if node.get('condition') is False:
                return [state]
            frontier, visited, exits = [state], set(), []
            while frontier:
                entry = frontier.pop()
                if entry.key() in visited:
                    continue
                visited.add(entry.key())
                self.states = max(self.states, len(visited))
                if len(visited) > self.max_states:
                    raise Fault('timeout', 'loop fixed-point state budget exceeded')
                if node.get('condition', '*') is not True:
                    exits.append(copy.deepcopy(entry))
                for out in self.sequence(node.get('body', []), [copy.deepcopy(entry)]):
                    (frontier if out.outcome == 'normal' else exits).append(out)
            return self.unique(exits)
        if op == 'scope':
            results = self.sequence(node.get('body', []), [state])
            out = []
            for result in results:
                previous = result.outcome, result.returned
                result.outcome, result.returned = 'normal', None
                for cleaned in self.sequence(node.get('cleanup', []), [result]):
                    if cleaned.outcome == 'normal':
                        cleaned.outcome, cleaned.returned = previous
                    out.append(cleaned)
            return out
        if op == 'call':
            return self.call(node, state)
        if op == 'alloc':
            name = self.name(node['name'])
            if name in state.owners and state.owners[name]['alive']:
                raise Fault('rejected', 'double-initialization: ' + name)
            if any(l['active'] and l['owner'] == name for l in state.loans.values()):
                raise Fault('rejected', 'stale-generation: ' + name)
            fields = list(node.get('fields', {'value': 0}))
            state.owners[name] = {'alive': True, 'fields': fields,
                'initialized': fields.copy(), 'mutable': node.get('mutable', True)}
        elif op == 'borrow':
            name = self.name(node['name'])
            if name in state.loans and state.loans[name]['active']:
                raise Fault('rejected', 'reference-rebinding: ' + name)
            owner, path, parent = self.access(state, node['target'], node['mode'] == 'mut')
            state.loans[name] = {'owner': owner, 'path': path, 'parent': parent,
                                 'mode': node['mode'], 'active': True}
        elif op == 'end':
            name = self.name(node['name'])
            if name not in state.loans or not state.loans[name]['active']:
                raise Fault('rejected', 'ended-loan: ' + name)
            for child_name, child in state.loans.items():
                if child_name.endswith('@return@') and child['active'] and child['parent'] == name:
                    child['parent'] = state.loans[name]['parent']
            if self.mutation != 'cleanup' and any(l['active'] and l['parent'] == name
                                                  for l in state.loans.values()):
                raise Fault('rejected', 'live-child: ' + name)
            state.loans[name]['active'] = False
        elif op in ('read', 'write', 'init'):
            root, projection = parts(self.name(node['target']))
            if op == 'init' and not projection and root in state.owners and not state.owners[root]['alive']:
                if any(l['active'] and l['owner'] == root for l in state.loans.values()):
                    raise Fault('rejected', 'owner-borrowed: ' + root)
                state.owners[root]['alive'] = True
                state.owners[root]['initialized'] = []
            owner, path, _ = self.access(state, node['target'], op != 'read', op != 'init')
            if op == 'init':
                obj = state.owners[owner]
                obj['initialized'] = sorted(set(obj['initialized']) |
                                            (set(path) if path else set(obj['fields'])))
        elif op in ('drop', 'move'):
            target = node['source'] if op == 'move' else node['target']
            owner, path, via = self.place(state, target)
            if via:
                raise Fault('rejected', 'borrowed-owner-transition: ' + target)
            if self.mutation != 'owner_liveness' and any(l['active'] and self.overlap(
                owner, path, l['owner'], tuple(l['path'])) for l in state.loans.values()):
                raise Fault('rejected', 'owner-borrowed: ' + target)
            obj = state.owners[owner]
            selected = set(path) if path else set(obj['fields'])
            if not selected.issubset(obj['initialized']):
                raise Fault('rejected', 'uninitialized: ' + target)
            if op == 'move':
                name = self.name(node['name'])
                if name in state.owners and state.owners[name]['alive']:
                    raise Fault('rejected', 'occupied-move-target: ' + name)
                fields = ['value'] if path else obj['fields'].copy()
                state.owners[name] = {'alive': True, 'fields': fields,
                                      'initialized': fields.copy(), 'mutable': obj['mutable']}
            if path:
                obj['initialized'] = sorted(set(obj['initialized']) - selected)
            else:
                obj['alive'] = False
        elif op == 'fail':
            state.outcome = 'failure'
        elif op == 'return':
            if not self.calls:
                raise Fault('unsupported', 'return outside function')
            owner, path, via = self.access(state, node['target'])
            name = self.prefix + '@return@'
            mode = state.loans[via]['mode'] if via else 'shared'
            state.loans[name] = {'owner': owner, 'path': path, 'parent': via,
                                'mode': mode, 'active': True}
            state.outcome, state.returned = 'return', name
        return [state]

    def call(self, node, state):
        name = node['function']
        fn = self.program.get('functions', {}).get(name)
        if fn is None:
            raise Fault('unsupported', 'unknown-call: ' + name)
        if name in self.calls:
            raise Fault('unsupported', 'recursive-call: ' + name)
        args, params = node.get('args', []), fn.get('params', [])
        if len(args) != len(params):
            raise Fault('unsupported', 'call-arity: ' + name)
        old_prefix, old_aliases = self.prefix, self.aliases
        prefix = self.prefix + '@call_' + name + '@'
        origins, mapping, actuals, temps = [], {}, [], []
        for i, (arg, param) in enumerate(zip(args, params)):
            if isinstance(arg, int):
                if param['mode'] != 'shared':
                    raise Fault('rejected', 'mutable-temporary: ' + name)
                arg = '@temporary_' + str(i)
                self.step({'op': 'alloc', 'name': arg, 'fields': {'value': args[i]}}, state)
                temps.append(self.name(arg))
            actuals.append(self.name(arg))
            parameter = prefix + 'arg' + str(i)
            self.step({'op': 'borrow', 'name': parameter[len(old_prefix):],
                       'target': arg, 'mode': param['mode']}, state)
            loan = state.loans[parameter]
            origins.append((loan['owner'], tuple(loan['path']), param['mode']))
            mapping[param['name']] = parameter
        self.prefix, self.aliases = prefix, mapping
        self.calls.append(name)
        try:
            results = self.sequence(fn.get('body', []), [state])
        finally:
            self.calls.pop()
            self.prefix, self.aliases = old_prefix, old_aliases
        for result in results:
            ret_data = None
            if result.outcome == 'return':
                owner, path, via = self.place(result, result.returned, qualified=True)
                mode = result.loans[via]['mode'] if via else 'shared'
                ret_data = (owner, path, mode)
                if self.mutation != 'escape' and (owner in temps or not any(
                    owner == x[0] and (not x[1] or path[:len(x[1])] == x[1]) for x in origins
                )):
                    raise Fault('rejected', 'escaping-temporary: ' + name if owner in temps
                                else 'escaping-local: ' + name)
                result.outcome, result.returned = 'normal', None
            elif node.get('name') and result.outcome == 'normal':
                raise Fault('rejected', 'missing-return: ' + name)
            for key in list(result.loans):
                if key.startswith(prefix):
                    del result.loans[key]
            for key in list(result.owners):
                if key.startswith(prefix) or key in temps:
                    del result.owners[key]
            if ret_data and node.get('name'):
                existing = result.loans.get(self.name(node['name']))
                if existing and existing['active']:
                    raise Fault('rejected', 'reference-rebinding: ' + node['name'])
                matching = next((i for i, x in enumerate(origins) if x[0] == ret_data[0]
                                 and (not x[1] or ret_data[1][:len(x[1])] == x[1])), None)
                parent = parts(actuals[matching])[0] if matching is not None else None
                if parent not in result.loans:
                    parent = None
                result.loans[self.name(node['name'])] = {'owner': ret_data[0], 'path': ret_data[1],
                    'mode': ret_data[2], 'parent': parent, 'active': True}
        return results


def check(program, *, max_states=4096, max_steps=100000, mutation=None):
    analysis = Analysis(program, max_states, max_steps, mutation)
    try:
        validate(program)
        if mutation is not None and mutation not in MUTATIONS:
            raise Fault('unsupported', 'unknown mutation')
        analysis.sequence(program['body'], [State()])
        return Result('accepted', trace=analysis.trace, steps=analysis.steps, states=analysis.states)
    except Fault as error:
        return Result(error.status, str(error), analysis.trace, steps=analysis.steps, states=analysis.states)
    except RecursionError:
        return Result('timeout', 'analysis nesting budget exceeded', analysis.trace, steps=analysis.steps)
    except (KeyError, TypeError, ValueError, AttributeError) as error:
        return Result('unsupported', 'malformed-input: ' + str(error), analysis.trace, steps=analysis.steps)


# Concrete state deliberately uses allocation ids/generations and memory cells,
# not the static state's owner/initialization abstraction or transfer routines.
class Machine:
    def __init__(self, program, inputs, max_steps, mutation):
        self.program, self.inputs, self.limit, self.mutation = program, inputs, max_steps, mutation
        self.heap, self.bindings, self.refs = {}, {}, {}
        self.next_id, self.steps = 1, 0
        self.trace, self.values, self.stack = [], [], []
        self.prefix, self.params = '', {}

    def binding(self, raw):
        name, field = parts(raw)
        return self.params.get(name, self.prefix + name), field

    def tick(self, node):
        self.steps += 1
        self.trace.append(node['op'] + ' ' + str(node.get('target', node.get('name', ''))))
        self.trace = self.trace[-64:]
        if self.steps > self.limit:
            raise Fault('timeout', 'execution step budget exceeded')

    def address(self, raw, write=False, initialized=True):
        name, field = self.binding(raw)
        tag = self.refs.get(name)
        if tag:
            if not tag['valid']:
                raise Fault('rejected', 'ended-loan: ' + raw)
            allocation, generation = tag['allocation'], tag['generation']
            selected = tuple(tag['fields']) + field
            lineage, parent = {name}, tag['parent']
            while parent:
                if parent in lineage:
                    raise Fault('rejected', 'cyclic-parent')
                lineage.add(parent)
                parent = self.refs[parent]['parent']
        elif name in self.bindings:
            allocation = self.bindings[name]
            generation = self.heap[allocation]['generation']
            selected, lineage = field, set()
        else:
            raise Fault('rejected', 'dead-storage: ' + raw)
        obj = self.heap[allocation]
        if not obj['live'] or generation != obj['generation']:
            raise Fault('rejected', 'stale-generation: ' + raw)
        if len(selected) > 1 or (selected and selected[0] not in obj['cells']):
            raise Fault('unsupported', 'place: unknown or nested field ' + raw)
        cells = set(selected) if selected else set(obj['cells'])
        if initialized and self.mutation != 'init' and any(obj['cells'][c] is None for c in cells):
            raise Fault('rejected', 'uninitialized: ' + raw)
        if write and (not obj['mutable'] or tag and tag['permission'] != 'mut'):
            raise Fault('rejected', 'read-only: ' + raw)
        if self.mutation != 'overlap':
            for other_name, other in self.refs.items():
                if other['valid'] and other_name not in lineage and other['allocation'] == allocation:
                    other_cells = set(other['fields']) if other['fields'] else set(obj['cells'])
                    if cells & other_cells and (write or other['permission'] == 'mut'):
                        raise Fault('rejected', 'borrow-conflict: ' + raw)
        return allocation, tuple(selected), name if tag else None

    def sequence(self, nodes):
        for node in nodes:
            result = self.step(node)
            if result:
                return result
        return None

    def step(self, node):
        self.tick(node)
        op = node['op']
        if op == 'alloc':
            name, field = self.binding(node['name'])
            if field:
                raise Fault('unsupported', 'allocation: root name required')
            if name in self.bindings and self.heap[self.bindings[name]]['live']:
                raise Fault('rejected', 'double-initialization: ' + name)
            allocation = self.bindings.get(name, self.next_id)
            if allocation == self.next_id:
                self.next_id += 1
            generation = self.heap.get(allocation, {}).get('generation', 0) + 1
            self.heap[allocation] = {'live': True, 'generation': generation,
                'mutable': node.get('mutable', True), 'cells': dict(node.get('fields', {'value': 0}))}
            self.bindings[name] = allocation
        elif op == 'borrow':
            name, _ = self.binding(node['name'])
            if name in self.refs and self.refs[name]['valid']:
                raise Fault('rejected', 'reference-rebinding: ' + name)
            allocation, fields, parent = self.address(node['target'], node['mode'] == 'mut')
            self.refs[name] = {'allocation': allocation, 'generation': self.heap[allocation]['generation'],
                'fields': fields, 'parent': parent, 'permission': node['mode'], 'valid': True}
        elif op == 'end':
            name, _ = self.binding(node['name'])
            if name not in self.refs or not self.refs[name]['valid']:
                raise Fault('rejected', 'ended-loan: ' + name)
            for child_name, child in self.refs.items():
                if child_name.endswith('@return@') and child['valid'] and child['parent'] == name:
                    child['parent'] = self.refs[name]['parent']
            if self.mutation != 'cleanup' and any(r['valid'] and r['parent'] == name for r in self.refs.values()):
                raise Fault('rejected', 'live-child: ' + name)
            self.refs[name]['valid'] = False
        elif op in ('read', 'write', 'init'):
            root, projection = self.binding(node['target'])
            if op == 'init' and not projection and root in self.bindings and not self.heap[self.bindings[root]]['live']:
                slot = self.bindings[root]
                if any(r['valid'] and r['allocation'] == slot for r in self.refs.values()):
                    raise Fault('rejected', 'owner-borrowed: ' + root)
                self.heap[slot]['live'] = True
                self.heap[slot]['generation'] += 1
                self.heap[slot]['cells'] = {f: None for f in self.heap[slot]['cells']}
            allocation, fields, _ = self.address(node['target'], op != 'read', op != 'init')
            obj = self.heap[allocation]
            selected = list(fields) if fields else list(obj['cells'])
            if op == 'read':
                self.values.extend(obj['cells'][f] for f in selected)
            else:
                for field in selected:
                    obj['cells'][field] = node.get('value', 0)
        elif op in ('drop', 'move'):
            raw = node['source'] if op == 'move' else node['target']
            name, selected = self.binding(raw)
            if name in self.refs:
                raise Fault('rejected', 'borrowed-owner-transition: ' + raw)
            if name not in self.bindings or not self.heap[self.bindings[name]]['live']:
                raise Fault('rejected', 'dead-storage: ' + raw)
            allocation = self.bindings[name]
            obj = self.heap[allocation]
            fields = set(selected) if selected else set(obj['cells'])
            if not fields.issubset(obj['cells']):
                raise Fault('unsupported', 'place: unknown field ' + raw)
            if any(obj['cells'][f] is None for f in fields):
                raise Fault('rejected', 'uninitialized: ' + raw)
            if self.mutation != 'owner_liveness':
                for ref in self.refs.values():
                    borrowed = set(ref['fields']) if ref['fields'] else set(obj['cells'])
                    if ref['valid'] and ref['allocation'] == allocation and fields & borrowed:
                        raise Fault('rejected', 'owner-borrowed: ' + raw)
            if op == 'move':
                target, _ = self.binding(node['name'])
                if target in self.bindings and self.heap[self.bindings[target]]['live']:
                    raise Fault('rejected', 'occupied-move-target: ' + target)
                destination, self.next_id = self.next_id, self.next_id + 1
                cells = {'value': obj['cells'][selected[0]]} if selected else dict(obj['cells'])
                self.heap[destination] = {'live': True, 'generation': 1,
                    'mutable': obj['mutable'], 'cells': cells}
                self.bindings[target] = destination
            if selected:
                for field in selected:
                    obj['cells'][field] = None
            else:
                obj['live'] = False
        elif op == 'if':
            condition = node.get('condition', '*')
            selected = condition if isinstance(condition, bool) else bool(self.inputs.get(condition, False))
            return self.sequence(node.get('then' if selected else 'else', []))
        elif op == 'loop':
            condition = node.get('condition', '*')
            if condition is False:
                return None
            count = node.get('iterations', self.inputs.get(str(condition), 0))
            if condition is True:
                count = self.limit + 1
            if not isinstance(count, int) or count < 0:
                raise Fault('unsupported', 'loop: nonnegative iteration count required')
            for _ in range(count):
                self.tick({'op': 'loop-iteration'})
                result = self.sequence(node.get('body', []))
                if result:
                    return result
        elif op == 'scope':
            result = self.sequence(node.get('body', []))
            cleaned = self.sequence(node.get('cleanup', []))
            return cleaned or result
        elif op == 'fail':
            return 'failure', None
        elif op == 'return':
            if not self.stack:
                raise Fault('unsupported', 'return outside function')
            allocation, fields, via = self.address(node['target'])
            permission = self.refs[via]['permission'] if via else 'shared'
            self.refs[self.prefix + '@return@'] = {
                'allocation': allocation, 'generation': self.heap[allocation]['generation'],
                'fields': fields, 'permission': permission, 'parent': via, 'valid': True}
            return 'return', (allocation, self.heap[allocation]['generation'], fields, permission)
        elif op == 'call':
            return self.call(node)
        return None

    def call(self, node):
        name = node['function']
        fn = self.program.get('functions', {}).get(name)
        if fn is None:
            raise Fault('unsupported', 'unknown-call: ' + name)
        if name in self.stack:
            raise Fault('unsupported', 'recursive-call: ' + name)
        args, params = node.get('args', []), fn.get('params', [])
        if len(args) != len(params):
            raise Fault('unsupported', 'call-arity: ' + name)
        old_prefix, old_params = self.prefix, self.params
        prefix = self.prefix + '@call_' + name + '@'
        mapping, origins, actuals, temps = {}, [], [], []
        for i, (arg, param) in enumerate(zip(args, params)):
            if isinstance(arg, int):
                if param['mode'] != 'shared':
                    raise Fault('rejected', 'mutable-temporary: ' + name)
                arg = '@temporary_' + str(i)
                self.step({'op': 'alloc', 'name': arg, 'fields': {'value': args[i]} })
                temps.append(self.binding(arg)[0])
            actuals.append(self.binding(arg)[0])
            allocation, fields, parent = self.address(arg, param['mode'] == 'mut')
            key = prefix + 'arg' + str(i)
            self.refs[key] = {'allocation': allocation, 'generation': self.heap[allocation]['generation'],
                'fields': fields, 'permission': param['mode'], 'parent': parent, 'valid': True}
            origins.append((allocation, fields))
            mapping[param['name']] = key
        self.stack.append(name)
        self.prefix, self.params = prefix, mapping
        try:
            outcome = self.sequence(fn.get('body', []))
        finally:
            self.stack.pop()
            self.prefix, self.params = old_prefix, old_params
        returned = outcome[1] if outcome and outcome[0] == 'return' else None
        matching = None
        if returned:
            matching = next((i for i, origin in enumerate(origins) if origin[0] == returned[0]
                             and (not origin[1] or returned[2][:len(origin[1])] == origin[1])), None)
            if self.mutation != 'escape' and (matching is None or any(
                self.bindings[t] == returned[0] for t in temps
            )):
                raise Fault('rejected', 'escaping-temporary: ' + name if matching is not None
                            else 'escaping-local: ' + name)
        elif node.get('name') and not outcome:
            raise Fault('rejected', 'missing-return: ' + name)
        for key in list(self.refs):
            if key.startswith(prefix):
                del self.refs[key]
        for key in list(self.bindings):
            if key.startswith(prefix) or key in temps:
                self.heap[self.bindings[key]]['live'] = False
                del self.bindings[key]
        if returned and node.get('name'):
            key, _ = self.binding(node['name'])
            if key in self.refs and self.refs[key]['valid']:
                raise Fault('rejected', 'reference-rebinding: ' + node['name'])
            parent = actuals[matching] if matching is not None and actuals[matching] in self.refs else None
            self.refs[key] = {'allocation': returned[0], 'generation': returned[1], 'fields': returned[2],
                'permission': returned[3], 'parent': parent, 'valid': True}
        return outcome if outcome and outcome[0] == 'failure' else None


def execute(program, *, inputs=None, max_steps=10000, mutation=None):
    machine = Machine(program, inputs or {}, max_steps, mutation)
    try:
        validate(program)
        if mutation is not None and mutation not in MUTATIONS:
            raise Fault('unsupported', 'unknown mutation')
        outcome = machine.sequence(program['body'])
        return Result('accepted', 'defined-failure' if outcome and outcome[0] == 'failure' else None,
                      machine.trace, machine.values, machine.steps)
    except Fault as error:
        return Result(error.status, str(error), machine.trace, machine.values, machine.steps)
    except RecursionError:
        return Result('timeout', 'execution nesting budget exceeded', machine.trace, machine.values, machine.steps)
    except (KeyError, TypeError, ValueError, AttributeError) as error:
        return Result('unsupported', 'malformed-input: ' + str(error), machine.trace, machine.values, machine.steps)
