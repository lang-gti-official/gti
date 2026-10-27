#!/usr/bin/env python3
"""Real debug object/cache/companion publication; no debugger bypass."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gti', type=Path)
    parser.add_argument('--backend', choices=('llvm', 'cpp'), default='llvm')
    args = parser.parse_args()
    gti = args.gti.resolve()
    with tempfile.TemporaryDirectory(prefix='gti-debug-artifacts-') as directory:
        root = Path(directory).resolve()
        (root / 'src').mkdir()
        source = root / 'src/main.gti'
        def write_source(value):
            source.write_text('int value(int input) {\n  return input + 1;\n}\n'
                              f'int main() {{ return value({value - 1}); }}\n')
        write_source(17)
        (root / 'gti.toml').write_text(
            'manifest-version = 1\n[package]\nname = "debug-artifacts"\n'
            'version = "0.1.0"\n[targets.app]\nkind = "executable"\n'
            'root = "src/main.gti"\n[profiles.dev]\n'
            f'backend = "{args.backend}"\noptimization = 0\ndebug-info = true\n')
        def run(argv, expected=0):
            result = subprocess.run([str(x) for x in argv], cwd=root,
                                    capture_output=True, text=True, timeout=90)
            assert result.returncode == expected, (argv, result.returncode,
                                                    result.stdout, result.stderr)
            return result
        def build(ok=True):
            argv = [gti, 'build', 'app', '--verbose']
            result = subprocess.run([str(x) for x in argv], cwd=root,
                                    capture_output=True, text=True, timeout=90)
            assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
            return result
        model = json.loads(run([gti, 'metadata']).stdout)
        target = next(x for x in model['targets'] if x['name'] == 'app')
        dev = next(x for x in target['outputs'] if x['profile'] == 'dev')
        output = Path(dev['executable'])
        bundle = Path(str(output) + '.dSYM')
        first = build()
        assert 'gti: cache miss ' in first.stderr, first.stderr
        run([output], 17)
        cache = [x for x in (root / 'build').rglob('metadata')
                 if x.read_text().startswith('gti-build-cache-')]
        assert len(cache) == 1, cache
        entry = cache[0].parent
        original = output.read_bytes()
        if args.backend == 'llvm':
            obj = entry / 'artifact.o'
            assert obj.is_file() and not (entry / 'generated.cpp').exists()
            assert b'__debug_info' in obj.read_bytes() if sys.platform == 'darwin' else b'.debug_info' in obj.read_bytes()
        if sys.platform == 'darwin':
            def uuid(path):
                text = run(['dwarfdump', '--uuid', path]).stdout
                values = set(re.findall(r'UUID: ([A-Fa-f0-9-]+)', text))
                assert values, text
                return values
            assert uuid(output) == uuid(bundle)
            dwarf = run(['dwarfdump', '--debug-info', bundle]).stdout
            if args.backend == 'llvm':
                assert 'main.gti' in dwarf and 'input' in dwarf, dwarf[:2000]
            assert (bundle / '.gti-debug-symbols').read_text() == 'gti-debug-symbols-v1\n'
        else:
            text = run(['readelf', '--debug-dump=decodedline', output]).stdout
            assert 'main.gti' in text, text[:2000]
        output.unlink()
        if sys.platform == 'darwin':
            shutil.rmtree(bundle)
        restored = build()
        assert 'gti: cache hit ' in restored.stderr, restored.stderr
        assert output.read_bytes() == original
        run([output], 17)
        if sys.platform == 'darwin':
            assert uuid(output) == uuid(bundle)
            # Preserve a different valid executable while the cached request is
            # rejected by an unowned companion. Old code overwrote it first.
            write_source(23)
            build()
            run([output], 23)
            previous = output.read_bytes()
            saved = root / 'saved.dSYM'
            bundle.rename(saved)
            bundle.mkdir()
            sentinel = bundle / 'keep-user-data'
            sentinel.write_text('preserve')
            write_source(17)
            denied = build(False)
            assert 'not owned by GTI' in denied.stderr, denied.stderr
            assert output.read_bytes() == previous, 'failed companion restore replaced executable'
            assert sentinel.read_text() == 'preserve'
            run([output], 23)
            shutil.rmtree(bundle)
            saved.rename(bundle)
            hit = build()
            assert 'gti: cache hit ' in hit.stderr, hit.stderr
            assert output.read_bytes() == original
            assert uuid(output) == uuid(bundle)
            run([output], 17)
            def tree_bytes(path):
                return {str(p.relative_to(path)): p.read_bytes()
                        for p in path.rglob('*') if p.is_file()}
            # Force final executable rename to fail after symbol publication.
            # The existing executable is held by the fixture, while a directory
            # blocks its destination. Cold and cache paths must restore symbols.
            for cached, value in ((False, 31), (True, 23)):
                if cached:
                    write_source(23)
                    build()
                previous_pair = tree_bytes(bundle)
                saved_executable = root / 'saved-app'
                output.rename(saved_executable)
                output.mkdir()
                write_source(17 if cached else value)
                before_entries = set((root / 'build').rglob('metadata'))
                failed = build(False)
                expected = ('failed to restore cached executable' if cached else
                            'failed to publish executable')
                assert expected in failed.stderr, failed.stderr
                assert tree_bytes(bundle) == previous_pair, 'failed executable rename lost previous companion'
                assert set((root / 'build').rglob('metadata')) == before_entries
                assert output.is_dir() and not list(output.iterdir())
                output.rmdir()
                saved_executable.rename(output)
                run([output], 23 if cached else 17)
                assert uuid(output) == uuid(bundle)
            write_source(17)
            build()
            # A symbol-directory symlink is never an owned bundle.
            bundle.rename(saved)
            outside = root / 'unowned-symbols'
            outside.mkdir()
            (outside / 'keep').write_text('preserve')
            bundle.symlink_to(outside, target_is_directory=True)
            write_source(23)
            failed = build(False)
            assert 'non-directory or symlink' in failed.stderr, failed.stderr
            assert output.read_bytes() == original
            assert (outside / 'keep').read_text() == 'preserve'
            bundle.unlink()
            saved.rename(bundle)
            write_source(17)
            # A valid-looking metadata record cannot suppress a required
            # companion and advertise a successful incomplete cache hit.
            metadata = cache[0].read_text()
            assert re.search(r'^symbols [0-9a-f]+ [0-9]+$', metadata, re.M)
            cache[0].write_text(re.sub(r'^symbols .*$', 'symbols none', metadata, flags=re.M))
            shutil.rmtree(bundle)
            output.unlink()
            recovered = build()
            assert 'cache hit ' not in recovered.stderr, recovered.stderr
            assert 'companion presence' in recovered.stderr, recovered.stderr
            assert uuid(output) == uuid(bundle)
            run([output], 17)
        # Relocating the executable with its companion retains symbol lookup.
        relocated = root / 'relocated debug with spaces'
        relocated.mkdir()
        moved = relocated / output.name
        shutil.move(output, moved)
        if sys.platform == 'darwin':
            moved_bundle = Path(str(moved) + '.dSYM')
            shutil.move(bundle, moved_bundle)
            assert uuid(moved) == uuid(moved_bundle)
            assert 'main.gti' in run(['dwarfdump', '--debug-line', moved_bundle]).stdout
        else:
            assert 'main.gti' in run(['readelf', '--debug-dump=decodedline', moved]).stdout
        run([moved], 17)
        assert not list((root / 'build').rglob('*.gti-stage-*'))
        if args.backend == 'llvm':
            owner_source = root / 'owner.gti'
            owner_output = root / 'owner'
            owner_cases = [
                '#include <std/memory>\nint main() { auto value = '
                'std::make_unique<int>(17); return *value == 17 ? 0 : 1; }\n',
                (Path(__file__).parent / 'fixtures/llvm_memory_empty_unique.gti').read_text(),
                '''#include <std/memory>
mut int released = 0;
struct Value {
  int number;
  Value(int input) : number(input) {}
  ~Value() { released += 1; }
};
int main() {
  mut auto owner = std::make_unique<Value>(17);
  mut auto moved = std::move(owner);
  if (moved->number != 17) { return 1; }
  moved.reset(); moved.reset();
  return released == 1 ? 0 : 2;
}
''',
            ]
            for level in (0, 3):
                for text in owner_cases:
                    owner_source.write_text(text)
                    run([gti, owner_source, '--backend', 'llvm', '-g',
                         f'-O{level}', '--progress', 'never', '-o', owner_output])
                    run([owner_output])
                previous = owner_output.read_bytes()
                owner_bundle = Path(str(owner_output) + '.dSYM')
                previous_symbols = tree_bytes(owner_bundle) if sys.platform == 'darwin' else None
                owner_source.write_text('''#include <std/memory>
int main() {
  auto owner = std::make_unique<int>(17);
  auto moved = std::move(owner);
  return *owner;
}
''')
                failures = []
                for debug in (False, True):
                    argv = [gti, owner_source, '--backend', 'llvm', f'-O{level}',
                            '--progress', 'never', '-o', owner_output]
                    if debug:
                        argv.append('-g')
                    denied = run(argv, 65)
                    assert 'error[' in denied.stderr and 'GTI-B' not in denied.stderr
                    assert owner_output.read_bytes() == previous
                    if previous_symbols is not None:
                        assert tree_bytes(owner_bundle) == previous_symbols
                    failures.append(denied.stderr)
                assert failures[0] == failures[1], 'debug changed move rejection'
                owner_source.write_text('''#include <std/memory>
int main() { std::unique_ptr<int> owner{nullptr}; return *owner; }
''')
                run([gti, owner_source, '--backend', 'llvm', '-g', f'-O{level}',
                     '--progress', 'never', '-o', owner_output])
                failed = run([owner_output], 70)
                assert 'GTI-R0008' in failed.stderr and not failed.stdout
        print(f'{args.backend}: real debug object, source metadata, cold/warm/restoration, '
              'companion rejection/recovery passed; compiler sha256=' +
              hashlib.sha256(gti.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
