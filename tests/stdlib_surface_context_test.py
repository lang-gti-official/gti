#!/usr/bin/env python3
"""Frozen stdlib setup/build/observation plans; no full corpus execution."""

from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

from llvm_case_context import CaseContext
import stdlib_surface_test as surface


def frozen_plan(case, compiler, cxx, runtime, repository, scratch):
    """Independent oracle copied from run_case at 1bd2cb2a."""
    case_root = scratch / case['id']
    environment = os.environ.copy()
    environment.update({'GTI_STDLIB_PATH': str(repository / 'stdlib'),
                        'GTI_RUNTIME_INCLUDE': str(repository / 'runtime' / 'include'),
                        'GTI_RUNTIME_LIBRARY': str(runtime)})
    optimizations = case['optimizations'] if case['mode'] == 'run' else [case['optimization']]
    steps = []
    for optimization in optimizations:
        output = case_root / f'program-{optimization}'
        command = [str(compiler), str(repository / case['source']), '--cxx', str(cxx), f'-{optimization}']
        if case.get('execution_profile') is not None:
            command += ['--execution-profile', str(case['execution_profile'])]
        steps.append({
            'optimization': optimization, 'command': command, 'executable': output,
            'build_options': {
                'native_sources': [repository / path for path in case.get('native_sources', [])],
                'optimization': optimization, 'cxx': cxx, 'cwd': case_root, 'env': environment,
                'text': True, 'capture_output': True,
                'timeout': case.get('build_timeout_seconds', 300), 'check': False,
            },
            'run_command': [str(output)],
            'run_options': {'cwd': case_root, 'input': case.get('stdin', ''),
                            'text': True, 'capture_output': True, 'timeout': 120, 'check': False},
        })
    return {'case_root': case_root, 'mode': case['mode'], 'expected': case['expected'],
            'files': {name: bytes.fromhex(record['contents'])
                      for name, record in case.get('files', {}).items()}, 'steps': steps}


def equivalent(case, root):
    args = (case, Path('@COMPILER'), Path('@CXX'), Path('@RUNTIME_LIBRARY'), root, Path('@SCRATCH'))
    actual = surface.case_plan(*args)
    assert actual == frozen_plan(*args), 'source/options/dependencies/environment/observation plan changed'
    contextual = surface.case_context(case, root)
    context = CaseContext.from_record(contextual['context'])
    assert context.provider == 'stdlib-surface' and context.identity == case['id']
    assert context.source == case['source']
    assert context.source_sha256 == hashlib.sha256((root / case['source']).read_bytes()).hexdigest()
    assert context.loading == 'stdlib-surface-native-fixture-with-compiler-overrides'
    assert set(context.unresolved) == {
        'llvm-target-applicability', 'native-fixture-build-contract', 'implicit-shipped-backend'}
    assert contextual['case'] == case and contextual['observed'] == 'unrun'
    assert contextual['role'] == ('positive-admission' if case['mode'] == 'run' else 'expected-rejection-control')
    assert contextual['runtime_environment'] == 'inherited-separately'
    assert contextual['compiler_environment'] == {
        'policy': 'copy-inherited-and-override',
        'overrides': {'GTI_STDLIB_PATH': '@REPOSITORY/stdlib',
                      'GTI_RUNTIME_INCLUDE': '@REPOSITORY/runtime/include',
                      'GTI_RUNTIME_LIBRARY': '@RUNTIME_LIBRARY'}}
    assert 'DO_NOT_SERIALIZE_INHERITED' not in json.dumps(contextual)
    token_plan = frozen_plan(case, Path('@COMPILER'), Path('@CXX'), Path('@RUNTIME_LIBRARY'),
                             Path('@REPOSITORY'), Path('@SCRATCH'))
    assert len(contextual['steps']) == len(token_plan['steps'])
    for record, step in zip(contextual['steps'], token_plan['steps'], strict=True):
        assert record['command'] == step['command'] and record['executable'] == str(step['executable'])
        assert record['native_sources'] == [str(path) for path in step['build_options']['native_sources']]
        assert record['build_options'] == {
            key: ([str(path) for path in value] if key == 'native_sources' else
                  str(value) if isinstance(value, Path) else value)
            for key, value in step['build_options'].items() if key != 'env'}
        assert record['cwd'] == str(step['build_options']['cwd'])
        assert record['build_timeout'] == step['build_options']['timeout']
        expected_runtime = ({'command': step['run_command'],
                             'options': {key: str(value) if isinstance(value, Path) else value
                                         for key, value in step['run_options'].items()}}
                            if case['mode'] == 'run' else None)
        assert record['runtime'] == expected_runtime


def rejects(action):
    try:
        action()
    except (AssertionError, ValueError):
        return
    raise AssertionError('stdlib context/observation mutation escaped validation')


