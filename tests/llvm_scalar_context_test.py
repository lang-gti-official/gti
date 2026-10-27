#!/usr/bin/env python3
"""Scalar context/plan equivalence controls; real execution is separate."""

from contextlib import redirect_stdout
from copy import deepcopy
from dataclasses import replace
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
from unittest.mock import patch

from backend_execution import Endpoint, ExecutionResult
from llvm_evidence_manifest import manifest_digest, validate_context_sidecar
import llvm_scalar_execution_test as scalar


# Positive source/profile facts frozen before extraction, reconciled to v2's
# explicit execution profile. This is a regression oracle, not a registry.
FROZEN_MANIFEST = "9a896985196618a95349cb42552e8ed0045d601c465175f40b9737b98b11a342"
FROZEN_SUCCESSES = (
    'scalars', 'widths', 'coverage-constructions', 'shipped-example', 'globals',
    'switch-default', 'integral-enum-underlying', 'dormant-owner',
    'numeric-intrinsics',
)
FROZEN_DIAGNOSTICS = {
    'signed-add': 'GTI-R0001', 'signed-sub': 'GTI-R0001', 'signed-mul': 'GTI-R0001',
    'unsigned-add': 'GTI-R0001', 'unsigned-sub': 'GTI-R0001', 'negate': 'GTI-R0001',
    'division': 'GTI-R0002', 'division-overflow': 'GTI-R0001', 'modulo': 'GTI-R0003',
    'negative-shift': 'GTI-R0004', 'wide-shift': 'GTI-R0005', 'integer-narrow': 'GTI-R0006',
    'float-high': 'GTI-R0006', 'float-low': 'GTI-R0006', 'float-unsigned': 'GTI-R0006',
    'float-nan': 'GTI-R0006', 'float-infinity': 'GTI-R0006', 'compound-narrow': 'GTI-R0006',
    'panic': 'GTI-R0017', 'empty-panic': 'GTI-R0017', 'truncated-panic': 'GTI-R0017',
    'assertion': 'GTI-R0017', 'initializer': 'GTI-R0001',
}
FROZEN_CONTROLS = {
    'polymorphic-replacement': '162a8ef5b3b9dc3b7c95bd6ff1a7ffb4e76cc4b3dfd55e335cfe0f1439397890',
    'polymorphic-array': '966e991114ab46d129af01c45c0277cb7819bbac52f36432fe5f4778234d7835',
    'polymorphic-tls': '0c6a904d28f62b5be56d8cd1f15f515ba478da38e444ceabe3a0055d89d28e55',
}
FROZEN_ENDPOINTS = (Endpoint('cpp', 'O3', 'c++23'), Endpoint('llvm', 'O0'), Endpoint('llvm', 'O3'))


def expectation(name: str) -> dict:
    if name in FROZEN_SUCCESSES:
        return {'kind': 'success', 'exit_code': 0, 'stdout': '', 'stderr': '',
                'differential': 'exact-exit-stdout-stderr'}
    if name in FROZEN_DIAGNOSTICS:
        return {'kind': 'defined-failure', 'exit_code': 70, 'diagnostic': FROZEN_DIAGNOSTICS[name],
                'differential': 'exact-exit-stdout-stderr'}
    return {'kind': 'backend-rejection', 'exit_code': 65, 'diagnostic': 'GTI-B0003',
            'preserved_output_hex': b'existing artifact'.hex()}


def frozen_plans(name: str, gti: Path, root: Path, work: Path) -> list[dict]:
    control = name in FROZEN_CONTROLS
    source = work / (f'reject-{name}.gti' if control else f'{name}.gti')
    if control:
        return [{'command': [str(gti), str(source), '--backend', 'llvm', '-o', str(work / f'reject-{name}')],
                 'options': {'capture_output': True, 'text': True, 'timeout': 60},
                 'cwd_policy': 'inherited', 'environment_policy': 'inherited'}]
    results = []
    for endpoint in FROZEN_ENDPOINTS:
        output = work / f'{name}-{endpoint.backend}-{endpoint.optimization}' / (
            'program.exe' if os.name == 'nt' else 'program')
        command = [str(gti), str(source), '--backend', endpoint.backend, f'-{endpoint.optimization}']
        if endpoint.standard:
            command += ['--std', endpoint.standard]
        command += ['-o', str(output)]
        results.append({'command': command, 'cwd': str(root), 'environment_policy': 'copy-inherited',
                        'build_timeout': 240, 'run_timeout': 15})
    return results


