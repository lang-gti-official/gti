#!/usr/bin/env python3
"""Rebuild and run the adapter from the installed SDK without compiler targets."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import source_stdlib_ci


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def main():
    build = Path(sys.argv[1]).resolve().parent
    # The parser argument makes its build dependency visible to test-target
    # selection. All other exported archives belong to the compiler target.
    assert Path(sys.argv[2]).is_file()
    with tempfile.TemporaryDirectory(prefix='lsp-sdk-', dir=build) as temporary:
        work = Path(temporary); stage = work / 'stage'; adapter = work / 'adapter'
        run('cmake', '--install', build, '--prefix', stage, '--component', 'gti_toolchain')
        query = adapter / '.cmake/api/v1/query'; query.mkdir(parents=True)
        (query / 'codemodel-v2').touch()
        llvm = next((line.split('=', 1)[1] for line in (build / 'CMakeCache.txt').read_text().splitlines() if line.startswith('LLVM_DIR:')), '')
        run('cmake', '-S', ROOT, '-B', adapter, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DGTI_REUSE_TOOLCHAIN='+str(stage), '-DGTI_REBUILD_LSP=ON',
            '-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF', *(['-DLLVM_DIR='+llvm] if llvm else []))
        pattern = '^(lsp_protocol|frontend_cache_invalidation|lsp_project_facts|lsp_configuration)$'
        targets = source_stdlib_ci.consumer_targets(adapter, pattern, native_roots=(ROOT / 'src/lsp',))
        run('cmake', '--build', adapter, '--parallel', '2', '--target', *targets)
        run('ctest', '--test-dir', adapter, '--output-on-failure', '--no-tests=error', '-R', pattern)
        # Relocate the rebuilt executable with its SDK resources and exercise
        # the installed discovery path as well as the build-tree adapter.
        import shutil
        shutil.copy2(adapter / 'gti_lsp', stage / 'bin/gti_lsp')
        run(sys.executable, ROOT / 'tests/lsp_smoke_test.py', stage / 'bin/gti_lsp')
        print('Installed SDK adapter rebuild and protocol/frontend/project checks passed')


if __name__ == '__main__':
    main()
