"""Exercise the shipped math library in a private copy of its resource tree."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def verify(stage):
    stage = stage.resolve()
    fixture = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="gti-installed-cmath-") as temporary:
        root = Path(temporary)
        stdlib = root / "stdlib"
        shutil.copytree(stage / "share/gti/stdlib", stdlib)
        bundles = root / "precompiled-stdlib"
        shutil.copytree(stage / "share/gti/precompiled-stdlib", bundles)
        # The identity envelope begins with the SHA-256 of the exact compiler
        # executable. Check the installed bytes against an independent hash.
        with (stage / "bin/gti").open("rb") as stream:
            compiler_digest = hashlib.file_digest(stream, "sha256").hexdigest().encode()
        for profile in ("dev", "release"):
            bundle, = bundles.glob(f"*/{profile}/cmath.gtilib")
            with bundle.open("rb") as stream:
                header = stream.read(20)
                if header not in {b"GTILIB\r\n" + struct.pack("<III", version, 1, len(compiler_digest))
                                  for version in (2, 3)}:
                    raise RuntimeError(f"invalid compiled standard library envelope in {bundle}")
                if stream.read(len(compiler_digest)) != compiler_digest:
                    raise RuntimeError(f"compiler SHA-256 in {bundle} differs from the staged executable")
                if bundle.parents[1].name == "arm64-apple-macos":
                    target = b"arm64-apple-macosx11.0.0"
                    if stream.read(4 + len(target)) != struct.pack("<I", len(target)) + target:
                        raise RuntimeError(f"{bundle} must target the macOS arm64 baseline across OS updates")
        (stdlib / "std/cmath.gti").unlink()
        env = dict(os.environ, GTI_STDLIB_PATH=str(stdlib),
                   GTI_RUNTIME_LIBRARY=str(stage / "lib/libgti_runtime.a"),
                   GTI_RUNTIME_INCLUDE=str(stage / "include"))
        project = root / "app"
        source = project / "src/main.gti"
        source.parent.mkdir(parents=True)
        shutil.copyfile(fixture / "gti.toml", project / "gti.toml")
        shutil.copyfile(fixture.parents[1] / "fixtures/stdlib_surface_cmath.gti", source)

        def run(*arguments, rejection=None, cwd=project):
            result = subprocess.run([str(stage / "bin/gti"), *arguments, "--offline"],
                                    cwd=cwd, env=env, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if rejection is None:
                if result.returncode:
                    raise RuntimeError(output)
            elif result.returncode == 0 or rejection not in output:
                raise RuntimeError(f"expected {rejection!r} from {arguments}\n{output}")

        for profile in ([], ["--release"]):
            run("check", *profile)
            run("run", *profile)
        # One executable can import its own compiled library and the installed
        # standard library in the same source graph.
        mixed = root / "mixed"
        (mixed / "src").mkdir(parents=True)
        (mixed / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "mixed"\nversion = "0.1.0"\n')
        (mixed / "src/lib.gti").write_text(
            "export module values;\nexport float radicand() { return 9.0; }\n")
        (mixed / "src/main.gti").write_text(
            "import values;\nimport std.cmath;\n"
            "int main() { return std::sqrt(radicand()) == 3.0 ? 0 : 1; }\n")
        run("run", "--release", cwd=mixed)
        if not list(mixed.glob("build/gti/release/*/mixed-lib.gtilib")):
            raise RuntimeError("mixed consumer did not build its inferred library")
        # The previous release run left a valid warm executable. A damaged
        # receipt must still fail before any cache hit can publish that output.
        bundle, = bundles.glob("*/release/cmath.gtilib")
        with bundle.open("r+b") as stream:
            stream.seek(-1, os.SEEK_END)
            checksum = stream.read(1)
            stream.seek(-1, os.SEEK_END)
            stream.write(b"0" if checksum != b"0" else b"1")
        for command in ("build", "check"):
            run(command, "--release", rejection="library bundle checksum mismatch")
        run("build", "--release", cwd=mixed, rejection="library bundle checksum mismatch")
        source.write_text("int main() { return 0; }\n")
        run("run", "--release")

        # A toolchain-owned module whose transitive source depends on a flag
        # must never be stamped as independent of the application's flags.
        shutil.copyfile(stage / "share/gti/stdlib/std/cmath.gti", stdlib / "std/cmath.gti")
        shutil.copyfile(fixture.parents[2] / "cmake/stdlib-bundles/gti.toml", root / "gti.toml")
        companion = stdlib / "std/detail/cmath_declarations.gti"
        with companion.open("a") as stream:
            stream.write("\n#ifdef NDEBUG\n#endif\n")
        run("build", "cmath", cwd=root,
            rejection="flag-independent source closure")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    verify(parser.parse_args().stage)