def equivalent(root: Path, cases: dict, sidecar: dict) -> None:
    manifest = scalar.evidence_manifest(root)
    assert manifest_digest(manifest) == FROZEN_MANIFEST, 'membership/source/profile drift'
    validate_context_sidecar(sidecar, manifest,
                             expected_partition_policy=scalar.evidence_partition_policy(root))
    assert sidecar['partition_policy'] == {
        'cases': 'positive-admission', 'controls': 'expected-rejection-control'}
    assert set(cases) == set(FROZEN_SUCCESSES) | set(FROZEN_DIAGNOSTICS) | set(FROZEN_CONTROLS)
    assert set(sidecar['cases']) == set(manifest['sources'])
    assert set(sidecar['controls']) == set(FROZEN_CONTROLS)
    assert all(sidecar['cases'][name]['role'] == 'positive-admission' for name in FROZEN_DIAGNOSTICS)
    for name, case in cases.items():
        control = name in FROZEN_CONTROLS
        record = sidecar['controls' if control else 'cases'][name]
        assert record == case.sidecar_record(), 'context does not project actual harness data'
        assert case.expectation == expectation(name), 'assertion policy drift'
        assert record['role'] == ('expected-rejection-control' if control else 'positive-admission')
        assert case.context.materialized_name == (f'reject-{name}.gti' if control else f'{name}.gti')
        assert case.context.execution_profile == 'single-threaded'
        assert case.context.loading == 'materialized-source-with-inherited-compiler-environment'
        assert set(case.context.unresolved) == {
            'host-target-applicability', 'effective-compiler-environment', 'implicit-toolchain-runtime'}
        expected_hash = FROZEN_CONTROLS[name] if control else manifest['sources'][name]['sha256']
        assert hashlib.sha256(case.source_text.encode()).hexdigest() == expected_hash
        assert case.context.source_sha256 == expected_hash
        assert case.endpoints == (() if control else FROZEN_ENDPOINTS), 'endpoint order/flags changed'
        assert scalar.scalar_command_plans(case, name, Path('/controlled/gti'), root,
                                           Path('/controlled/work')) == frozen_plans(
                                               name, Path('/controlled/gti'), root, Path('/controlled/work'))


def rejects(action) -> None:
    try:
        action()
    except (AssertionError, ValueError):
        return
    raise AssertionError('context or oracle mutation was accepted')


def exercise_main(root: Path, cases: dict, selection: str | None = None,
                  mutation: str | None = None) -> list[tuple]:
    visited = []
    inherited = dict(os.environ)

    def execute(gti, actual_root, output, source, endpoint, env, **options):
        name = source.stem
        assert actual_root == root and env == inherited
        assert options == {'build_timeout': 240, 'run_timeout': 15}
        assert output.name == f'{name}-{endpoint.backend}-{endpoint.optimization}'
        assert source.read_text() == cases[name].source_text
        plan = next(plan for plan in frozen_plans(name, gti, root, source.parent)
                    if plan['command'][3:5] == [endpoint.backend, f'-{endpoint.optimization}'])
        visited.append((name, endpoint.backend, endpoint.optimization))
        if name in FROZEN_SUCCESSES:
            stdout = 'unexpected' if mutation == 'success-output' else ''
            return ExecutionResult(tuple(plan['command']), 0, 0, stdout, '')
        stderr = FROZEN_DIAGNOSTICS[name] + f' source/failure attribution: {source}'
        if mutation == 'missing-diagnostic':
            stderr = 'not a defined failure'
        if mutation == 'differential' and endpoint.backend == 'llvm' and endpoint.optimization == 'O3':
            stderr += ' changed attribution'
        return ExecutionResult(tuple(plan['command']), 0,
                               0 if mutation == 'failure-exit' else 70, '', stderr)

    def reject(command, **options):
        source = Path(command[1])
        name = source.stem.removeprefix('reject-')
        expected = frozen_plans(name, Path(command[0]), root, source.parent)[0]
        assert command == expected['command'] and options == expected['options']
        assert source.read_text() == cases[name].source_text
        output = Path(command[-1])
        assert output.read_bytes() == b'existing artifact'
        visited.append((name, 'llvm', None))
        if mutation == 'replaced-output':
            output.write_bytes(b'changed')
        return subprocess.CompletedProcess(command, 0 if mutation == 'rejection-exit' else 65,
                                           '', '' if mutation == 'rejection-diagnostic' else 'GTI-B0003')

    args = ['llvm_scalar_execution_test.py', '/controlled/gti', str(root)]
    if selection:
        args.append(selection)
    with patch.object(scalar, 'execute', side_effect=execute), \
            patch.object(scalar.subprocess, 'run', side_effect=reject), \
            patch.object(sys, 'argv', args), redirect_stdout(io.StringIO()):
        scalar.main()
    return visited


