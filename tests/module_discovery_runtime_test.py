#!/usr/bin/env python3
"""Default discovery composes private implementations and invalidates caches."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

from project_cli_smoke_test import executable_named, run


def reject(arguments, cwd):
    result = subprocess.run(arguments, cwd=cwd, text=True, capture_output=True,
                            timeout=120)
    assert result.returncode != 0, result
    return result


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    source = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="gti-module-discovery-") as temporary:
        root = pathlib.Path(temporary) / "app"
        shutil.copytree(source / "examples/module-discovery", root,
                        ignore=shutil.ignore_patterns("build", "gti.lock"))
        command = [compiler, "build", "--backend", "llvm"]
        run(command, cwd=root)
        binary = executable_named(root / "build/gti/dev", "demo")
        run([str(binary)])
        cached = run(command + ["--verbose"], cwd=root)
        assert "gti: cache hit " in cached.stderr, cached.stderr

        # Unused invalid bodies are indexed, never analyzed or lowered.
        (root / "src/unused.gti").write_text('export module unused;\n"unterminated')
        run(command, cwd=root)
        run([str(binary)])

        body = root / "src/implementation/adjustment.gti"
        original = body.read_text()
        moved = root / "src/implementation/renamed.gti"
        body.rename(moved)
        rebuilt = run(command + ["--verbose"], cwd=root)
        assert "gti: cache hit " not in rebuilt.stderr, rebuilt.stderr
        run([str(binary)])
        moved.write_text(original.replace("return 2", "return 3"))
        run(command, cwd=root)
        assert subprocess.run([str(binary)], timeout=30).returncode == 1
        moved.unlink()
        failed = reject(command, cwd=root)
        assert "adjustment" in failed.stderr, failed.stderr
        moved.write_text(original)
        run(command, cwd=root)
        run([str(binary)])

        # Module discovery does not publish private declarations.
        entry = root / "src/main.gti"
        main_text = entry.read_text()
        entry.write_text(main_text.replace("demo::answer()", "demo::adjustment()"))
        failed = reject(command, cwd=root)
        assert "not exported" in failed.stderr, failed.stderr
        entry.write_text(main_text)

        # Package dependencies use the same discovered maps and private bodies.
        consumer = pathlib.Path(temporary) / "consumer"
        (consumer / "src").mkdir(parents=True)
        (consumer / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "consumer"\nversion = "0.1.0"\n'
            '[dependencies]\nlib = { path = "../app" }\n'
            '[targets.consumer]\nkind = "executable"\nroot = "src/main.gti"\n')
        (consumer / "src/main.gti").write_text(
            'import lib.demo.counter; int main() { return demo::answer() == 42 ? 0 : 1; }')
        run(command, cwd=consumer)
        run([str(executable_named(consumer / "build/gti/dev", "consumer"))])
        # Direct compilation still receives no project discovery map.
        direct = reject([compiler, str(entry), "--emit-cpp"], cwd=root)
        assert "demo.counter" in direct.stderr, direct.stderr
    print("PASS default module discovery, private bodies, dependencies and cache invalidation")


if __name__ == "__main__":
    main()
