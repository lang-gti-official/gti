#!/usr/bin/env python3
"""External source macro packages, exact project cache identity and installed imports."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--install-build', type=Path)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    environment = dict(os.environ)
    for name in ('GTI_STDLIB_PATH', 'GTI_RUNTIME_PATH', 'GTI_RUNTIME_LIBRARY', 'GTI_RUNTIME_INCLUDE'):
        environment.pop(name, None)
    with tempfile.TemporaryDirectory(prefix='gti-procedural-library-') as temporary:
        root = Path(temporary)
        compiler = args.compiler.resolve()
        if args.install_build:
            stage = root / 'toolchain'
            subprocess.run(['cmake', '--install', str(args.install_build.resolve()),
                            '--prefix', str(stage), '--component', 'gti_toolchain'],
                           env=environment, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            compiler = stage / 'bin/gti'
        library, project = root / 'demo', root / 'app'
        shutil.copytree(repository / 'examples/demo', library)
        shutil.copytree(repository / 'examples/procedural-html-project', project)
        manifest = project / 'gti.toml'
        manifest.write_text(manifest.read_text() + '\n[profiles.dev]\nbackend = "llvm"\noptimization = 3\n')

        def invoke(*arguments, success=True):
            result = subprocess.run([str(compiler), *map(str, arguments)], cwd=project,
                                    env=environment, capture_output=True, text=True, timeout=180)
            assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
            return result

        metadata = json.loads(invoke('metadata').stdout)
        target = next(item for item in metadata['targets'] if item['name'] == 'app')
        output = Path(next(item for item in target['outputs'] if item['profile'] == 'dev')['executable'])
        first = invoke('build', 'app', '--verbose')
        assert 'gti: cache miss ' in first.stderr, first.stderr
        subprocess.run([output], check=True)
        original = output.read_bytes()
        output.unlink()
        warm = invoke('build', 'app', '--verbose')
        assert 'gti: cache hit ' in warm.stderr and output.read_bytes() == original, warm.stderr
        subprocess.run([output], check=True)
        # The helper is a transitive imported module, not a direct project root.
        # Change generated semantics while retaining the caller's exact bytes.
        helper = library / 'detail/markup.gti'
        helper_source = helper.read_text()
        helper.write_text(helper_source.replace('call("element", tag_literal, origin)',
            'call("element", std::syntax::string_literal("changed", origin), origin)'))
        changed = invoke('build', 'app', '--verbose')
        assert 'gti: cache miss ' in changed.stderr and output.read_bytes() != original, changed.stderr
        assert subprocess.run([output]).returncode == 1, 'transitive helper change was not reflected'
        helper.write_text(helper_source)
        restored = invoke('build', 'app', '--verbose')
        assert 'gti: cache hit ' in restored.stderr and output.read_bytes() == original, restored.stderr
        source = project / 'main.gti'
        source.write_text(source.read_text().replace('<div class=', '<div /* retained trivia */ class=', 1))
        trivia = invoke('build', 'app', '--verbose')
        assert 'gti: cache miss ' in trivia.stderr, trivia.stderr
        subprocess.run([output], check=True)
        # A body edit that changes the generated category must be checked anew,
        # never receive the previous node's compiled executable from the cache.
        interface = library / 'html.gti'
        interface.write_text(interface.read_text().replace('return demo::detail::page(input);',
            'return std::syntax::parse(std::syntax::literal("0", std::syntax::location(input, uint64_t(0), uint64_t(1))));'))
        invalid = invoke('build', 'app', '--verbose', success=False)
        assert 'error[' in invalid.stderr and 'cache hit' not in invalid.stderr, invalid.stderr
        print('external HTML import, cold/warm cache, helper/body/trivia invalidation passed; installed=' + str(bool(args.install_build)))


if __name__ == '__main__':
    main()
