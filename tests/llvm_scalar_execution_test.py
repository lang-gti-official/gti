#!/usr/bin/env python3
"""Execute real lowered GTI at native O0/O3 against the production oracle."""

from pathlib import Path
from dataclasses import asdict, dataclass
import hashlib
import json
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute, plan_compile_command
from llvm_case_context import CaseContext
from llvm_evidence_manifest import build_manifest, build_context_sidecar


SCALARS = r'''
T identity<T>(T value) { return value; }
int pick(bool condition, int left, int right) { return condition ? left : right; }
int fib(int value) {
  if (value < 2) { return value; }
  return fib(value - 1) + fib(value - 2);
}
enum class Choice : uint16_t { first = 1, second = 2, third = 3 };
int choose(Choice choice) {
  switch (choice) {
  case Choice::first: return 11;
  case Choice::second: return 22;
  case Choice::third: return 33;
  }
  return 0;
}
int loop(int limit) {
  mut int result = 0;
  for (mut int index = 0; index < limit; index++) {
    if (index == 3) { continue; }
    if (index == 8) { break; }
    result += index;
  }
  mut int countdown = 3;
  while (countdown > 0) { result += countdown--; }
  return result;
}
bool short_circuit(int zero) {
  return (zero != 0 && 1 / zero == 1) || (zero == 0 && true);
}
bool floating(double value) {
  double infinity = 1.0 / value;
  double nan = value / value;
  float small = float(0.000000000000000000000000000000000000000000001);
  return infinity > 1.0 && nan != nan && !(nan == nan) && !(nan < value)
      && !(nan <= value) && !(nan > value) && !(nan >= value)
      && small > float(0) && 1.0 / -value < 0.0;
}
int main() {
  if (identity<int>(7) != 7 || fib(10) != 55 || choose(Choice::second) != 22) { return 1; }
  if (pick(true, 4, 9) != 4 || pick(false, 4, 9) != 9) { return 10; }
  if (loop(20) != 31 || !short_circuit(0) || !floating(double(0))) { return 2; }
  mut int counter = 0;
  counter += 9; counter -= 1; counter *= 3; counter /= 2; counter %= 5;
  counter |= 8; counter &= 11; counter ^= 3; counter <<= 2; counter >>= 1;
  if (counter != 18 || ~uint32_t(0) != uint32_t(4294967295)) { return 3; }
  if ((-8 >> 1) != -4 || (uint32_t(2147483648) << 1) != uint32_t(0)) { return 4; }
  int minimum = -2147483647 - 1;
  if (minimum % -1 != 0 || -7 % 3 != -1 || -7 / 3 != -2) { return 5; }
  char code = char(uint8_t(255));
  if (uint8_t(code) != uint8_t(255) || 'a' == 'b' || !true || false) { return 6; }
  if (int(-0.9) != 0 || uint32_t(-0.9) != uint32_t(0) || double(float(1.5)) != 1.5) { return 7; }
  if (int8_t(127.9d) != int8_t(127) || int8_t(-128.9d) != int8_t(-128)
      || uint8_t(255.9d) != uint8_t(255)) { return 8; }
  if (!(int64_t(-1) < uint32_t(1)) || !(uint32_t(1) > int64_t(-1))
      || int16_t(-1) + uint16_t(1) != 0 || double(1.5) + float(0.5) != 2.0d) { return 9; }
  return 0;
}
'''

COVERAGE_CONSTRUCTIONS = r'''
#include <std/numeric>

using Index = uint8_t;
mut int implicit_zero;

T convert<std::numeric T>(int value) { return T(value); }
T default_value<std::default_initializable T>() { return T(); }

int main() {
  mut int value = 2;
  int sequenced = (value += 1, +value);
  Index alias = Index(sequenced);
  uint8_t converted = convert<uint8_t>(sequenced);
  uint8_t zero = default_value<uint8_t>();
  return int(alias) + int(converted) + int(zero) + implicit_zero - 6;
}
'''


