#!/usr/bin/env python3
"""Split definitions retain MIR execution, module privacy and cache identity."""

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

from project_cli_smoke_test import executable_named, run as checked_run


def run(*args, **kwargs):
    try:
        return checked_run(*args, **kwargs)
    except AssertionError as error:
        details = []
        prefix = "gti: backend diagnostics saved at "
        for line in str(error).splitlines():
            if line.startswith(prefix):
                log = pathlib.Path(line[len(prefix):])
                if log.is_file():
                    details.append(log.read_text())
        raise AssertionError(str(error) + "\n" + "\n".join(details)) from error


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    repository = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-split-definitions-") as temporary:
        root = pathlib.Path(temporary)
        source = repository / "examples/150-out-of-class-methods.gti"
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O1", "-O3"):
                binary = root / f"direct-{standard}-{optimization}"
                run([compiler, str(source), "--std", standard, optimization,
                     "--keep-cpp", "-o", str(binary)])
                assert run([str(binary)]).stdout == "separate definitions\n"
                assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()

        project = root / "factory"
        shutil.copytree(repository / "examples/window-factory", project,
                        ignore=shutil.ignore_patterns("build", "gti.lock"))
        report = root / "report.json"
        run([compiler, "build", "--build-report", str(report)], cwd=project)
        binary = executable_named(project / "build/gti/dev", "window-factory")
        run([str(binary)])
        assert "window_factory.gti" in report.read_text(), json.loads(report.read_text())
        run([compiler, "build"], cwd=project)
        assert "gti: cache hit " in run([compiler, "build", "--verbose"], cwd=project).stderr

        main_path = project / "src/main.gti"
        before = main_path.read_text()
        main_path.write_text(before.replace("auto desktop = Window::Create();", "DesktopWindow desktop{};"))
        private = subprocess.run([compiler, "check"], cwd=project, capture_output=True, text=True)
        assert private.returncode != 0 and "DesktopWindow" in private.stderr, private.stderr
        main_path.write_text(before)

        implementation = project / "src/window_factory.gti"
        content = implementation.read_text()
        implementation.write_text(content.replace("backend == window_backend::headless", "backend != window_backend::headless"))
        changed = run([compiler, "build", "--verbose"], cwd=project)
        assert "gti: cache hit " not in changed.stderr, changed.stderr
        assert subprocess.run([str(binary)]).returncode == 1
        implementation.write_text(content)
        run([compiler, "build"], cwd=project)
        run([str(binary)])

        manifest_path = project / "gti.toml"
        manifest = manifest_path.read_text()
        registration = 'window = ["src/window_factory.gti"]'
        for replacement, code in (
            ('unknown = ["src/window_factory.gti"]', "GTI-B1005"),
            ('window = "src/window_factory.gti"', "GTI-B1004"),
            ('window = [5]', "GTI-B1004"),
            ('window = ["src/missing.gti"]', "GTI-B1103"),
            ('window = ["src/window.gti"]', "GTI-B1005"),
            ('window = ["src/window_factory.gti", "src/window_factory.gti"]', "GTI-B1005"),
            ('window = ["../outside.gti"]', "GTI-B1104"),
            ('window = ["src/escape.gti"]', "GTI-B1104"),
        ):
            (root / "outside.gti").write_text("int hidden() { return 0; }\n")
            escape = project / "src/escape.gti"
            if not escape.exists():
                escape.symlink_to(root / "outside.gti")
            manifest_path.write_text(manifest.replace(registration, replacement))
            failed = subprocess.run([compiler, "check"], cwd=project, capture_output=True, text=True)
            assert failed.returncode != 0 and code in failed.stderr, failed.stderr
        manifest_path.write_text(manifest.replace(registration, "window = []"))
        missing = subprocess.run([compiler, "build"], cwd=project, capture_output=True, text=True)
        assert missing.returncode != 0 and "GTI-S2106" in missing.stderr, missing.stderr
        manifest_path.write_text(manifest)

        # Unreached module bodies are not scanned or compiled.
        (project / "src/unused.gti").write_text("export module unused;\n")
        (project / "src/broken.gti").write_text("not valid gti source\n")
        unused = manifest.replace('[modules]', '[modules]\nunused = "src/unused.gti"').replace(
            '[module-implementations]', '[module-implementations]\nunused = ["src/broken.gti"]')
        manifest_path.write_text(unused)
        run([compiler, "build"], cwd=project)
        manifest_path.write_text(manifest)

        # A dependency's registered bodies are loaded without exposing their
        # dependencies or requiring consumers to include implementation files.
        app = root / "app"
        app.mkdir()
        (app / "main.gti").write_text(before.replace("import window;", "import factory.window;"))
        (app / "gti.toml").write_text('manifest-version = 1\n[package]\nname = "consumer"\n'
            'version = "0.1.0"\n[dependencies]\nfactory = { path = "../factory" }\n'
            '[targets.consumer]\nkind = "executable"\nroot = "main.gti"\n')
        run([compiler, "build"], cwd=app)
        run([str(executable_named(app / "build/gti/dev", "consumer"))])
        (app / "main.gti").write_text('import factory.window; import std.memory;\n'
            'std::unique_ptr<Window> Window::Create(window_backend backend) { return std::unique_ptr<Window>(); }\n'
            'int main() { return 0; }\n')
        foreign = subprocess.run([compiler, "check"], cwd=app, capture_output=True, text=True)
        assert foreign.returncode != 0 and "GTI-S2120" in foreign.stderr, foreign.stderr


if __name__ == "__main__":
    main()
