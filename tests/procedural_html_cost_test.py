#!/usr/bin/env python3
"""Measure HTML expansion versus equivalent handwritten owned-tree construction."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--measure', action='store_true')
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    provider = repository / 'tests/fixtures/stdlib_adoption_allocation_counts.cpp'
    report = {}
    with tempfile.TemporaryDirectory(prefix='gti-html-cost-') as directory:
        root = Path(directory)
        shutil.copytree(repository / 'examples/demo', root / 'demo')
        interface = root / 'demo/html.gti'
        base_library = interface.read_text()
        for count in ((1, 4, 16, 64) if args.measure else (4, 64)):
            # The handwritten client uses the same reserve/append operations,
            # but spells the ordinary tree construction directly.
            expression = 'demo::detail::reserve_children(demo::detail::element("div"),' + str(count) + ')'
            for first in range(0, count, 8):
                expression = 'demo::detail::append(' + expression + ',' + ','.join(['handwritten_leaf(label)'] * min(8,count-first)) + ')'
            interface.write_text(base_library + '\ndemo::node handwritten_leaf(std::string_view label) {\n  return demo::detail::append(demo::detail::reserve_children(demo::detail::element("p"),1),demo::detail::child(label));\n}\nexport demo::node handwritten(std::string_view label) { return ' + expression + '; }\n')
            variants = {}
            for variant in ('handwritten', 'macro'):
                generated = 'handwritten(label)' if variant == 'handwritten' else 'html!({<div>' + '<p>{label}</p>' * count + '</div>})'
                source = root / (variant + '.gti')
                source.write_text('''import "demo/html.gti";
extern "C" {
  void adoption_count_begin(); void adoption_count_end();
  uint64_t adoption_allocations(); uint64_t adoption_bytes();
  void adoption_trace(int32_t value);
}
int main() {
  std::string_view label = "Ada & Lin";
  mut uint64_t checksum = 0;
  adoption_count_begin();
  for(mut int iteration=0;iteration<20000;iteration++) {
    auto page = ''' + generated + ''';
    auto text = demo::render(page);
    checksum += text.size();
  }
  adoption_count_end();
  adoption_trace(int32_t(checksum));
  adoption_trace(int32_t(adoption_allocations()));
  adoption_trace(int32_t(adoption_bytes()));
  return 0;
}
''')
                binary, mir = root / variant, root / (variant + '.mir')
                builds, memory = [], []
                for _ in range(3 if args.measure else 1):
                    timer = ['-l'] if sys.platform == 'darwin' else ['-v']
                    command = ['/usr/bin/time', *timer, str(args.compiler.resolve()), str(source),
                               '--backend', 'llvm', '-O3', '--dump-mir-after', str(mir),
                               '-o', str(binary), '--', str(provider)]
                    begin = time.perf_counter()
                    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
                    builds.append(time.perf_counter() - begin)
                    assert result.returncode == 0, result.stdout + result.stderr
                    pattern = r'(\d+)\s+maximum resident set size' if sys.platform == 'darwin' else r'Maximum resident set size \(kbytes\): (\d+)'
                    found = re.search(pattern, result.stderr)
                    assert found, result.stderr
                    memory.append(int(found.group(1)) * (1 if sys.platform == 'darwin' else 1024))
                assert not re.search(r'syntax-(?!kind=0\b)', mir.read_text()), 'procedural operation or capability survived in executable MIR'
                size = subprocess.check_output(['size', '-m' if sys.platform == 'darwin' else '-A', str(binary)], text=True)
                found = re.search(r'Section __text:\s+(\d+)' if sys.platform == 'darwin' else r'\.text\s+(\d+)', size)
                assert found, size
                variants[variant] = {'compile_seconds': builds, 'peak_rss_bytes': memory,
                                     'text_bytes': int(found.group(1)), 'executable_bytes': binary.stat().st_size,
                                     'runtime_seconds': []}
            for repetition in range(9 if args.measure else 3):
                for variant in (('macro','handwritten') if repetition % 2 else ('handwritten','macro')):
                    begin = time.perf_counter()
                    result = subprocess.run([str(root / variant)], capture_output=True, text=True, timeout=30)
                    assert result.returncode == 0 and not result.stderr, result
                    elapsed = time.perf_counter() - begin
                    values = list(map(int, result.stdout.split()))
                    assert len(values) == 3, result.stdout
                    variants[variant]['checksum_allocations_bytes'] = values
                    variants[variant]['runtime_seconds'].append(elapsed)
            assert variants['macro']['checksum_allocations_bytes'] == variants['handwritten']['checksum_allocations_bytes'], variants
            assert variants['macro']['checksum_allocations_bytes'][1] > 0, 'allocation provider did not observe tree construction'
            variants['runtime_ratio'] = statistics.median(variants['macro']['runtime_seconds']) / statistics.median(variants['handwritten']['runtime_seconds'])
            report[count] = variants
            print('HTML_COST ' + json.dumps({'children':count, **variants},sort_keys=True),flush=True)
        print('HTML_COST_COMPLETE ' + json.dumps(report,sort_keys=True))


if __name__ == '__main__':
    main()