def widths() -> str:
    bodies = []
    calls = []
    for name in ('int8_t', 'int16_t', 'int32_t', 'int64_t',
                 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t'):
        bodies.append(f'''
int test_{name}(mut {name} value) {{
  if (value++ != {name}(7)) {{ return 1; }}
  value += {name}(2);
  value *= {name}(3);
  value /= {name}(2);
  value %= {name}(4);
  return int(value) - 3;
}}
''')
        calls.append(f'if (test_{name}({name}(7)) != 0) {{ return 1; }}')
    return '\n'.join(bodies) + '\nint main() {\n' + '\n'.join(calls) + '\nreturn 0; }\n'


def failures() -> dict[str, tuple[str, str]]:
    cases = {}
    def add(name: str, result: str, parameters: str, expression: str,
            arguments: str, diagnostic: str) -> None:
        if parameters == 'double a':
            arguments = f'double({arguments})'
        cases[name] = (f'{result} fail({parameters}) {{ return {expression}; }}\n'
                       f'int main() {{ [[discard]] fail({arguments}); return 0; }}\n', diagnostic)
    add('signed-add', 'int', 'int a, int b', 'a + b', '2147483647, 1', 'GTI-R0001')
    add('signed-sub', 'int', 'int a, int b', 'a - b', '-2147483647 - 1, 1', 'GTI-R0001')
    add('signed-mul', 'int64_t', 'int64_t a, int64_t b', 'a * b', 'int64_t(9223372036854775807), int64_t(2)', 'GTI-R0001')
    add('unsigned-add', 'uint64_t', 'uint64_t a, uint64_t b', 'a + b', 'uint64_t(18446744073709551615), uint64_t(1)', 'GTI-R0001')
    add('unsigned-sub', 'uint32_t', 'uint32_t a, uint32_t b', 'a - b', 'uint32_t(0), uint32_t(1)', 'GTI-R0001')
    add('negate', 'int', 'int a', '-a', '-2147483647 - 1', 'GTI-R0001')
    add('division', 'int', 'int a, int b', 'a / b', '1, 0', 'GTI-R0002')
    add('division-overflow', 'int', 'int a, int b', 'a / b', '-2147483647 - 1, -1', 'GTI-R0001')
    add('modulo', 'uint64_t', 'uint64_t a, uint64_t b', 'a % b', 'uint64_t(1), uint64_t(0)', 'GTI-R0003')
    add('negative-shift', 'int', 'int a, int b', 'a << b', '1, -1', 'GTI-R0004')
    add('wide-shift', 'int', 'int a, uint64_t b', 'a >> b', '1, uint64_t(4294967296)', 'GTI-R0005')
    add('integer-narrow', 'uint8_t', 'int a', 'uint8_t(a)', '-1', 'GTI-R0006')
    add('float-high', 'int', 'double a', 'int(a)', '2147483648.0', 'GTI-R0006')
    add('float-low', 'int', 'double a', 'int(a)', '-2147483649.0d', 'GTI-R0006')
    add('float-unsigned', 'uint64_t', 'double a', 'uint64_t(a)', '-1.0', 'GTI-R0006')
    add('float-nan', 'int', 'double a', 'int(a / a)', '0.0', 'GTI-R0006')
    add('float-infinity', 'int', 'double a', 'int(1.0 / a)', '0.0', 'GTI-R0006')
    cases['compound-narrow'] = ('''
int fail(mut int8_t value) { value += 1; return int(value); }
int main() { return fail(int8_t(127)); }
''', 'GTI-R0006')
    cases['panic'] = ('#include <std/panic>\nint main() { std::panic("native panic"); }\n', 'GTI-R0017')
    cases['empty-panic'] = ('#include <std/panic>\nint main() { std::panic(""); }\n', 'GTI-R0017')
    cases['truncated-panic'] = ('#include <std/panic>\nint main() { std::panic("' + '\u03bb' * 260 + '"); }\n', 'GTI-R0017')
    cases['assertion'] = ('''#include <std/panic>
#macro require(bool condition) {
  if (!condition) { std::panic("assertion failed"); }
}
int main() { require(false); return 0; }
''', 'GTI-R0017')
    cases['initializer'] = ('int fail(int value) { return 2147483647 + value; }\n'
                            'int global = fail(1);\nint main() { return 0; }\n', 'GTI-R0001')
    return cases


def successes(root: Path) -> dict[str, str]:
    return {'scalars': SCALARS, 'widths': widths(),
            'coverage-constructions': COVERAGE_CONSTRUCTIONS,
            'shipped-example': (root / 'examples/llvm/scalar-control-flow.gti').read_text(),
            'globals': '''
int initialize() { return 3; }
mut int value = initialize();
bool enabled = true;
int main() { value += 1; if (!enabled) { return 1; } return value - 4; }
''',
            'switch-default': '''
int pick(uint8_t selector) {
  switch (selector) {
  case uint8_t(0): return 100;
  case uint8_t(1): return 101;
  case uint8_t(2): return 102;
  default: return 999;
  }
}
int only_default(uint8_t selector) {
  switch (selector) {
  case uint8_t(0): return 10;
  default: return 20;
  }
}
int signed_route(int32_t value) {
  switch (value) {
  case -7: return 1;
  case -1: return 2;
  case 5: return 3;
  default: return 4;
  }
}
int main() {
  if (pick(uint8_t(0)) != 100) { return 1; }
  if (pick(uint8_t(1)) != 101) { return 2; }
  if (pick(uint8_t(2)) != 102) { return 3; }
  // The default arm must not fall through to the last case's block.
  if (pick(uint8_t(3)) != 999) { return 4; }
  if (pick(uint8_t(200)) != 999) { return 5; }
  if (only_default(uint8_t(0)) != 10) { return 6; }
  if (only_default(uint8_t(5)) != 20) { return 7; }
  mut int taken = 0;
  mut uint8_t probe = uint8_t(7);
  switch (probe) {
  case uint8_t(7): taken = 1; break;
  default: taken = 2; break;
  }
  if (taken != 1) { return 8; }
  probe = uint8_t(8);
  switch (probe) {
  case uint8_t(7): taken = 1; break;
  default: taken = 2; break;
  }
  if (taken != 2) { return 9; }
  // Negative case labels are canonical constants at the selector's width.
  if (signed_route(-7) != 1) { return 10; }
  if (signed_route(-1) != 2) { return 11; }
  if (signed_route(5) != 3) { return 12; }
  if (signed_route(-6) != 4) { return 13; }
  return 0;
}
''',
            # Negative enumerators cross function arguments and select switch
            # arms: their constants must be canonical at the storage width, or
            # O3 interprocedural propagation answers the comparison wrongly.
            'integral-enum-underlying': '''
enum class Opcode : uint8_t { halt = 0, load = 7, store = 9 };
enum class Wide : int32_t { low = 5, high = 1000 };
enum class Signed : int32_t { low = -5, high = 1000 };
enum class Tiny : int8_t { least = -128, minus = -1, most = 127 };
uint8_t opcode(Opcode value) { return std::to_underlying<uint8_t, Opcode>(value); }
int32_t wide(Wide value) { return std::to_underlying<int32_t, Wide>(value); }
int32_t is_low(Signed value) { if (value == Signed::low) { return 1; } return 2; }
int route(Signed value) {
  switch (value) {
  case Signed::low: return 10;
  case Signed::high: return 20;
  default: return 30;
  }
}
int8_t tiny(Tiny value) { return std::to_underlying<int8_t, Tiny>(value); }
int main() {
  if (opcode(Opcode::halt) != uint8_t(0)) { return 1; }
  if (opcode(Opcode::load) != uint8_t(7)) { return 2; }
  if (opcode(Opcode::store) != uint8_t(9)) { return 3; }
  if (wide(Wide::low) != 5) { return 4; }
  if (wide(Wide::high) != 1000) { return 5; }
  mut Opcode selected = Opcode::store;
  selected = Opcode::load;
  if (opcode(selected) != uint8_t(7)) { return 6; }
  if (is_low(Signed::low) != 1) { return 7; }
  if (is_low(Signed::high) != 2) { return 8; }
  if (route(Signed::low) != 10) { return 9; }
  if (route(Signed::high) != 20) { return 10; }
  if (std::to_underlying<int32_t, Signed>(Signed::low) != -5) { return 11; }
  if (tiny(Tiny::least) != int8_t(-128)) { return 12; }
  if (tiny(Tiny::minus) != int8_t(-1)) { return 13; }
  if (tiny(Tiny::most) != int8_t(127)) { return 14; }
  return 0;
}
''',
            'dormant-owner': '''
class Unused { public: Unused() {} ~Unused() {} };
int unused() { Unused value{}; return 1; }
int main() { return 0; }
''', 'numeric-intrinsics': '''
#include <std/numeric>
int main() {
  if (std::wrapping_add(uint8_t(255), uint8_t(1)) != uint8_t(0)) { return 1; }
  if (std::wrapping_mul(int8_t(64), int8_t(2)) != int8_t(-128)) { return 2; }
  if (std::saturating_sub(int8_t(-128), int8_t(1)) != int8_t(-128)) { return 3; }
  if (std::saturating_mul(int8_t(-64), int8_t(-3)) != int8_t(127)) { return 4; }
  if (std::saturating_add(uint64_t(18446744073709551615), uint64_t(1)) != uint64_t(18446744073709551615)) { return 5; }
  return 0;
}
'''}


def rejections() -> dict[str, str]:
    return {
        # The former scalar virtual-call source executes verbatim in the
        # dispatch harness. Replacing a live polymorphic object still rejects.
        'polymorphic-replacement': 'class Owner { public: virtual int read() { return 0; } }; int main() { mut Owner value{}; Owner other{}; value = other; return value.read(); }',
        'polymorphic-array': 'class Owner { public: virtual int read() { return 0; } }; int main() { Owner data[2] = {Owner(), Owner()}; return data[0].read(); }',
        'polymorphic-tls': 'class Owner { public: virtual int read() { return 0; } }; thread_local Owner value{}; int main() { return value.read(); }',
    }


@dataclass(frozen=True)
class ScalarCase:
    source_text: str
    context: CaseContext
    expectation: dict
    endpoints: tuple[Endpoint, ...]
    build_timeout: int
    run_timeout: int | None

    @property
    def is_control(self) -> bool:
        return self.expectation['kind'] == 'backend-rejection'

    def sidecar_record(self) -> dict:
        return {
            'context': self.context.record(),
            'role': 'expected-rejection-control' if self.is_control else 'positive-admission',
            'observation': {
                'endpoints': ([asdict(endpoint) for endpoint in self.endpoints] if not self.is_control
                              else [{'backend': 'llvm', 'optimization': None}]),
                'setup': ({'cwd': 'inherited', 'environment': 'inherited', 'build_timeout': self.build_timeout,
                           'extra_native_inputs': [], 'argv': None} if self.is_control else
                          {'cwd': 'repository-root', 'environment': 'copy-inherited',
                           'build_timeout': self.build_timeout, 'run_timeout': self.run_timeout,
                           'extra_native_inputs': [], 'argv': []}),
                'expectation': dict(self.expectation),
            },
        }


def scalar_cases(root: Path) -> dict[str, ScalarCase]:
    endpoints = (Endpoint('cpp', 'O3', 'c++23'), Endpoint('llvm', 'O0'), Endpoint('llvm', 'O3'))
    cases = {}

    def add(name: str, text: str, expectation: dict) -> None:
        control = expectation['kind'] == 'backend-rejection'
        origin = ('examples/llvm/scalar-control-flow.gti' if name == 'shipped-example'
                  else f'inline/{name}.gti')
        context = CaseContext(
            provider='llvm_scalar_execution', identity=name, source=origin,
            source_sha256=hashlib.sha256(text.encode('utf-8')).hexdigest(),
            source_kind='repository' if name == 'shipped-example' else 'inline',
            materialized_name=f'reject-{name}.gti' if control else f'{name}.gti',
            execution_profile='single-threaded',
            loading='materialized-source-with-inherited-compiler-environment',
            observation='scalar-unsupported-control' if control else 'scalar-differential-execution',
            unresolved=('host-target-applicability', 'effective-compiler-environment',
                        'implicit-toolchain-runtime'),
        )
        cases[name] = ScalarCase(text, context, expectation, () if control else endpoints,
                                 60 if control else 240, None if control else 15)

    for name, text in successes(root).items():
        add(name, text, {'kind': 'success', 'exit_code': 0, 'stdout': '', 'stderr': '',
                         'differential': 'exact-exit-stdout-stderr'})
    for name, (text, diagnostic) in failures().items():
        add(name, text, {'kind': 'defined-failure', 'exit_code': 70, 'diagnostic': diagnostic,
                         'differential': 'exact-exit-stdout-stderr'})
    for name, text in rejections().items():
        add(name, text, {'kind': 'backend-rejection', 'exit_code': 65,
                         'diagnostic': 'GTI-B0003',
                         'preserved_output_hex': b'existing artifact'.hex()})
    return cases


def rejection_plan(gti: Path, source: Path, output: Path, *, timeout: int) -> tuple[list[str], dict]:
    return ([str(gti), str(source), '--backend', 'llvm', '-o', str(output)],
            {'capture_output': True, 'text': True, 'timeout': timeout})


def scalar_command_plans(case: ScalarCase, name: str, gti: Path,
                         root: Path, work: Path) -> list[dict]:
    source = work / case.context.materialized_name
    if case.is_control:
        command, options = rejection_plan(gti, source, work / f'reject-{name}', timeout=case.build_timeout)
        return [{'command': command, 'options': options,
                 'cwd_policy': 'inherited', 'environment_policy': 'inherited'}]
    return [{
        'command': plan_compile_command(
            gti, source, work / f'{name}-{endpoint.backend}-{endpoint.optimization}' /
            ('program.exe' if os.name == 'nt' else 'program'), endpoint),
        'cwd': str(root), 'environment_policy': 'copy-inherited',
        'build_timeout': case.build_timeout, 'run_timeout': case.run_timeout,
    } for endpoint in case.endpoints]


def evidence_cases(root: Path) -> dict[str, str]:
    return {name: case.source_text for name, case in scalar_cases(root).items() if not case.is_control}


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    cases = evidence_cases(root)
    panic = ('panic', 'empty-panic', 'truncated-panic', 'assertion')
    profiles = {
        'scalars': ('scalars',),
        'widths': ('widths',),
        'globals': ('globals',),
        'numeric-intrinsics': ('numeric-intrinsics',),
        'coverage-constructions': ('coverage-constructions',),
        'integral-enum-underlying': ('integral-enum-underlying',),
        'switch-default': ('switch-default',),
        'defined-failures': tuple(failures()),
        'panic-failures': panic,
    }
    assert all(case in cases for profile in profiles.values() for case in profile)
    return profiles


def evidence_manifest(root: Path) -> dict:
    origins = {name: case.context.source for name, case in scalar_cases(root).items()
               if not case.is_control and case.context.source_kind == 'repository'}
    return build_manifest(
        'llvm_scalar_execution', evidence_cases(root), evidence_profiles(root),
        origins)


def evidence_partition_policy(root: Path) -> dict[str, str]:
    del root
    return {'cases': 'positive-admission', 'controls': 'expected-rejection-control'}


def evidence_context(root: Path) -> dict:
    cases = scalar_cases(root)
    return build_context_sidecar(
        evidence_manifest(root),
        {name: case.sidecar_record() for name, case in cases.items() if not case.is_control},
        {name: case.sidecar_record() for name, case in cases.items() if case.is_control},
        partition_policy=evidence_partition_policy(root))


def list_evidence(root: Path) -> None:
    print(json.dumps(evidence_manifest(root), sort_keys=True))


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == '--list-evidence':
        list_evidence(Path(sys.argv[2]).resolve())
        return
    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    selection = sys.argv[3] if len(sys.argv) > 3 else None
    env = os.environ.copy()
    cases = scalar_cases(root)
    success_cases = {name: case for name, case in cases.items() if case.expectation['kind'] == 'success'}
    failure_cases = {name: case for name, case in cases.items() if case.expectation['kind'] == 'defined-failure'}
    rejection_cases = {name: case for name, case in cases.items() if case.is_control}
    if (selection and selection not in success_cases and
            selection not in failure_cases and selection not in rejection_cases):
        raise ValueError(f'unknown scalar execution case: {selection}')
    with tempfile.TemporaryDirectory(prefix='gti-llvm-scalars-') as temporary:
        work = Path(temporary)
        for name, specification in success_cases.items():
            if selection and selection != name:
                continue
            source = work / specification.context.materialized_name
            source.write_text(specification.source_text)
            results = []
            for endpoint in specification.endpoints:
                case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
                result = execute(gti, root, case, source, endpoint, env,
                                 build_timeout=specification.build_timeout,
                                 run_timeout=specification.run_timeout)
                expected = specification.expectation
                if result.error or result.observable != (expected['exit_code'], expected['stdout'], expected['stderr']):
                    raise AssertionError(f'{name} {endpoint}: {result}')
                results.append(result.observable)
            assert results[0] == results[1] == results[2]
        for name, specification in failure_cases.items():
            if selection and selection != name:
                continue
            source = work / specification.context.materialized_name
            source.write_text(specification.source_text)
            results = []
            for endpoint in specification.endpoints:
                case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
                result = execute(gti, root, case, source, endpoint, env,
                                 build_timeout=specification.build_timeout,
                                 run_timeout=specification.run_timeout)
                expected = specification.expectation
                if result.error or result.returncode != expected['exit_code'] or expected['diagnostic'] not in result.stderr:
                    raise AssertionError(f'{name} {endpoint}: {result}')
                results.append(result.observable)
            if not results[0] == results[1] == results[2]:
                raise AssertionError(f'{name}: source/failure attribution drift: {results}')
        for name, specification in rejection_cases.items():
            if selection and selection != name:
                continue
            source = work / specification.context.materialized_name
            source.write_text(specification.source_text)
            output = work / f'reject-{name}'
            expected = specification.expectation
            sentinel = bytes.fromhex(expected['preserved_output_hex'])
            output.write_bytes(sentinel)
            command, options = rejection_plan(gti, source, output, timeout=specification.build_timeout)
            result = subprocess.run(command, **options)
            if result.returncode != expected['exit_code'] or expected['diagnostic'] not in result.stderr or output.read_bytes() != sentinel:
                raise AssertionError(f'unsupported {name}: {result}')
    print('LLVM scalar O0/O3 execution, C++ differential failures and unsupported hierarchy-storage controls passed')


if __name__ == '__main__':
    main()