def main() -> None:
    root = Path(sys.argv[1]).resolve()
    cases = scalar.scalar_cases(root)
    sidecar = scalar.evidence_context(root)
    equivalent(root, cases, sidecar)
    expected_visits = [(name, endpoint.backend, endpoint.optimization)
                       for name in (*FROZEN_SUCCESSES, *FROZEN_DIAGNOSTICS)
                       for endpoint in FROZEN_ENDPOINTS]
    expected_visits += [(name, 'llvm', None) for name in FROZEN_CONTROLS]
    assert exercise_main(root, cases) == expected_visits
    for name in cases:
        assert exercise_main(root, cases, name) == [row for row in expected_visits if row[0] == name]
    for selection, mutation in (
        ('scalars', 'success-output'), ('signed-add', 'failure-exit'),
        ('signed-add', 'missing-diagnostic'), ('signed-add', 'differential'),
        ('polymorphic-replacement', 'replaced-output'), ('polymorphic-replacement', 'rejection-exit'),
        ('polymorphic-replacement', 'rejection-diagnostic'),
    ):
        rejects(lambda: exercise_main(root, cases, selection, mutation))
    for name, changed in (
        ('signed-add', replace(cases['signed-add'], expectation={**cases['signed-add'].expectation, 'exit_code': 0})),
        ('scalars', replace(cases['scalars'], endpoints=(Endpoint('llvm', 'O2'),))),
        ('scalars', replace(cases['scalars'], source_text='int main() { return 1; }')),
        ('scalars', replace(cases['scalars'], build_timeout=1)),
        ('scalars', replace(cases['scalars'], context=replace(cases['scalars'].context,
                                                           materialized_name='other.gti'))),
    ):
        altered = dict(cases)
        altered[name] = changed
        rejects(lambda: equivalent(root, altered, sidecar))
    for mutate in (
        lambda value: value['cases'].pop('scalars'),
        lambda value: value['cases'].__setitem__('extra', value['cases']['scalars']),
        lambda value: value.__setitem__('observed', 'passed'),
        lambda value: value.__setitem__('manifest_sha256', '0' * 64),
        lambda value: value['profiles'].pop('scalars'),
        lambda value: value['cases']['scalars']['context'].__setitem__('source_sha256', '0' * 64),
        lambda value: value['cases']['scalars'].__setitem__('role', {}),
        lambda value: value['cases']['scalars'].__setitem__('role', 'expected-rejection-control'),
        lambda value: value['controls']['polymorphic-replacement'].__setitem__('role', 'positive-admission'),
        lambda value: value['cases']['signed-add'].__setitem__('role', 'expected-rejection-control'),
        lambda value: value.pop('partition_policy'),
        lambda value: value['partition_policy'].__setitem__('cases', 'expected-rejection-control'),
        lambda value: value['cases']['scalars']['observation'].__setitem__('expectation', 'named assertion'),
    ):
        changed = deepcopy(sidecar)
        mutate(changed)
        rejects(lambda: validate_context_sidecar(changed, scalar.evidence_manifest(root)))
    for partition, role in (('cases', 'expected-rejection-control'), ('controls', 'positive-admission')):
        coordinated = deepcopy(sidecar)
        coordinated['partition_policy'][partition] = role
        for record in coordinated[partition].values():
            record['role'] = role
        # Generic consistency is not authority to reinterpret this provider.
        validate_context_sidecar(coordinated, scalar.evidence_manifest(root))
        rejects(lambda: validate_context_sidecar(
            coordinated, scalar.evidence_manifest(root),
            expected_partition_policy=scalar.evidence_partition_policy(root)))
    print(json.dumps({'v2_manifest_unchanged': FROZEN_MANIFEST,
                      'positive_cases': len(sidecar['cases']),
                      'separate_controls': len(sidecar['controls']),
                      'execution': 'controlled-protocol-only', 'campaign_promotion_ready': False}))


if __name__ == '__main__':
    main()