def exercise(case, root, *, mutation=None):
    compiler, cxx, runtime = Path('/controlled/gti'), Path('/controlled/clang++'), Path('/controlled/runtime.a')
    builds, runs = [], []
    with tempfile.TemporaryDirectory(prefix='gti-stdlib-context-') as raw:
        scratch = Path(raw)
        expected = frozen_plan(case, compiler, cxx, runtime, root, scratch)

        def build(command, output, **options):
            step = expected['steps'][len(builds)]
            assert command == step['command'] and output == step['executable']
            assert options == step['build_options']
            assert options['cwd'].is_dir()
            for name, contents in expected['files'].items():
                assert (options['cwd'] / name).read_bytes() == contents
            builds.append(command)
            if case['mode'] == 'diagnostic':
                return subprocess.CompletedProcess(command,
                    0 if mutation == 'diagnostic-exit' else case['expected']['exit_code'],
                    'compiler output', '' if mutation == 'diagnostic-fragment'
                    else '\n'.join(case['expected']['stderr_contains']))
            return subprocess.CompletedProcess(command, 65 if mutation == 'build-failure' else 0,
                                               'compiler output', 'compiler warning')

        def run(command, **options):
            step = expected['steps'][len(runs)]
            assert command == step['run_command'] and options == step['run_options']
            assert 'env' not in options, 'compiler overrides leaked into runtime environment'
            runs.append(command)
            wanted = case['expected']
            return subprocess.CompletedProcess(command,
                7 if mutation == 'run-exit' else wanted['exit_code'],
                'changed output' if mutation == 'run-stdout' else wanted['stdout'],
                'changed diagnostic' if mutation == 'run-stderr' else wanted['stderr'])

        with patch.object(surface, 'build_fixture', side_effect=build), \
                patch.object(surface.subprocess, 'run', side_effect=run):
            surface.run_case(case, compiler, cxx, runtime, root, scratch)
        assert len(builds) == len(expected['steps'])
        assert len(runs) == (len(expected['steps']) if case['mode'] == 'run' else 0)


def main():
    root = Path(sys.argv[1]).resolve()
    manifest = surface.load_manifest(root / 'tests/stdlib_surface_manifest.json')
    surface.validate_manifest(manifest, root)
    cases = manifest['cases']
    assert cases and len({case['id'] for case in cases}) == len(cases)
    with patch.dict(os.environ, {'CONTEXT_INHERITED': 'DO_NOT_SERIALIZE_INHERITED'}, clear=True):
        for case in cases:
            equivalent(case, root)
            exercise(case, root)
        # Existing diagnostic and timeout branches are controlled examples,
        # not additions to the canonical manifest or native execution evidence.
        diagnostic = deepcopy(cases[0])
        diagnostic.update(id='controlled-diagnostic', mode='diagnostic', optimization='O3',
                          expected={'exit_code': 65, 'stderr_contains': ['GTI-CONTROL-A', 'GTI-CONTROL-B']},
                          build_timeout_seconds=450, build_timeout_reason='controlled existing override')
        diagnostic.pop('optimizations', None)
        equivalent(diagnostic, root)
        exercise(diagnostic, root)
        for mutation in ('diagnostic-exit', 'diagnostic-fragment'):
            rejects(lambda: exercise(diagnostic, root, mutation=mutation))
        control = deepcopy(cases[0])
        control['native_sources'] = ['tests/fixtures/second.cpp', 'tests/fixtures/first.cpp']
        control['files'] = {'control.bin': {'encoding': 'hex', 'contents': '41005aff'}}
        control['stdin'] = 'a\0b\n'
        control['execution_profile'] = 'single-threaded'
        equivalent(control, root)
        exercise(control, root)
        for mutation in ('build-failure', 'run-exit', 'run-stdout', 'run-stderr'):
            rejects(lambda: exercise(control, root, mutation=mutation))
        old_plan = surface.case_plan
        for field in ('environment', 'native-order', 'profile', 'runtime-env', 'timeout'):
            def mutated(*args):
                result = old_plan(*args)
                step = result['steps'][0]
                if field == 'environment':
                    step['build_options']['env'].pop('CONTEXT_INHERITED')
                elif field == 'native-order':
                    step['build_options']['native_sources'].reverse()
                elif field == 'profile':
                    step['command'] += ['--execution-profile', 'concurrent']
                elif field == 'runtime-env':
                    step['run_options']['env'] = step['build_options']['env']
                else:
                    step['build_options']['timeout'] = 1
                return result
            with patch.object(surface, 'case_plan', side_effect=mutated):
                rejects(lambda: equivalent(control, root))
    print(json.dumps({'equivalent_manifest_cases': len(cases), 'membership_unchanged': True,
                      'observations': 'controlled-only', 'llvm_execution': 'unrun',
                      'campaign_promotion_ready': False}))


if __name__ == '__main__':
    main()
