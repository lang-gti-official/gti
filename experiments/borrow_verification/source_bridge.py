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


class CoreRenderer(Renderer):
    """Observation-checking source profile for the independent MIR core.

    Expected integers come from the independent concrete model, never production
    acceptance. Every read checks its ordinal and value; a final count detects
    missing reads. No formatting/runtime I/O is needed. Owned scalar allocations
    can select real unique ownership. LIFO cleanup is lexical RAII, including the
    compiler's actual defined-failure cleanup edges.
    """
    def __init__(self, program, inputs, expected, heap_owners):
        super().__init__(program, inputs)
        self.expected=list(expected)
        self.heap_owners=set(heap_owners)
        self.owner_scopes=[]
        self.handles={}
        self.dead=set()
        self.terminal=False

    def emit(self, nodes, env):
        lines=[]
        for node in nodes:
            if self.terminal:
                break
            op=node['op']
            if op=='alloc':
                name=node['name']
                if name in env:
                    if name not in self.dead:
                        raise Unsupported('core allocation name still live')
                    del env[name]
                    self.dead.remove(name)
                lines.append('{')
                self.owner_scopes.append(name)
                if name in self.heap_owners:
                    fields=tuple(node.get('fields',{'value':0}))
                    if fields!=('value',):raise Unsupported('core heap profile currently scalar')
                    value=node.get('fields',{'value':0})['value']
                    if type(value)is not int or not -100000<=value<=100000:raise Unsupported('core scalar allocation range')
                    handle=self.name(name)+'_'+self.fresh()
                    lines.append(('mut ' if node.get('mutable',True) else '')+'auto '+handle+' = std::make_unique<int>('+str(value)+');')
                    code='(*'+handle+')'
                    self.handles[name]=handle
                    env[name]=(code,fields,'owner',False)
                else:
                    lines.extend(super().emit([node],env))
            elif op=='read':
                code,fields,_=self.place(node['target'],env)
                lines.extend('lab_observe(lab_index, lab_bad, '+leaf+');' for leaf in self.leaves(code,fields))
            elif op=='drop' and '.' not in node['target']:
                name=node['target']
                if not self.owner_scopes or self.owner_scopes[-1]!=name:
                    raise Unsupported('core explicit free requires lexical LIFO owner order')
                self.owner_scopes.pop()
                lines.append('}')
                self.dead.add(name)
                # Ends every reference declared in the retiring lexical scope;
                # prior explicit endpoints were already anchored by lab_keep.
                env[name]=(env[name][0],env[name][1],'owner',True)
            elif op=='scope':
                cleanup=node.get('cleanup',[])
                if any(n['op'] not in ('end','drop') for n in cleanup):
                    raise Unsupported('core cleanup permits explicit loan endpoints and frees only')
                body=node.get('body',[])
                # The last-use anchor is before the terminal failure/return;
                # no intervening owner operation is allowed. Compiler-generated
                # cleanup then executes the lexical frees on both edges.
                terminal=next((i for i,n in enumerate(body) if n['op'] in ('fail','return')),None)
                if terminal is None:
                    lines.extend(self.emit(body,env))
                    lines.extend(self.emit(cleanup,env))
                else:
                    if terminal!=len(body)-1:raise Unsupported('core statements after terminal scope operation')
                    lines.extend(self.emit(body[:terminal],env))
                    ends=[n for n in cleanup if n['op']=='end']
                    drops=[n for n in cleanup if n['op']=='drop']
                    if cleanup!=ends+drops:raise Unsupported('core cleanup must end loans before freeing storage')
                    # Returning that exact reference is its final use itself.
                    returned=body[terminal].get('target')
                    for n in ends:
                        if n['name']!=returned:lines.extend(super().emit([n],env))
                    lines.extend(self.emit([body[terminal]],env))
                    was_terminal=self.terminal
                    self.terminal=False
                    lines.extend(self.emit(drops,env))
                    self.terminal=was_terminal
            elif op=='fail':
                lines.append('lab_fail(lab_index, lab_bad);')
                self.terminal=True
            elif op=='return':
                lines.extend(super().emit([node],env))
                self.terminal=True
            elif op=='call':
                emitted=super().emit([node],env)
                name=self.name(node['function'])+'('
                inserted='lab_index, lab_bad'+(', ' if node.get('args') else '')
                lines.extend(line.replace(name,name+inserted) for line in emitted)
            else:
                lines.extend(super().emit([node],env))
        return lines

    def return_mode(self, fn):
        modes={p['name']:p['mode'] for p in fn['params']}
        returned=[]
        def walk(nodes):
            for n in nodes:
                if n['op']=='alloc':modes[n['name']]='mut' if n.get('mutable',True) else 'shared'
                elif n['op']=='borrow':modes[n['name']]=n['mode']
                elif n['op']=='return':returned.append(n['target'].split('.')[0])
                for key in ('body','then','else'):walk(n.get(key,[]))
        walk(fn['body'])
        if not returned:return None
        return 'mut' if all(modes.get(r)=='mut' for r in returned) else 'shared'

    def render(self):
        functions=[]
        for name,fn in self.program.get('functions',{}).items():
            self.owner_scopes=[];self.terminal=False;self.dead=set()
            env={};params=['mut int& lab_index','mut int& lab_bad']
            for param in fn['params']:
                code=self.name(param['name'])
                params.append(('mut ' if param['mode']=='mut' else '')+'int& '+code)
                env[param['name']]=(code,('value',),param['mode'],False)
            body=self.emit(fn['body'],env)
            if not self.terminal:body+=self.finish_refs(env)
            body+=['}']*len(self.owner_scopes)
            mode=self.return_mode(fn)
            result='void' if mode is None else ('mut ' if mode=='mut' else '')+'int&'
            annotation=''
            if mode=='mut':
                origins={p['name']:p['name'] for p in fn['params']}
                results=[]
                def origin_walk(nodes):
                    for n in nodes:
                        if n['op']=='borrow':origins[n['name']]=origins.get(n['target'].split('.')[0])
                        elif n['op']=='return':results.append(origins.get(n['target'].split('.')[0]))
                        for key in ('body','then','else'):origin_walk(n.get(key,[]))
                origin_walk(fn['body'])
                if len(set(results))==1 and results[0] is not None:
                    annotation='[[borrowed_from('+self.name(results[0])+')]] '
                else:raise Unsupported('core mutable return needs one resolved argument origin')
            functions += [annotation+result+' '+self.name(name)+'('+', '.join(params)+') {',*body,'}']
        self.owner_scopes=[];self.terminal=False;self.dead=set()
        env={}
        body=self.emit(self.program['body'],env)
        if not self.terminal:body+=self.finish_refs(env)
        # Count/value assertions are made before lexical destruction. Failure
        # cleanup is independently checked by the MIR machine and native run.
        if not self.terminal:
            body+=['if (lab_bad != 0) return 91;','if (lab_index != '+str(len(self.expected))+') return 92;']
        body+=['}']*len(self.owner_scopes)
        records=[]
        for fields,name in self.types.items():
            params=', '.join('int arg'+str(i) for i,_ in enumerate(fields))
            init=', '.join(self.name(f)+'(arg'+str(i)+')' for i,f in enumerate(fields))
            records.append('struct '+name+' { '+' '.join('mut int '+self.name(f)+';' for f in fields)+' '+name+'('+params+') : '+init+' {} };')
        tests=[]
        for i,value in enumerate(self.expected):
            tests.append(('if' if i==0 else 'else if')+' (index == '+str(i)+') { if (value != '+str(value)+') bad = 1; }')
        tests.append(('else ' if tests else '')+'{ bad = 1; }')
        helpers=['int lab_zero(int n) { return n; }','void lab_keep(int& value) {}','void lab_drop<T>(T value) {}',
            'void lab_observe(mut int& index, mut int& bad, int value) {',*tests,'++index;','}',
            # A mismatch before a requested failure must not masquerade as the
            # expected division_by_zero. It takes integer_overflow instead.
            'void lab_fail(int& index, int& bad) {',
            'if (bad != 0 || index != '+str(len(self.expected))+') { mut int mismatch = 2147483647; mismatch += lab_zero(1); }',
            'int failure = 1 / lab_zero(0);','}']
        return '\n'.join(['import std.memory;',*records,*helpers,*functions,'int main() {',
            'mut int lab_index = 0;','mut int lab_bad = 0;',
            'bool lab_choice = '+str(bool(self.inputs.get('choice',False))).lower()+';',
            'int lab_count = '+str(self.inputs.get('count',2))+';',*body,'return 0;','}',''])


def render_core(program, expected_observations, inputs=None, heap_owners=()):
    """Render finite-input source with exact ordered read/count assertions.

    Native success is exit 0 with empty stdout. Intended defined failure is exit
    70 containing `division_by_zero`; observation mismatches use a distinct
    integer-overflow failure. heap_owners selects logical scalar allocation names
    to map to actual std::unique_ptr storage. No production checker is consulted.
    """
    return CoreRenderer(program,inputs or {},expected_observations,heap_owners).render()
