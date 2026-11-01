#!/usr/bin/env python3
"""Compare primitive and record GTI calls with the same external success ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import re
import statistics
import subprocess
import sys
import tempfile
import time


COMMON = r'''
#include <gti/runtime_failure.h>
#include <cstdint>
#include <climits>
struct Channel { gti_failure_record_v2 record; const gti_failure_artifact_descriptor_v1* origin; };
struct Point { std::int32_t x; std::int32_t y; };
extern "C" bool I64(std::int64_t*, Channel*, std::int64_t);
extern "C" bool DOUBLE(double*, Channel*, double);
extern "C" bool VOID(Channel*);
extern "C" bool DIVISION(int*, Channel*, int, int);
extern "C" bool POINT(Point*, Channel*, const Point*);
'''
BASELINE = r'''
static const gti_failure_artifact_descriptor_v1 artifact{.abi_version = 1};
[[gnu::noinline, gnu::cold]] static bool fail(Channel* channel,
    gti_failure_code_v1 code, gti_failure_detail_v1 detail) {
  channel->record = {};
  channel->record.abi_version = GTI_FAILURE_ABI_VERSION_V2;
  channel->record.code = code;
  channel->record.detail = detail;
  channel->record.site_index = 1;
  channel->origin = &artifact;
  return false;
}
extern "C" bool I64(std::int64_t* out, Channel*, std::int64_t x) { *out = x; return true; }
extern "C" bool DOUBLE(double* out, Channel*, double x) { *out = x; return true; }
extern "C" bool VOID(Channel*) { return true; }
extern "C" bool POINT(Point* out, Channel*, const Point* value) { *out = *value; return true; }
extern "C" bool DIVISION(int* out, Channel* failure, int x, int y) {
  if (y == 0)
    return fail(failure, GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1, GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1);
  if (x == INT_MIN && y == -1)
    return fail(failure, GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1, GTI_FAILURE_DETAIL_DIVISION_V1);
  *out = x / y;
  return true;
}
'''
HOST = r'''
#include <chrono>
#include <cstdio>
#include <string_view>
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  const std::string_view mode = argv[1];
  Channel failure{};
  if (mode == "probe") {
    int result = 123;
    if (DIVISION(&result, &failure, 7, 0) || result != 123 ||
        failure.record.abi_version != GTI_FAILURE_ABI_VERSION_V2 ||
        failure.record.code != GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 ||
        failure.record.detail != GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1 ||
        !failure.record.site_index || !failure.origin) return 2;
    failure = {};
    if (DIVISION(&result, &failure, INT_MIN, -1) || result != 123 ||
        failure.record.abi_version != GTI_FAILURE_ABI_VERSION_V2 ||
        failure.record.code != GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1 ||
        failure.record.detail != GTI_FAILURE_DETAIL_DIVISION_V1 ||
        !failure.record.site_index || !failure.origin) return 3;
    if (!DIVISION(&result, &failure, -15, 2) || result != -7) return 4;
    return 0;
  }
  constexpr unsigned iterations = 50000000;
  constexpr std::uint64_t remainder = iterations % 1024;
  constexpr std::uint64_t smallSum = (iterations / 1024) * 523776ULL + remainder * (remainder - 1) / 2;
  std::uint64_t expectedDivision = 0;
  for (unsigned i = 0; i < 1024; ++i) expectedDivision += (i + 1) / 7;
  expectedDivision *= iterations / 1024;
  for (unsigned i = 0; i < remainder; ++i) expectedDivision += (i + 1) / 7;
  std::uint64_t sum = 0;
  double doubleSum = 0;
  auto started = std::chrono::steady_clock::now();
  if (mode == "int64") {
    for (unsigned i = 0; i < iterations; ++i) {
      std::int64_t result;
      if (!I64(&result, &failure, (1LL << 40) + (i & 1023))) return 5;
      sum += std::uint64_t(result);
    }
  } else if (mode == "double") {
    for (unsigned i = 0; i < iterations; ++i) {
      double result;
      if (!DOUBLE(&result, &failure, double(i & 1023) + 0.5)) return 6;
      doubleSum += result;
    }
  } else if (mode == "void") {
    for (unsigned i = 0; i < iterations; ++i)
      if (!VOID(&failure)) return 7;
  } else if (mode == "division") {
    for (unsigned i = 0; i < iterations; ++i) {
      int result;
      if (!DIVISION(&result, &failure, int(i & 1023) + 1, 7)) return 8;
      sum += unsigned(result);
    }
  } else if (mode == "point") {
    for (unsigned i = 0; i < iterations; ++i) {
      const Point value{std::int32_t(i & 1023), std::int32_t(i & 1023) + 1};
      Point result;
      if (!POINT(&result, &failure, &value)) return 13;
      sum += std::uint64_t(result.x) + std::uint64_t(result.y);
    }
  } else return 9;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  if (mode == "int64" && sum != std::uint64_t(iterations) * (1ULL << 40) + smallSum) return 10;
  if (mode == "double" && doubleSum != double(smallSum) + iterations * 0.5) return 11;
  if (mode == "division" && sum != expectedDivision) return 12;
  if (mode == "point" && sum != 2 * smallSum + iterations) return 14;
  std::printf("%.9f\n", seconds);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', type=Path)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--measure', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    report = {
        'contract': 'int64/double and two-int32 record identity, infallible void, successful checked int32 division; external GTI status/result/failure-channel ABI without LTO',
        'comparison_limit': 'successful calls and guards; baseline failure records retain code/detail/site/origin channels, but artifact source descriptors differ and symbolization cost is not compared',
        'allocation_scope': 'fixtures have no heap operations; host/loader allocations excluded',
        'producer_scope': 'producer timing includes native bundle generation and decoding its canonical interface to retrieve compiler-owned signature identities',
        'producer_sha256': hashlib.sha256(args.tool.read_bytes()).hexdigest(),
        'timing_acceptance': args.measure, 'iterations_per_sample': 50000000,
        'steps': [],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gti-primitive-cost-') as temporary:
        root = Path(temporary)

        def run(command, name):
            started = time.monotonic()
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=120)
            report['steps'].append({'name': name, 'seconds': time.monotonic() - started, 'exit': done.returncode,
                                    'cumulative_child_peak_rss_bytes': resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss *
                                    (1 if sys.platform == 'darwin' else 1024)})
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            if done.returncode:
                raise RuntimeError(name + ': ' + done.stdout + done.stderr)
            return done.stdout

        run([args.tool.resolve(), args.runtime.resolve(), root], 'produce-O3')
        assert not (root / 'library.gti').exists(), 'producer source was retained'
        symbols = json.loads((root / 'symbols.json').read_text())
        assert set(symbols) == {'int64', 'double', 'void', 'division', 'point'}
        assert len(set(symbols.values())) == 5

        def source(body):
            text = COMMON + body
            for marker, key in [('I64', 'int64'), ('DOUBLE', 'double'), ('VOID', 'void'), ('DIVISION', 'division'), ('POINT', 'point')]:
                # Only replace complete placeholder identifiers, never ABI constants.
                text = re.sub(r'\b' + marker + r'\b', symbols[key], text)
            return text

        (root / 'baseline.cpp').write_text(source(BASELINE))
        (root / 'host.cpp').write_text(source(HOST))
        includes = ['-I', repo / 'runtime/include', '-I', repo / 'include']
        for name in ('baseline', 'host'):
            run(['c++', '-O3', '-fno-lto', '-std=c++20', *includes, '-c', root / (name + '.cpp'),
                 '-o', root / (name + '.o')], name + '-compile')
        for name, obj in [('native', 'library.o'), ('baseline', 'baseline.o')]:
            run(['c++', '-fno-lto', root / 'host.o', root / obj, '-o', root / name], name + '-link')
            run([root / name, 'probe'], name + '-failure-probes')
        samples = {operation: {'native': [], 'baseline': []} for operation in symbols}
        for sample in range(9 if args.measure else 1):
            operations = list(symbols)[::1 if sample % 2 == 0 else -1]
            for operation in operations:
                for name in ('native', 'baseline')[::1 if sample % 2 == 0 else -1]:
                    samples[operation][name].append(float(run([root / name, operation], f'{operation}-{name}-{sample}')))
        ratios = {operation: statistics.median(values['native']) / statistics.median(values['baseline'])
                  for operation, values in samples.items()}
        report.update(samples_seconds=samples, ratios=ratios,
                      bytes={name: (root / name).stat().st_size for name in
                             ['library.o', 'baseline.o', 'host.o', 'primitive.gtilib', 'native', 'baseline']},
                      maximum_child_rss_bytes=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss *
                      (1 if sys.platform == 'darwin' else 1024))
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        regressions = {name: ratio for name, ratio in ratios.items() if ratio > 1.25}
        if args.measure and regressions:
            raise RuntimeError('same-contract boundary cost regression: ' + str(regressions))
    print(args.output)


if __name__ == '__main__':
    main()
