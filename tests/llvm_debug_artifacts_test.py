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
        assert not list((root / 'build').rglob('*.gti-stage-*'))
        print(f'{args.backend}: real debug object, source metadata, cold/warm/restoration, '
              'companion rejection/recovery passed; compiler sha256=' +
              hashlib.sha256(gti.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
