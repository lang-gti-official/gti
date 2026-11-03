#!/usr/bin/env python3
"""Explicit remote-runner production evidence for owner-operated preview releases.

This protocol is preview-only. It never fabricates a GitHub Actions run identity.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform as host
import re
import subprocess
import sys

import preview_release
from release_artifacts import PLATFORMS, PROFILE, digest

STEPS = ('configure', 'build', 'admission', 'installed', 'package')
MARKER = 'GTI_REMOTE_PREVIEW_PACKAGE='


def plan(sha, platform, output, jobs=4, test_jobs=4):
    preview_release.checked_sha(sha)
    if platform not in PLATFORMS or not Path(output).is_absolute():
        raise ValueError('reviewed platform and absolute export directory required')
    if not 1 <= jobs <= 64 or not 1 <= test_jobs <= 64:
        raise ValueError('invalid resource budget')
    return [{'name': name, 'kind': 'build' if name in ('configure', 'build', 'package') else 'suite',
             'cwd': 'source', 'argv': ['python3', '{source}/scripts/preview_remote.py', 'step', name,
             '--sha', sha, '--platform', platform, '--build', '{build}', '--output', str(output),
             '--jobs', str(jobs), '--test-jobs', str(test_jobs)]} for name in STEPS]


def execute(argv, *, env=None, cwd=None):
    subprocess.run(list(map(str, argv)), check=True, env=env, cwd=cwd)


def step(name, sha, platform, build, output, jobs, test_jobs):
    root = Path(__file__).resolve().parents[1]
    expected = {'darwin-arm64': ('Darwin', 'arm64'), 'linux-x64': ('Linux', 'x86_64')}[platform]
    if (host.system(), host.machine()) != expected:
        raise ValueError('execution host does not match release platform')
    if preview_release.command('git', '-C', str(root), 'rev-parse', 'HEAD') != sha:
        raise ValueError('source head changed')
    if preview_release.command('git', '-C', str(root), 'status', '--porcelain'):
        raise ValueError('release source is not clean')
    cc, cxx = (('/usr/bin/clang', '/usr/bin/clang++') if platform == 'darwin-arm64'
               else ('/usr/bin/gcc-14', '/usr/bin/g++-14'))
    env = dict(os.environ, CC=cc, CXX=cxx, CTEST_PARALLEL_LEVEL=str(test_jobs),
               GTI_TEST_JOBS=str(test_jobs))
    stage = build / 'preview-stage'
    if name == 'configure':
        query = build / '.cmake/api/v1/query'; query.mkdir(parents=True, exist_ok=True)
        (query / 'codemodel-v2').touch()
        execute(['cmake', '-S', root, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 '-DGTI_RELEASE_BUILD=ON', '-DGTI_BUNDLE_LLVM=ON', '-DBUILD_TESTING=ON',
                 '-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF', '-DCMAKE_C_COMPILER='+cc,
                 '-DCMAKE_CXX_COMPILER='+cxx], env=env)
    elif name == 'build':
        execute(['python3', root/'scripts/preview_build.py', '--source', root, '--build', build,
                 '--parallel', jobs], env=env)
    elif name == 'admission':
        execute(['python3', root/'scripts/check_ci_sanitizers.py', '--cxx',
                 '/usr/bin/clang++' if platform == 'darwin-arm64' else '/usr/bin/g++-14',
                 '--sanitizer', 'address,undefined'], env=env)
        execute(['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error',
                 '--output-junit', build/'Testing/remote-preview-admission.xml',
                 '-R', '^preview_admission$'], env=env)
    elif name == 'installed':
        execute(['cmake', '--install', build, '--config', 'Release', '--prefix', stage,
                 '--component', 'gti_toolchain'], env=env)
        env['PATH'] = str(stage/'bin') + os.pathsep + env['PATH']
        for key in ('GTI_STDLIB_PATH', 'GTI_RUNTIME_LIBRARY', 'GTI_RUNTIME_INCLUDE'):
            env.pop(key, None)
        commands = [
            ['lsp_smoke_test.py', 'gti_lsp'], ['cli_smoke_test.py', 'gti'],
            ['project_cli_smoke_test.py', 'gti'], ['process_resource_policy_test.py', 'gti', '--installed'],
            ['managed_thread_runtime_test.py', 'gti'],
            ['atomic_runtime_test.py', 'gti', *[root/'tests/fixtures'/('atomic_'+n+'.gti')
                for n in ('scalar', 'concurrent', 'orders', 'fetch', 'fetch_overflow')]]]
        for script, *args in commands:
            execute(['python3', root/'tests'/script, *args], env=env, cwd=build)
        consumer = build/'installed-library-smoke'
        execute(['cmake', '-S', root/'tests/installed_library_smoke', '-B', consumer, '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_PREFIX_PATH='+str(stage)], env=env)
        execute(['cmake', '--build', consumer, '--config', 'Release', '--parallel', jobs], env=env)
        execute(['ctest', '--test-dir', consumer, '-C', 'Release', '--parallel', test_jobs,
                 '--output-on-failure', '--no-tests=error', '--output-junit',
                 build/'Testing/remote-preview-installed.xml'], env=env)
    elif name == 'package':
        cache = (build/'CMakeCache.txt').read_text()
        for line in ('CMAKE_BUILD_TYPE:STRING=Release', 'GTI_RELEASE_BUILD:BOOL=ON', 'GTI_BUNDLE_LLVM:BOOL=ON'):
            if line not in cache.splitlines():
                raise ValueError('production release configuration is missing: '+line)
        preview_release.stage(root, stage, sha)
        output.mkdir(parents=True, exist_ok=False)
        execute(['cmake', '-DGTI_SOURCE_DIR='+str(root), '-DGTI_STAGE_DIR='+str(stage),
                 '-DGTI_OUTPUT_DIR='+str(output), '-DGTI_PLATFORM='+platform, '-DGTI_CHANNEL=preview',
                 '-DGTI_EXPECTED_TAG=preview', '-DGTI_BUILD_ID='+sha, '-P', root/'cmake/PackageRelease.cmake'], env=env)
        names = {f'gti-preview-{platform}.tar.gz', f'gti-preview-{platform}.tar.gz.sha256'}
        if {p.name for p in output.iterdir()} != names:
            raise ValueError('unexpected exported package inventory')
        print(MARKER + json.dumps({'sha': sha, 'platform': platform, 'profile': PROFILE,
                                  'files': {n: digest(output/n) for n in sorted(names)}}), flush=True)
    else:
        raise ValueError('unknown release step')


def receipt(results, sha, platform):
    """Require the complete known production plan, not merely a passed report."""
    report = json.loads((results/'report.json').read_text())
    request = json.loads((results/'request.json').read_text())
    if report.get('state') != 'passed' or report.get('exit_code') != 0:
        raise ValueError('remote preview run did not pass')
    for key in ('source', 'final_source'):
        if report.get(key, {}).get('head') != sha or report[key].get('status') != '':
            raise ValueError('remote preview source identity changed')
    run = report.get('id', '')
    if not re.fullmatch(r'run-[a-f0-9]{32}', run) or request.get('id') != run:
        raise ValueError('remote preview run identity mismatch')
    if request.get('build_type') != 'Release' or request.get('required_platform') != (
            'Darwin' if platform == 'darwin-arm64' else 'Linux'):
        raise ValueError('remote preview platform/configuration mismatch')
    snapshot = report['source'].get('snapshot_sha256', '')
    if not re.fullmatch(r'[a-f0-9]{64}', snapshot) or request.get('source') != report['source']:
        raise ValueError('remote snapshot identity mismatch')
    try:
        argv = request['steps'][0]['argv']
        value = lambda flag: argv[argv.index(flag)+1]
        expected = plan(sha, platform, value('--output'), int(value('--jobs')), int(value('--test-jobs')))
    except (KeyError, IndexError, ValueError, TypeError) as error:
        raise ValueError('invalid remote preview plan') from error
    if request['steps'] != expected or len(report.get('steps', [])) != len(expected):
        raise ValueError('remote preview admission plan was changed or incomplete')
    for actual, wanted in zip(report['steps'], expected):
        if actual.get('name') != wanted['name'] or actual.get('exit_code') != 0:
            raise ValueError('required remote preview step did not pass')
        # All expansions must refer to one run-owned source/build pair.
        actual_argv = actual.get('argv', [])
        if len(actual_argv) != len(wanted['argv']):
            raise ValueError('remote executed command differs from plan')
        source = actual_argv[1].removesuffix('/scripts/preview_remote.py')
        build = actual_argv[actual_argv.index('--build')+1]
        if Path(source).parent != Path(build).parent or Path(source).parent.name != run:
            raise ValueError('remote command escaped its run')
        if actual_argv != [a.replace('{source}', source).replace('{build}', build) for a in wanted['argv']]:
            raise ValueError('remote executed command differs from plan')
    truncated = json.loads((results/'truncation.json').read_text()).get('tail_only', [])
    if any(any(name in str(item) for name in ('admission', 'installed', 'package')) for item in truncated):
        raise ValueError('required preview evidence was truncated')
    rows = [line[len(MARKER):] for line in (results/'logs/package.log').read_text().splitlines()
            if line.startswith(MARKER)]
    if len(rows) != 1:
        raise ValueError('remote package receipt missing or ambiguous')
    package = json.loads(rows[0])
    names = {f'gti-preview-{platform}.tar.gz', f'gti-preview-{platform}.tar.gz.sha256'}
    if (package.get('sha') != sha or package.get('platform') != platform or package.get('profile') != PROFILE
            or set(package.get('files', {})) != names):
        raise ValueError('remote package identity mismatch')
    return {'schema': 2, 'executor': 'remote-runner', 'channel': 'preview', 'sha': sha,
            'platform': platform, 'profile': PROFILE, 'repository': preview_release.REPOSITORY,
            'run_id': run, 'snapshot_sha256': snapshot, 'files': package['files'],
            'report_sha256': digest(results/'report.json'), 'request_sha256': digest(results/'request.json')}


def verify(directory, sha, results, record=False):
    if set(results) != set(PLATFORMS):
        raise ValueError('both reviewed preview platforms are required')
    expected = set()
    for platform in PLATFORMS:
        document = receipt(Path(results[platform]), sha, platform)
        name = f'provenance-{platform}.json'; path = directory/name
        for filename, checksum in document['files'].items():
            asset = directory/filename
            if asset.is_symlink() or not asset.is_file() or digest(asset) != checksum:
                raise ValueError('remote preview package checksum mismatch: '+filename)
        if record:
            if path.is_symlink():raise ValueError('symlink provenance')
            path.write_text(json.dumps(document, sort_keys=True)+'\n')
        elif path.is_symlink() or json.loads(path.read_text()) != document:
            raise ValueError('remote preview provenance differs from retained evidence')
        expected.update(document['files']); expected.add(name)
    if {p.name for p in directory.iterdir()} != expected:
        raise ValueError('unexpected remote preview artifact set')


def admit(directory, sha, results, repo, candidate):
    """Import complete production evidence through the normal preview lifecycle."""
    from agent_workflow import Workflow
    from preview_workflow import Preview
    inclusion = preview_release.check_packages(directory, sha, None, None, remote_results=results)
    flow = Workflow(repo); preview = Preview(flow)
    with flow.lock():
        state = preview.load(candidate)
        preview.current(state)
        if state['head'] != sha or state['manifest'] != inclusion:
            raise ValueError('production artifacts differ from the prepared candidate')
        evidence = {p: receipt(Path(results[p]), sha, p) for p in PLATFORMS}
        state.update(phase='verified', verified={'head': sha, 'at': flow.now(),
                     'execution': {'kind': 'remote-production-preview', 'platforms': evidence},
                     'logs': {p: str(Path(results[p]).resolve()) for p in PLATFORMS},
                     'steps': {p: json.loads((Path(results[p])/'report.json').read_text())['steps']
                               for p in PLATFORMS}})
        preview.save(state)
    return preview.publish(candidate)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['plan', 'step', 'record', 'check', 'admit', 'publish'])
    parser.add_argument('step', nargs='?', choices=STEPS)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--platform', choices=PLATFORMS)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--test-jobs', type=int, default=4)
    parser.add_argument('--results', action='append', default=[], help='PLATFORM=retained remote results directory')
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--candidate')
    args = parser.parse_args(); preview_release.checked_sha(args.sha)
    if args.mode == 'plan':
        print(json.dumps(plan(args.sha, args.platform, args.output, args.jobs, args.test_jobs), indent=2))
    elif args.mode == 'step':
        step(args.step, args.sha, args.platform, args.build, args.output, args.jobs, args.test_jobs)
    else:
        results = dict(item.split('=', 1) for item in args.results)
        if args.mode == 'record':verify(args.output, args.sha, results, record=True)
        elif args.mode == 'check':preview_release.check_packages(args.output, args.sha, None, None, remote_results=results)
        elif args.mode == 'admit':
            if args.repo is None or args.candidate is None:parser.error('admit requires --repo and --candidate')
            print(json.dumps(admit(args.output, args.sha, results, args.repo, args.candidate), indent=2))
        else:
            token = os.environ.get('GH_TOKEN') or subprocess.check_output(['gh', 'auth', 'token'], text=True).strip()
            preview_release.publish(args.output, args.sha, preview_release.github_request(token), None, None,
                                    remote_results=results)


if __name__ == '__main__':
    main()
