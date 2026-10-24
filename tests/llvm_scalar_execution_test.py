#!/usr/bin/env python3
"""Execute real lowered GTI at native O0/O3 against the production oracle."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute


SCALARS = r'''
T identity<T>(T value) { return value; }
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


def main() -> None:
    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    env = os.environ.copy()
    successes = {'scalars': SCALARS, 'widths': widths(),
                 'shipped-example': (root / 'examples/llvm/scalar-control-flow.gti').read_text(),
                 'globals': '''
int initialize() { return 3; }
mut int value = initialize();
bool enabled = true;
int main() { value += 1; if (!enabled) { return 1; } return value - 4; }
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
    with tempfile.TemporaryDirectory(prefix='gti-llvm-scalars-') as temporary:
        work = Path(temporary)
        for name, source_text in successes.items():
            source = work / f'{name}.gti'
            source.write_text(source_text)
            results = []
            for endpoint in (Endpoint('cpp', 'O3', 'c++23'), Endpoint('llvm', 'O0'), Endpoint('llvm', 'O3')):
                case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
                result = execute(gti, root, case, source, endpoint, env)
                if result.error or result.observable != (0, '', ''):
                    raise AssertionError(f'{name} {endpoint}: {result}')
                results.append(result.observable)
            assert results[0] == results[1] == results[2]
        for name, (source_text, diagnostic) in failures().items():
            source = work / f'{name}.gti'
            source.write_text(source_text)
            results = []
            for endpoint in (Endpoint('cpp', 'O3', 'c++23'), Endpoint('llvm', 'O0'), Endpoint('llvm', 'O3')):
                case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
                result = execute(gti, root, case, source, endpoint, env)
                if result.error or result.returncode != 70 or diagnostic not in result.stderr:
                    raise AssertionError(f'{name} {endpoint}: {result}')
                results.append(result.observable)
            if not results[0] == results[1] == results[2]:
                raise AssertionError(f'{name}: source/failure attribution drift: {results}')
        for name, text in {'owner': 'class Owner { public: Owner() {} ~Owner() {} }; int main() { Owner value{}; return 0; }',
                           'array': 'int main() { int data[2] = {1, 2}; return data[0]; }',
                           'tls': 'thread_local mut int value = 3; int main() { return value; }'}.items():
            source = work / f'reject-{name}.gti'
            source.write_text(text)
            output = work / f'reject-{name}'
            output.write_bytes(b'existing artifact')
            result = subprocess.run([str(gti), str(source), '--backend', 'llvm', '-o', str(output)],
                                    capture_output=True, text=True, timeout=60)
            if result.returncode != 65 or 'GTI-B0003' not in result.stderr or output.read_bytes() != b'existing artifact':
                raise AssertionError(f'unsupported {name}: {result}')
    print('LLVM scalar O0/O3 execution, C++ differential failures and unsupported-owner controls passed')


if __name__ == '__main__':
    main()
