"""Restricted model-to-GTI translation; an experimental boundary, not a proof.

Allocations map to local storage, drop to explicit consuming transfer. This tests
source ownership, not native heap reclamation. Unsupported mappings are reported.
"""
from __future__ import annotations
import copy
import re


class Unsupported(ValueError):
    pass


class Renderer:
    def __init__(self, program, inputs):
        self.program, self.inputs = program, inputs
        self.types, self.counter = {}, 0
        self.parents = {}

    def name(self, name):
        if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
            raise Unsupported('unsupported identifier')
        return 'labv_' + name

    def fresh(self):
        self.counter += 1
        return 'labn_' + str(self.counter)

    def type(self, fields):
        if tuple(fields) == ('value',):
            return 'int'
        key = tuple(fields)
        if key not in self.types:
            self.types[key] = 'LabRecord' + str(len(self.types))
        return self.types[key]

    def place(self, target, env):
        if not isinstance(target, str):
            raise Unsupported('non-static place')
        root, dot, field = target.partition('.')
        if root not in env:
            raise Unsupported('unbound source name: ' + root)
        code, fields, mode, ended = env[root]
        if ended:
            raise Unsupported('source would reuse an explicitly ended reference: ' + root)
        if dot:
            if field not in fields:
                raise Unsupported('unknown field')
            return (code if fields == ('value',) else code + '.' + self.name(field), ('value',), mode)
        return code, fields, mode

    def leaves(self, code, fields):
        return [code] if fields == ('value',) else [code + '.' + self.name(f) for f in fields]

    def emit(self, nodes, env):
        lines = []
        for node in nodes:
            op = node['op']
            if op == 'alloc':
                if node['name'] in env:
                    raise Unsupported('allocation-name reuse needs explicit lexical storage adapter')
                fields = tuple(node.get('fields', {'value': 0}))
                values = list(node.get('fields', {'value': 0}).values())
                if any(type(v) is not int or not -100000 <= v <= 100000 for v in values):
                    raise Unsupported('source integer range')
                code = self.name(node['name']) + '_' + self.fresh()
                env[node['name']] = (code, fields, 'owner', False)
                initial = str(values[0]) if fields == ('value',) else self.type(fields) + '(' + ', '.join(map(str, values)) + ')'
                lines.append(('mut ' if node.get('mutable', True) else '') + self.type(fields) + ' ' + code + ' = ' + initial + ';')
            elif op == 'borrow':
                code, fields, target_mode = self.place(node['target'], env)
                name = self.name(node['name']) + '_' + self.fresh()
                if node['name'] in env and not env[node['name']][3]:
                    raise Unsupported('overlapping source declarations of a reference')
                mode = node['mode']
                lines.append(('mut ' if mode == 'mut' else '') + self.type(fields) + '& ' + name + ' = ' + code + ';')
                env[node['name']] = (name, fields, mode, False)
                if target_mode != 'owner':
                    self.parents[name] = code
            elif op == 'end':
                code, fields, mode = self.place(node['name'], env)
                if mode == 'owner':
                    raise Unsupported('end is not a reference')
                if any(not item[3] and self.parents.get(item[0]) == code for item in env.values()):
                    raise Unsupported('GTI retains parent through child; cannot express premature parent end')
                # Explicitly anchor GTI's last-use lifetime at the model endpoint.
                lines.extend('lab_keep(' + leaf + ');' for leaf in self.leaves(code, fields))
                env[node['name']] = (code, fields, mode, True)
            elif op in ('read', 'write', 'init'):
                code, fields, _ = self.place(node['target'], env)
                for leaf in self.leaves(code, fields):
                    if op == 'read':
                        lines.append('std::println(' + leaf + ');')
                    else:
                        value = node.get('value', 0)
                        if type(value) is not int or not -100000 <= value <= 100000:
                            raise Unsupported('source integer range')
                        lines.append(leaf + ' = ' + str(value) + ';')
            elif op in ('drop', 'move'):
                code, fields, mode = self.place(node['source'] if op == 'move' else node['target'], env)
                if mode != 'owner':
                    raise Unsupported('source cannot explicitly move the referent of a reference')
                if op == 'drop':
                    lines.append('lab_drop(std::move(' + code + '));')
                else:
                    name = self.name(node['name']) + '_' + self.fresh()
                    if node['name'] in env:
                        raise Unsupported('source move destination already declared')
                    env[node['name']] = (name, fields, 'owner', False)
                    lines.append('mut ' + self.type(fields) + ' ' + name + ' = std::move(' + code + ');')
            elif op == 'if':
                cond = node.get('condition', '*')
                if isinstance(cond, bool):
                    expression = str(cond).lower()
                elif cond == 'choice':
                    expression = 'lab_choice'
                else:
                    raise Unsupported('unknown condition input')
                left, right = copy.deepcopy(env), copy.deepcopy(env)
                a, b = self.emit(node.get('then', []), left), self.emit(node.get('else', []), right)
                if left != env or right != env:
                    raise Unsupported('branch changes binding shape or explicit endpoint')
                lines += ['if (' + expression + ') {', *a, '} else {', *b, '}']
            elif op == 'loop':
                cond = node.get('condition', '*')
                if cond not in ('count', '*', False):
                    raise Unsupported('unbounded/unknown source loop')
                count = node.get('iterations', self.inputs.get(str(cond), 0))
                if not isinstance(count, int) or not 0 <= count <= 100:
                    raise Unsupported('source loop bounds')
                local = copy.deepcopy(env)
                body = self.emit(node.get('body', []), local)
                if any(k in env and env[k] != v for k, v in local.items()):
                    raise Unsupported('loop changes outer reference binding shape')
                index = self.fresh()
                lines += ['for (mut int ' + index + ' = 0; ' + index + ' < ' + str(0 if cond is False else count) + '; ++' + index + ') {', *body, '}']
            elif op == 'call':
                fn = self.program.get('functions', {}).get(node['function'])
                if not fn:
                    raise Unsupported('unknown function')
                args = []
                for value in node.get('args', []):
                    if type(value) is int:
                        args.append(str(value))
                    else:
                        code, fields, _ = self.place(value, env)
                        if fields != ('value',):
                            raise Unsupported('record call argument')
                        args.append(code)
                call = self.name(node['function']) + '(' + ', '.join(args) + ')'
                if 'name' in node:
                    if node['name'] in env and not env[node['name']][3]:
                        raise Unsupported('call would rebind an active source reference')
                    mode = self.return_mode(fn)
                    name = self.name(node['name']) + '_' + self.fresh()
                    lines.append(('mut ' if mode == 'mut' else '') + 'int& ' + name + ' = ' + call + ';')
                    env[node['name']] = (name, ('value',), mode, False)
                    returns = [n['target'] for n in fn['body'] if n['op'] == 'return']
                    params = [p['name'] for p in fn['params']]
                    if len(returns) == 1 and returns[0] in params:
                        actual = node['args'][params.index(returns[0])]
                        if isinstance(actual, str):
                            parent_code, _, parent_mode = self.place(actual, env)
                            if parent_mode != 'owner':
                                self.parents[name] = parent_code
                    elif any(p['mode'] == 'mut' for p in fn['params']):
                        raise Unsupported('unresolved mutable return dependency')
                else:
                    lines.append(call + ';')
            elif op == 'return':
                code, fields, _ = self.place(node['target'], env)
                if fields != ('value',):
                    raise Unsupported('record reference return')
                lines.append('return ' + code + ';')
            elif op == 'scope':
                # Arbitrary cleanup on failure needs a separate RAII lowering,
                # not textual statements after a panic. Do not fake that mapping.
                raise Unsupported('explicit cleanup scope needs RAII adapter; covered by source corpus')
            elif op == 'fail':
                raise Unsupported('defined-failure source adapter requires cleanup scope')
            else:
                raise Unsupported('unknown source mapping: ' + op)
        return lines

    def finish_refs(self, env):
        # The model keeps unended loans live until exit. Anchor that lifetime in
        # GTI; otherwise an unused loan can end earlier and create a false finding.
        lines = []
        for code, fields, mode, ended in reversed(list(env.values())):
            if mode != 'owner' and not ended:
                lines.extend('lab_keep(' + leaf + ');' for leaf in self.leaves(code, fields))
        return lines

    def return_mode(self, fn):
        def walk(nodes):
            for n in nodes:
                if n['op'] == 'return':
                    yield n['target'].split('.')[0]
                for k in ('body', 'then', 'else'):
                    yield from walk(n.get(k, []))
        roots = list(walk(fn['body']))
        if not roots:
            return None
        modes = {p['name']: p['mode'] for p in fn['params']}
        return 'mut' if all(modes.get(r) == 'mut' for r in roots) else 'shared'

    def render(self):
        functions = []
        for name, fn in self.program.get('functions', {}).items():
            # Complex local-loan/cleanup returns need an explicit correspondence
            # argument. The model handles them; this source adapter does not.
            if any(n['op'] not in ('alloc', 'read', 'write', 'init', 'return') for n in fn['body']):
                raise Unsupported('function local loans/control flow require richer source adapter')
            env, params = {}, []
            for param in fn['params']:
                code = self.name(param['name'])
                params.append(('mut ' if param['mode'] == 'mut' else '') + 'int& ' + code)
                env[param['name']] = (code, ('value',), param['mode'], False)
            mode = self.return_mode(fn)
            result = 'void' if mode is None else ('mut ' if mode == 'mut' else '') + 'int&'
            functions += [result + ' ' + self.name(name) + '(' + ', '.join(params) + ') {', *self.emit(fn['body'], env), '}']
        env = {}
        body = self.emit(self.program['body'], env) + self.finish_refs(env)
        records = []
        for fields, name in self.types.items():
            parameters = ', '.join('int arg' + str(i) for i, _ in enumerate(fields))
            initializers = ', '.join(self.name(f) + '(arg' + str(i) + ')' for i, f in enumerate(fields))
            records.append('struct ' + name + ' { ' + ' '.join('mut int ' + self.name(f) + ';' for f in fields)
                           + ' ' + name + '(' + parameters + ') : ' + initializers + ' {} };')
        return '\n'.join(['import std.io;', *records, 'void lab_keep(int& x) {}',
                          'void lab_drop<T>(T value) {}', *functions,
                          'void lab_body(bool lab_choice, int lab_count) {', *body, '}',
                          'int main() { lab_body(' + str(bool(self.inputs.get('choice', False))).lower() + ', ' + str(self.inputs.get('count', 2)) + '); return 0; }', ''])


def render(program, inputs=None):
    return Renderer(program, inputs or {}).render()
