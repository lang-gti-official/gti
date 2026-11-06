#!/usr/bin/env python3
"""Compare the scalar GTI library boundary with the same native ABI contract."""
import argparse
import json
from pathlib import Path
import re
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', type=Path)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('examples', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--measure', action='store_true')
    parser.add_argument('--passive-storage', action='store_true',
                        help='compare a private persistent scalar load/store with the same native operation')
    args = parser.parse_args()
    report = {'contract': 'int32 selector; conservative GTI status/result/failure-pointer ABI; external call without LTO',
              'comparison_limit': 'same ABI baseline, not whole-program inlining parity',
              'allocation_scope': 'fixture has no heap operations; object has no undefined native dependencies; host/loader allocations excluded',
              'steps': [], 'timing_acceptance': args.measure}
    if args.passive_storage:
        report['contract'] = 'private zero-initialized int32 exchange; conservative GTI status/result/failure-pointer ABI; external call without LTO'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gti-scalar-cost-') as temporary:
        root = Path(temporary).resolve()

        def run(command, name):
            started = time.monotonic()
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=120)
            report['steps'].append({'name': name, 'seconds': time.monotonic()-started, 'exit': done.returncode})
            if done.returncode:
                raise RuntimeError(name + ': ' + done.stdout + done.stderr)
            return done.stdout

        (root / 'stdlib').mkdir()
        (root / 'stdlib/prelude.gti').write_text('')
        shutil.copytree(args.examples, root / 'producer')
        (root / 'consumer').mkdir()
        shutil.copyfile(args.examples / 'consumer.gti', root / 'consumer/main.gti')
        if args.passive_storage:
            (root / 'producer/library.gti').write_text('''export module numbers;
mut int state;
export int choose(int x) {
  int previous = state;
  state = x;
  return previous;
}
''')
            (root / 'consumer/main.gti').write_text('''import "api.gti";
int main() {
  if (choose(7) != 0) { return 1; }
  if (choose(3) != 7) { return 2; }
  return choose(0) - 3;
}
''')
        prefix = [args.tool.resolve(), '--produce', args.runtime.resolve(), root, '3', 'child']
        run(prefix, 'produce')
        shutil.rmtree(root / 'producer')
        prefix[1] = '--consume'
        run(prefix, 'source-absent-consume')
        names = run(['nm', '-g', root / 'library.o'], 'symbols')
        symbols = set(re.findall(r'__gtilib_callable_v4_[0-9a-f]{64}', names))
        if len(symbols) != 1:
            raise RuntimeError('unexpected exported scalar object symbols')
        symbol = symbols.pop()
        operation = ('static int state; *result = state; state = x;' if args.passive_storage
                     else '*result = x == 7 ? 42 : 0;')
        (root / 'baseline.cpp').write_text('extern "C" bool ' + symbol + '(int* result, void*, int x) { ' + operation + ' return true; }\n')
        (root / 'host.cpp').write_text('''#include <chrono>
#include <cstdio>
extern "C" bool SYMBOL(int*, void*, int);
int main() {
  unsigned long long sum = 0;
  auto started = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 50000000; ++i) {
    int result = -1;
    if (!SYMBOL(&result, nullptr, int(i & 15))) return 2;
    sum += unsigned(result);
  }
  if (sum != EXPECTED) return 3;
  std::printf("%.9f\\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count());
}
'''.replace('SYMBOL', symbol).replace('EXPECTED', '374999985' if args.passive_storage else '131250000'))
        run(['c++', '-O3', '-std=c++20', '-c', root / 'host.cpp', '-o', root / 'host.o'], 'host-build')
        run(['c++', '-O3', '-c', root / 'baseline.cpp', '-o', root / 'baseline.o'], 'baseline-build')
        for name, obj in [('native', 'library.o'), ('baseline', 'baseline.o')]:
            run(['c++', root / 'host.o', root / obj, '-o', root / name], name + '-link')
        samples = {'native': [], 'baseline': []}
        for sample in range(9 if args.measure else 1):
            for name in ('native', 'baseline')[::1 if sample % 2 == 0 else -1]:
                samples[name].append(float(run([root / name], name + '-' + str(sample))))
        report.update(samples_seconds=samples,
                      ratio=statistics.median(samples['native']) / statistics.median(samples['baseline']),
                      bytes={name: (root / name).stat().st_size for name in ['library.o', 'baseline.o', 'numbers.gtilib', 'native', 'baseline']},
                      maximum_child_rss_bytes=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss * (1 if sys.platform == 'darwin' else 1024))
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        if args.measure and report['ratio'] > 1.25:
            raise RuntimeError('same-contract boundary cost regression: ' + str(report['ratio']))
    print(args.output)


if __name__ == '__main__':
    main()
