#!/usr/bin/env python3
"""Default discovery composes private implementations and invalidates caches."""
import json
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

        # Interface paths, names and additions also participate in identity.
        api = root / "src/public_api.gti"
        relocated_api = root / "src/renamed_api.gti"
        api.rename(relocated_api)
        rebuilt = run(command + ["--verbose"], cwd=root)
        assert "gti: cache hit " not in rebuilt.stderr, rebuilt.stderr
        run([str(binary)])
        duplicate = root / "src/duplicate.gti"
        duplicate.write_text(relocated_api.read_text())
        failed = reject(command, cwd=root)
        assert "GTI-B1005" in failed.stderr and "other module interface" in failed.stderr, failed.stderr
        duplicate.unlink()
        relocated_api.rename(api)

        named_sources = [api, moved, root / "src/implementation/answer.gti",
                         root / "src/main.gti"]
        saved_sources = [(path, path.read_text()) for path in named_sources]
        for path, text in saved_sources:
            path.write_text(text.replace("demo.counter", "demo.renamed"))
        renamed = run(command + ["--verbose"], cwd=root)
        assert "gti: cache hit " not in renamed.stderr, renamed.stderr
        run([str(binary)])
        for path, text in saved_sources:
            path.write_text(text)

        # Unused cyclic modules are indexed without loading their bodies;
        # importing that cycle still receives the ordinary frontend error.
        (root / "src/cycle_a.gti").write_text("export module cycle_a; import cycle_b;")
        (root / "src/cycle_b.gti").write_text("export module cycle_b; import cycle_a;")
        run(command, cwd=root)
        entry = root / "src/main.gti"
        main_text = entry.read_text()
        entry.write_text("import cycle_a;\n" + main_text)
        failed = reject(command, cwd=root)
        assert "Module import cycle" in failed.stderr, failed.stderr
        entry.write_text(main_text)
        (root / "src/cycle_a.gti").unlink()
        (root / "src/cycle_b.gti").unlink()

        # Explicit selection can exclude a platform alternative with the same
        # private definition, without manually registering the interface.
        alternative = root / "src/implementation/other_platform.gti"
        alternative.write_text(original.replace("return 2", "return 3"))
        manifest = root / "gti.toml"
        manifest_text = manifest.read_text()
        manifest.write_text(manifest_text + '\n[module-implementations]\n'
                            '"demo.counter" = ["src/implementation/answer.gti", '
                            '"src/implementation/renamed.gti"]\n')
        run(command, cwd=root)
        run([str(binary)])
        manifest.write_text(manifest_text + '\n[module-discovery]\n'
                            'exclude = ["src/implementation/other_platform.gti"]\n')
        run(command, cwd=root)
        run([str(binary)])
        alternative.unlink()
        manifest.write_text(manifest_text)

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

        # A discovered main module is the dependency's short import. Re-exports
        # publish declarations, never the transitive package's alias authority.
        facade = pathlib.Path(temporary) / "facade"
        (facade / "src").mkdir(parents=True)
        (facade / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "facade"\nversion = "0.1.0"\n'
            '[dependencies]\nlib = { path = "../app" }\n')
        (facade / "src/public.gti").write_text(
            'export module main; export import lib.demo.counter;')
        consumer_manifest = consumer / "gti.toml"
        consumer_manifest.write_text(consumer_manifest.read_text().replace(
            'lib = { path = "../app" }', 'facade = { path = "../facade" }'))
        consumer_entry = consumer / "src/main.gti"
        consumer_source = consumer_entry.read_text().replace("lib.demo.counter", "facade")
        consumer_entry.write_text(consumer_source)
        run(command, cwd=consumer)
        run([str(executable_named(consumer / "build/gti/dev", "consumer"))])
        consumer_entry.write_text("import lib.demo.counter;\n" + consumer_source)
        failed = reject(command, cwd=consumer)
        assert "lib.demo.counter" in failed.stderr, failed.stderr
        consumer_entry.write_text(consumer_source)

        # The same package maps compose when selected through a workspace.
        workspace = pathlib.Path(temporary)
        (workspace / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "workspace_root"\nversion = "0.1.0"\n'
            '[workspace]\nmembers = ["app", "consumer", "facade"]\n')
        run(command + ["--package", "consumer"], cwd=workspace)
        run([str(executable_named(workspace / "build/gti/packages/consumer/dev", "consumer"))])
        metadata = json.loads(run([compiler, "metadata", "--package", "consumer"], cwd=workspace).stdout)
        assert metadata["workspace"]["declared"] is True, metadata
        assert metadata["workspace"]["selectedPackage"] == "consumer", metadata
        run(command + ["--release"], cwd=root)
        run([str(executable_named(workspace / "build/gti/packages/module-discovery/release", "demo"))])
        # Direct compilation still receives no project discovery map.
        direct = reject([compiler, str(entry), "--emit-cpp"], cwd=root)
        assert "demo.counter" in direct.stderr, direct.stderr
    print("PASS default module discovery, private bodies, dependencies and cache invalidation")


if __name__ == "__main__":
    main()
