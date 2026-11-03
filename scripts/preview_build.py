#!/usr/bin/env python3
"""Build the installed preview payload and every admitted test's native targets."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

import validation_checks


def required_tests(source, platform):
    # The candidate owns its example and inclusion policy, not the publisher's
    # potentially newer checkout. This is the same code preview_admission runs.
    spec = importlib.util.spec_from_file_location('preview_candidate_examples', source / 'scripts/preview_examples.py')
    examples = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(examples)
    manifest = source / '.gti-preview.json'
    # Older candidates only know the whole-manifest replay.
    focused = (examples.pending_tests(manifest, source) if hasattr(examples, 'pending_tests')
               else examples.manifest_tests(manifest))
    selected = examples.required_tests(platform) | set(focused)
    if not selected or 'preview_admission' in selected:
        raise ValueError('empty or recursive preview selection')
    examples.validate_sources(subprocess.check_output(
        ['git', '-C', str(source), 'ls-files', '-z', '--', 'examples'], text=True).split('\0'))
    return selected


def build_targets(source, build, selected):
    cache = dict(re.findall(r'^([^/#\r\n][^:\r\n]*):[^=\r\n]+=([^\r\n]*)$',
                            (build / 'CMakeCache.txt').read_text(), re.M))
    if (Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != source or
            cache.get('CMAKE_BUILD_TYPE') not in {'Release', 'RelWithDebInfo', 'MinSizeRel'}):
        raise ValueError('preview requires this source with an optimized build')
    reply = build / '.cmake/api/v1/reply'
    index = json.loads(sorted(reply.glob('index-*.json'))[-1].read_text())
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
    configurations = model['configurations']
    if len(configurations) != 1 or Path(model['paths']['source']).resolve() != source:
        raise ValueError('preview requires a single-configuration source build')
    configuration = configurations[0]
    targets = {row['id']: json.loads((reply / row['jsonFile']).read_text())
               for row in configuration['targets']}
    artifacts = {str((build / item['path']).resolve()): row['name']
                 for row in targets.values() for item in row.get('artifacts', [])}
    chosen = set()
    for directory in configuration['directories']:
        for install in json.loads((reply / directory['jsonFile']).read_text()).get('installers', []):
            if install.get('component') != 'gti_toolchain':
                continue
            if install.get('targetId'):
                chosen.add(targets[install['targetId']]['name'])
            # Bundled LLVM archives/libclang use install(FILES TARGET_FILE),
            # rather than install(TARGETS); retain those artifact owners too.
            for item in install.get('paths', []):
                path = item['from'] if isinstance(item, dict) else item
                name = artifacts.get(str((build / path).resolve()))
                if name:
                    chosen.add(name)
    if not {'gti', 'gti_lsp', 'gti_tree_sitter'} <= chosen:
        raise ValueError('missing required installed preview product targets')
    matched = set()
    for file in build.rglob('CTestTestfile.cmake'):
        for match in validation_checks.TEST_LINE.finditer(file.read_text()):
            name = validation_checks.cmake_argument(match.group('name'))
            if name not in selected:
                continue
            matched.add(name)
            for token in validation_checks.QUOTED.finditer(match.group('arguments')):
                argument = token.group(1) if token.group(1) is not None else token.group(3)
                for part in argument.split('='):
                    # Exact artifacts only: scratch directories and Neovim's
                    # XDG paths under the build tree are not build targets.
                    owner = artifacts.get(part)
                    if owner:
                        chosen.add(owner)
    if matched != selected:
        raise ValueError(f'missing configured preview tests: {sorted(selected - matched)}')
    return sorted(chosen)


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('parallelism must be positive')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--parallel', type=positive, required=True)
    parser.add_argument('--products-only', action='store_true',
                        help='build only the installed toolchain products; the publisher runs no admission checks')
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    selected = set() if args.products_only else required_tests(source, sys.platform)
    targets = build_targets(source, build, selected)
    evidence = build / 'Testing/preview-build.json'
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(json.dumps({'tests': sorted(selected), 'targets': targets}, indent=2) + '\n')
    print(f'Preview: {len(selected)} required checks, {len(targets)} product/test targets', flush=True)
    subprocess.run(['cmake', '--build', str(build), '--config', 'Release', '--parallel', str(args.parallel),
                    '--target', *targets], check=True)


if __name__ == '__main__':
    main()
