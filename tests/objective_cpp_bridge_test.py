#!/usr/bin/env python3
"""Exercise the real macOS Objective-C++/C ABI boundary and its build identity."""

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from project_cli_smoke_test import DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT


def main():
    if sys.platform != "darwin":
        print("Objective-C++ execution requires macOS; target rejection is in project_model")
        return 77
    gti = str(pathlib.Path(sys.argv[1]).resolve())
    example = pathlib.Path(sys.argv[2]).resolve()
    env = os.environ.copy()
    for key in DEPENDENCY_INJECTING_NATIVE_ENVIRONMENT:
        env.pop(key, None)

    with tempfile.TemporaryDirectory(prefix="gti-objcxx-") as temporary:
        root = pathlib.Path(temporary) / "bridge with spaces"
        shutil.copytree(example, root, ignore=shutil.ignore_patterns("build", "gti.lock"))

        def run(*arguments, expected=0):
            result = subprocess.run([gti, *arguments], cwd=root, env=env,
                                    text=True, capture_output=True, check=False)
            assert result.returncode == expected, (arguments, result.returncode,
                                                    result.stdout, result.stderr)
            return result

        run("check")
        assert not (root / "build").exists(), "check must remain output-free"
        metadata = run("metadata")
        assert "foundation.mm" in metadata.stdout
        manifest = root / "gti.toml"
        original = manifest.read_text()

        def build():
            return run("build", "--verbose")

        first = build()
        assert "-x objective-c++" in first.stderr, first.stderr
        assert "gti: cache miss " in first.stderr, first.stderr
        cached = build()
        assert "gti: cache hit " in cached.stderr, cached.stderr
        assert not any(line.startswith("+ ") for line in cached.stderr.splitlines())
        executed = run("run", "--no-build")
        assert "psych/foundation\n" in executed.stdout, executed.stdout

        # No borrowed Objective-C storage is returned. Insufficient caller
        # capacity maps to the explicit failure result, not an out-of-bounds
        # write or a foreign exception crossing the C ABI.
        entry = root / "src/main.gti"
        entry_text = entry.read_text()
        entry.write_text(entry_text.replace("uint64_t(64)", "uint64_t(1)"))
        build()
        run("run", "--no-build", expected=1)
        entry.write_text(entry_text)
        assert "gti: cache hit " in build().stderr

        # A locally included header must enter the Objective-C++ dependency
        # closure, not a C++-only or guessed include scan.
        header = root / "native/bridge_config.h"
        header.write_text(header.read_text().replace('@"foundation"', '@"updated"'))
        changed = build()
        assert "gti: cache miss " in changed.stderr, changed.stderr
        assert "psych/updated\n" in run("run", "--no-build").stdout
        assert "gti: cache hit " in build().stderr

        # An ordinary C++ TU and generated GTI C++ remain C++ even when ARC is
        # passed through the existing shared C++ argument vector.
        (root / "native/plain.cpp").write_text(
            '#ifdef __OBJC__\n#error ordinary C++ changed language\n#endif\n'
            'int bridge_plain_cpp() { return 42; }\n')
        manifest.write_text(original.replace(
            'cpp-sources = ["native/foundation.mm"]',
            'cpp-sources = ["native/foundation.mm", "native/plain.cpp"]\n'
            'compile-args = ["-fobjc-arc"]'))
        arc = build()
        assert "gti: cache bypassed" in arc.stderr, arc.stderr
        assert "psych/updated\n" in run("run", "--no-build").stdout

        # Switching the existing profile C++ policy also reaches the bridge.
        manifest.write_text(manifest.read_text() + '\n[profiles.dev]\ncpp-standard = "c++20"\n')
        cpp20 = build()
        assert "-std=c++20" in cpp20.stderr, cpp20.stderr
        assert "psych/updated\n" in run("run", "--no-build").stdout

        # A library package contributes its own .mm source and framework through
        # the existing isolated native dependency group, without root flags.
        root_configuration = manifest.read_text()
        dependency = root / "apple"
        shutil.copytree(root / "native", dependency / "native")
        (dependency / "src").mkdir()
        (dependency / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "apple-bridge"\n'
            'version = "0.1.0"\n[package.native]\n'
            'cpp-sources = ["native/foundation.mm"]\nframeworks = ["Foundation"]\n')
        manifest.write_text(original.replace(
            '[package.native]\ncpp-sources = ["native/foundation.mm"]\n'
            'frameworks = ["Foundation"]',
            '[dependencies]\napple = { path = "apple" }'))
        composed = build()
        assert "/apple/native/foundation.mm" in composed.stderr
        assert "psych/updated\n" in run("run", "--no-build").stdout
        assert "gti: cache hit " in build().stderr
        manifest.write_text(root_configuration)
        build()

        # Both exception families are translated inside the bridge, never
        # propagated into the GTI call frame.
        source = root / "native/foundation.mm"
        native_text = source.read_text()
        source.write_text(native_text.replace(
            "@try {", '@try {\n@throw [NSException exceptionWithName:@"test" reason:nil userInfo:nil];'))
        build()
        run("run", "--no-build", expected=1)
        source.write_text(native_text.replace("  try {", "  try {\n    throw 42;"))
        build()
        run("run", "--no-build", expected=1)
        source.write_text(native_text)
        build()

        executable = next((root / "build/gti/dev").glob("*/objective-cpp-bridge"))
        objects = sorted((root / "build/gti/dev").glob("*/.gti-intermediate/*.o"))
        assert objects
        preserved = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in [executable, *objects]}
        source.write_text(source.read_text() + '\n#error deliberate bridge failure\n')
        failed = run("build", "--verbose", expected=1)
        assert "deliberate bridge failure" in failed.stderr
        for path, identity in preserved.items():
            assert hashlib.sha256(path.read_bytes()).hexdigest() == identity, path
        assert "psych/updated\n" in run("run", "--no-build").stdout
        print("Objective-C++ bridge, ARC/C++20, cache dependency, and failure preservation passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
