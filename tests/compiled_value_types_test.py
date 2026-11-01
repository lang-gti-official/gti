#!/usr/bin/env python3
"""Execute source-absent nominal GTI APIs with their ordinary type/access rules."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from project_cli_smoke_test import DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    output = args.output_dir.resolve(); output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT: env.pop(key, None)
    fixture = Path(__file__).resolve().parents[1] / 'examples/compiled-value-types'
    evidence = {'compiler_sha256': hashlib.sha256(compiler.read_bytes()).hexdigest(), 'steps': []}
    with tempfile.TemporaryDirectory(prefix='gti-nominal-library-') as scratch:
        def run(project, command, name, success=True, contains=None):
            result = subprocess.run([str(compiler), *command, '--color=never'], cwd=project,
                                    env=env, text=True, capture_output=True, timeout=120)
            log = output / (name + '.log'); log.write_text(result.stdout + result.stderr)
            evidence['steps'].append({'name': name, 'exit': result.returncode, 'log': str(log)})
            (output / 'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')
            assert (result.returncode == 0) == success, f'{name}: {log}'
            if contains: assert contains in result.stdout + result.stderr, f'{name}: {log}'
        for level in (0, 1, 3):
            root = Path(scratch) / str(level); shutil.copytree(fixture, root)
            producer, consumer = root/'producer', root/'consumer'
            for project in (producer, consumer):
                with (project/'gti.toml').open('a') as f: f.write(f'\n[profiles.release]\noptimization = {level}\n')
            prefix = f'O{level}-'
            run(producer, ['build','geometry','--release'], prefix+'produce')
            bundle = next((producer/'build').rglob('geometry.gtilib'))
            (consumer/'dependencies').mkdir(); shutil.copyfile(bundle, consumer/'dependencies/geometry.gtilib')
            original = bundle.read_bytes(); shutil.rmtree(producer)
            run(consumer, ['check','app','--release'], prefix+'check')
            run(consumer, ['run','app','--release'], prefix+'run')
            source = consumer/'main.gti'
            source.write_text('import "api.gti"; int main() { Secret value{}; return value.code; }\n')
            run(consumer, ['check','app','--release'], prefix+'private-field', False, 'private')
            source.write_text('import "api.gti"; int main() { Hidden hidden{}; return 0; }\n')
            run(consumer, ['check','app','--release'], prefix+'private-type', False)
            source.write_text('import "api.gti"; int main() { return sum(Status::ready); }\n')
            run(consumer, ['check','app','--release'], prefix+'wrong-nominal', False)
            assert (consumer/'dependencies/geometry.gtilib').read_bytes() == original
            assert not (consumer/'api.gti').exists()
    print(output/'evidence.json')
if __name__ == '__main__': main()
