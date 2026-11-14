#!/usr/bin/env python3
"""RFC 0003 executable contract for general retaining stores and replacement.

These programs deliberately exercise local values and unrelated user records,
not a recognized container spelling. During implementation, --case selects an
individual invariant; the full invocation requires every case to pass.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

PREFIX = 'import std.string;\nimport std.utility;\n'
CASES = {
    'local_replacement': (True, '''int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view();
  view = second.view();
  return view == "second" ? 0 : 1;
}'''),
    'replacement_releases_old_owner': (True, '''int main() {
  mut std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view();
  view = second.view(); first.clear();
  return view == "second" ? 0 : 1;
}'''),
    'copy_retains_old_owner': (False, '''int main() {
  mut std::string first{"first"}; std::string second{"second"};
  mut std::string_view view = first.view(); auto copy = view;
  view = second.view(); first.clear();
  return copy == "first" && view == "second" ? 0 : 1;
}'''),
    'replacement_protects_new_owner': (False, '''int main() {
  std::string first{"first"}; mut std::string second{"second"};
  mut std::string_view view = first.view();
  view = second.view(); second.clear();
  return view == "second" ? 0 : 1;
}'''),
    'local_escape_through_store': (False, '''int main() {
  mut std::string_view view = "initial";
  { std::string local{"short"}; view = local.view(); }
  return view == "short" ? 0 : 1;
}'''),
    'two_owner_record': (True, '''struct Labels<lifetime a, lifetime b> {
  std::string_view<a> first; std::string_view<b> second;
  Labels(std::string_view<a> x, std::string_view<b> y):first(x),second(y){}
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  Labels labels{first.view(),second.view()};
  return labels.first == "first" && labels.second == "second" ? 0 : 1;
}'''),
}


# Copy and move preserve both dependencies without changing runtime contents.
for operation in ('copy', 'move'):
    _, source = CASES['two_owner_record']
    initializer = 'labels' if operation == 'copy' else 'std::move(labels)'
    source = source.replace('  return labels.first',
                            '  auto transferred = ' + initializer + ';\n  return transferred.first')
    source = source.replace('&& labels.second', '&& transferred.second')
    CASES['two_owner_record_' + operation] = (True, source)


# The same aggregate publication must retain every owner through copies.
for owner in ('first', 'second'):
    for copy in (False, True):
        _, source = CASES['two_owner_record']
        source = source.replace('std::string ' + owner + '{', 'mut std::string ' + owner + '{')
        source = source.replace('  return labels.first',
            ('  auto copied = labels;\n' if copy else '') +
            '  ' + owner + '.clear();\n  return labels.first')
        if copy:
            source = source.replace('return labels.first', 'return copied.first').replace('&& labels.second', '&& copied.second')
        CASES['record_' + owner + '_owner_write' + ('_copy' if copy else '')] = (False, source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=sorted(CASES))
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    evidence = {'cases': {}, 'steps': []}
    environment = dict(os.environ)
    for key in ('SDKROOT', 'DEVELOPER_DIR', 'MACOSX_DEPLOYMENT_TARGET'):
        environment.pop(key, None)

    def run(label, command):
        result = subprocess.run(list(map(str, command)), capture_output=True,
                                text=True, timeout=240, env=environment, cwd=output)
        (output / (label + '.log')).write_text(result.stdout + result.stderr)
        evidence['steps'].append({'label': label, 'command': list(map(str, command)),
                                  'exit': result.returncode})
        return result

    try:
        for name in args.case or CASES:
            accepted, source = CASES[name]
            path = output / (name + '.gti')
            path.write_text(PREFIX + source + '\n')
            passed = True
            for mode in ['O0', 'O3']:
                label = name + '-' + mode
                binary = output / label
                result = run(label + '-compile',
                             [compiler, path, '--backend', 'llvm', '-' + mode, '-o', binary])
                if accepted:
                    ok = result.returncode == 0
                    if ok:
                        executed = run(label + '-execute', [binary])
                        ok = executed.returncode == 0 and not executed.stdout and not executed.stderr
                else:
                    # A parser failure, ICE or backend failure is not a borrow rejection.
                    diagnostic = result.stdout + result.stderr
                    ok = result.returncode != 0 and bool(re.search(r'error\[GTI-S\d+\]', diagnostic))
                    ok = ok and 'GTI-B0001' not in diagnostic
                    # Admission failures do not prove owner protection.
                    ok = ok and 'single-owner' not in diagnostic
                    ok = ok and 'second owner dependency' not in diagnostic
                    if name.startswith('record_'):
                        ok = ok and 'error[GTI-S2017]' in diagnostic
                        ok = ok and 'Retained borrow originates here.' in diagnostic
                if not ok:
                    print(label, (result.stdout + result.stderr)[-2500:], flush=True)
                passed = passed and ok
                binary.unlink(missing_ok=True)
            evidence['cases'][name] = {'expected_acceptance': accepted, 'passed': passed}
            print(name, 'PASS' if passed else 'FAIL', flush=True)
    finally:
        (output / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    return 0 if all(item['passed'] for item in evidence['cases'].values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
