"""Exercise native, MIR, and generic libraries in a private copy of the installed toolchain resources."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib


def verify(stage):
    stage = stage.resolve()
    fixture = Path(__file__).resolve().parent
    manifest = fixture.parents[2] / "cmake/stdlib-bundles/gti.toml"
    with manifest.open("rb") as stream:
        targets = tomllib.load(stream)["targets"]
    entries = [Path(t["root"]).relative_to("stdlib") for t in targets.values()]
    with tempfile.TemporaryDirectory(prefix="gti-installed-stdlib-") as temporary:
        root = Path(temporary)
        stdlib = root / "stdlib"
        shutil.copytree(stage / "share/gti/stdlib", stdlib)
        roots = {p.relative_to(stdlib) for p in (stdlib / "std").rglob("*.gti")}
        if set(entries) != roots - {Path("std/assert.gti")} or Path("std/assert.gti") not in roots:
            raise RuntimeError("the installed catalog must cover all std modules except the source-based std.assert")
        bundles = root / "precompiled-stdlib"
        shutil.copytree(stage / "share/gti/precompiled-stdlib", bundles)
        # The identity envelope begins with the SHA-256 of the exact compiler
        # executable. Check the installed bytes against an independent hash.
        with (stage / "bin/gti").open("rb") as stream:
            compiler_digest = hashlib.file_digest(stream, "sha256").hexdigest().encode()
        for profile in ("dev", "release"):
            for name in targets:
                bundle, = bundles.glob(f"*/{profile}/{name}.gtilib")
                with bundle.open("rb") as stream:
                    header = stream.read(20)
                    versions = [(2, 2)] if not targets[name].get("functions") else [(2, 1), (3, 1)]
                    if header not in {b"GTILIB\r\n" + struct.pack("<III", version, envelope, len(compiler_digest))
                                      for version, envelope in versions}:
                        raise RuntimeError(f"invalid compiled standard library envelope in {bundle}")
                    if stream.read(len(compiler_digest)) != compiler_digest:
                        raise RuntimeError(f"compiler SHA-256 in {bundle} differs from the staged executable")
                    if bundle.parents[1].name == "arm64-apple-macos":
                        target = b"arm64-apple-macosx11.0.0"
                        if stream.read(4 + len(target)) != struct.pack("<I", len(target)) + target:
                            raise RuntimeError(f"{bundle} must target the macOS arm64 baseline across OS updates")
        env = dict(os.environ, GTI_STDLIB_PATH=str(stdlib),
                   GTI_RUNTIME_LIBRARY=str(stage / "lib/libgti_runtime.a"),
                   GTI_RUNTIME_INCLUDE=str(stage / "include"))
        project = root / "app"
        source = project / "src/main.gti"
        source.parent.mkdir(parents=True)
        shutil.copyfile(fixture / "gti.toml", project / "gti.toml")

        def run(*arguments, rejection=None, cwd=project, environment=env):
            result = subprocess.run([str(stage / "bin/gti"), *arguments, "--offline"],
                                    cwd=cwd, env=environment, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if rejection is None:
                if result.returncode:
                    raise RuntimeError(output)
            elif result.returncode == 0 or rejection not in output:
                raise RuntimeError(f"expected {rejection!r} from {arguments}\n{output}")

        # Keep all logical and physical paths fixed, cold-build both ways, and
        # exercise an input-dependent destructor in the generic vector body.
        shutil.copyfile(fixture.parents[1] / "ui/stdlib/vector/construction_only/element_failure.gti", source)

        def cleanup_program(profile):
            shutil.rmtree(project / "build", ignore_errors=True)
            run("build", *(["--release"] if profile == "release" else []))
            executable, = (project / "build/gti" / profile).glob("*/app")
            observations = []
            for args, status in (([], 0), (["fail"], 70), (["fail", "fail"], 70)):
                result = subprocess.run([executable, *args], cwd=project, capture_output=True)
                if result.returncode != status:
                    raise RuntimeError(f"cleanup program {args} exited {result.returncode}, expected {status}")
                observations.append((result.stdout, result.stderr))
            return executable.read_bytes(), observations

        hidden = bundles.rename(root / "saved-bundles")
        source_builds = {profile: cleanup_program(profile) for profile in ("dev", "release")}
        for entry in entries:
            (stdlib / entry).unlink()
        hidden.rename(bundles)
        for profile, (executable, observations) in source_builds.items():
            packaged, output = cleanup_program(profile)
            if executable != packaged:
                raise RuntimeError(f"{profile}: vector recipe changed the executable bytes")
            if observations != output:
                raise RuntimeError(f"{profile}: vector recipe changed cleanup output or diagnostics")

        # The public imports reach the private module implementations through
        # their normal dependencies. Only std.assert still has a source body.
        imports = [f"import {'.'.join(entry.with_suffix('').parts)};"
                   for entry in sorted(roots) if entry.parts[1] != "detail"]
        source.write_text("\n".join(imports) + """
int main() {
  mut int evaluated = 0;
  std::debug_assert!(++evaluated == 1);
#ifdef NDEBUG
  return evaluated;
#else
  return evaluated == 1 ? 0 : 1;
#endif
}
""")
        for profile in ([], ["--release"]):
            run("run", *profile)

        shutil.copyfile(fixture.parents[1] / "fixtures/stdlib_surface_cmath.gti", source)
        for profile in ([], ["--release"]):
            run("check", *profile)
            run("run", *profile)
        # The producer was staged before this application's type existed.
        shutil.copyfile(fixture / "array_consumer.gti", source)
        for profile in ([], ["--release"]):
            run("check", *profile)
            run("run", *profile)
        for case in ("slow/stdlib/vector/constexpr_range_for_views.gti",
                     "ui/callables/borrowing_captures/borrowing_captures_23.gti"):
            shutil.copyfile(fixture.parents[1] / case, source)
            for profile in ([], ["--release"]):
                run("run", *profile)
        array_bundle, = bundles.glob("*/release/array.gtilib")
        array_receipt = array_bundle.with_suffix(".gtilib.sha256")
        digest = hashlib.sha256(array_bundle.read_bytes()).hexdigest()
        if array_receipt.read_text() != digest + "\n":
            raise RuntimeError("installed array receipt does not identify its artifact")
        # Select a second nominal type and extent from the *finished* artifact:
        # the producer cannot enumerate this application in advance.
        element = "ApplicationPacket_" + digest[:12]
        extent = 11 + int(digest[:2], 16) % 17
        values = ", ".join(f"{element}({i})" for i in range(extent))
        source.write_text(
            "import std.array;\n"
            f"struct {element} {{ uint64_t serial; {element}(uint64_t n) : serial(n) {{}} }};\n"
            f"int main() {{ auto packets = std::array<{element}, {extent}>({{{values}}});\n"
            f"return packets.size() == {extent} && packets.back().serial == {extent-1} ? 0 : 1; }}\n")
        for profile in ([], ["--release"]):
            run("run", *profile)
        # Exercise different substitutions of the same independently built bytes,
        # early evaluation, a private helper, and the empty array.
        array_cases = fixture.parents[1] / "ui/stdlib/array/comparison"
        for example in ("constexpr_ordering.gti", "ordering.gti", "readonly_elements.gti"):
            shutil.copyfile(array_cases / example, source)
            run("run", "--release")
        shutil.copyfile(array_cases / "equality_only.gti", source)
        run("check", rejection="GTI-S2029")
        source.write_text("import std.array;\nstd::array<int, 7>::iterator dangling() { "
                          "mut std::array<int, 7> x{}; return x.begin_mut(); }\n"
                          "int main() { return 0; }\n")
        run("check", rejection="GTI-S2045")
        if hashlib.sha256(array_bundle.read_bytes()).hexdigest() != digest:
            raise RuntimeError("instantiating consumer types modified the installed library")
        # Reaching the same module from a warm executable must still validate
        # the installation receipt before restoring executable cache entries.
        shutil.copyfile(fixture / "array_consumer.gti", source)
        run("run", "--release")
        array_receipt.write_text("0" * 64 + "\n")
        run("build", "--release", rejection="installed generic library receipt mismatch")
        array_receipt.write_text(digest + "\n")
        # A same-length edit changes the body recipe without changing any
        # declaration or source extent. Install that real producer output and
        # rebuild the unchanged application with its existing cache.
        producer = root / "array-producer"
        shutil.copytree(stage / "share/gti/stdlib", producer / "stdlib")
        shutil.copyfile(manifest, producer / "gti.toml")
        impl = producer / "stdlib/std/array.gti"
        implementation = impl.read_text()
        old = "constexpr size_t size() {\n    return N;"
        if implementation.count(old) != 1:
            raise RuntimeError("missing array size method in producer fixture")
        impl.write_text(implementation.replace(old, old.replace("return N;", "return 1;")))
        run("build", "array", "--release", cwd=producer,
            environment=dict(env, GTI_STDLIB_PATH=str(producer / "stdlib")))
        alternate, = (producer / "build/gti/release").glob("*/array.gtilib")
        original = array_bundle.read_bytes()
        array_bundle.write_bytes(alternate.read_bytes())
        array_receipt.write_text(hashlib.sha256(array_bundle.read_bytes()).hexdigest() + "\n")
        run("build", "--release")
        executable = project / "build/gti/release" / array_bundle.parents[1].name / "app"
        if subprocess.run([str(executable)]).returncode != 1:
            raise RuntimeError("unchanged application retained the old array size method")
        array_bundle.write_bytes(original)
        array_receipt.write_text(digest + "\n")
        run("run", "--release")

        # Check the reference sequences, independent state and entropy-device
        # API through the public module with the engine implementation absent.
        shutil.copyfile(fixture.parents[1] / "ui/stdlib/random/engine.gti", source)
        for profile in ([], ["--release"]):
            run("run", *profile)

        # These reference vectors exercise the public draw loop. Its body must
        # be visible after packaging; only seed initialization needs a native call.
        random_bundle, = bundles.glob("*/release/random.gtilib")
        executable, = project.glob("build/gti/release/*/app")
        subprocess.run([sys.executable, fixture.parent / "reusable_library_mir/check_imports.py",
                        random_bundle, executable], check=True)

        # One executable can import its own compiled library and the installed
        # standard library in the same source graph. Building its inferred
        # project library uses the source prelude, which imports std.meta.
        shutil.copyfile(stage / "share/gti/stdlib/std/meta.gti", stdlib / "std/meta.gti")
        mixed = root / "mixed"
        (mixed / "src").mkdir(parents=True)
        (mixed / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "mixed"\nversion = "0.1.0"\n')
        (mixed / "src/lib.gti").write_text(
            "export module values;\nexport float radicand() { return 9.0; }\n")
        (mixed / "src/main.gti").write_text(
            "import values;\nimport std.cmath;\nimport std.random;\n"
            "int main() { mut std::random_engine r{uint64_t(0)}; "
            "return std::sqrt(radicand()) == 3.0 && r() == uint64_t(0x53175d61490b23df) ? 0 : 1; }\n")
        run("run", "--release", cwd=mixed)
        if not list(mixed.glob("build/gti/release/*/mixed-lib.gtilib")):
            raise RuntimeError("mixed consumer did not build its inferred library")
        # Replace just the random bundle with a second, valid producer output.
        # The warm executable must observe each change, to native seed or MIR draw, in its second
        # dependency even though its source and math bundle are unchanged.
        producer = root / "producer"
        producer.mkdir()
        producer_stdlib = producer / "stdlib"
        shutil.copytree(stage / "share/gti/stdlib", producer_stdlib)
        shutil.copyfile(manifest, producer / "gti.toml")
        implementation = producer_stdlib / "std/detail/random_engine.gti"
        producer_source = implementation.read_text()
        producer_env = dict(env, GTI_STDLIB_PATH=str(producer_stdlib))
        destination, = bundles.glob("*/release/random.gtilib")
        original = destination.read_bytes()
        executable = mixed / "build/gti/release" / destination.parents[1].name / "mixed"
        for old, new in (("return state;", "state.s0 = uint64_t(0); return state;"),
                         ("return result;", "return result ^ uint64_t(1);")):
            assert producer_source.count(old) == 1
            implementation.write_text(producer_source.replace(old, new))
            run("build", "random", "--release", cwd=producer, environment=producer_env)
            alternate, = (producer / "build/gti/release").glob("*/random.gtilib")
            shutil.copyfile(alternate, destination)
            run("build", "--release", cwd=mixed)
            if subprocess.run([executable], cwd=mixed).returncode != 1:
                raise RuntimeError("warm executable did not use the replacement random bundle")
            destination.write_bytes(original)
            run("run", "--release", cwd=mixed)

        # The previous release run left a valid warm executable. A damaged
        # receipt must still fail before any cache hit can publish that output.
        # The random-only consumer is independent of a corrupt math module.
        random_source = source.read_text()
        shutil.copyfile(fixture.parents[1] / "fixtures/stdlib_surface_cmath.gti", source)
        run("run", "--release")
        bundle, = bundles.glob("*/release/cmath.gtilib")
        with bundle.open("r+b") as stream:
            stream.seek(-1, os.SEEK_END)
            checksum = stream.read(1)
            stream.seek(-1, os.SEEK_END)
            stream.write(b"0" if checksum != b"0" else b"1")
        for command in ("build", "check"):
            run(command, "--release", rejection="library bundle checksum mismatch")
        run("build", "--release", cwd=mixed, rejection="library bundle checksum mismatch")
        source.write_text(random_source)
        run("run", "--release")
        random_bundle, = bundles.glob("*/release/random.gtilib")
        random_bundle.unlink()
        run("build", "--release", rejection="compiled library input must be a regular file")
        source.write_text("int main() { return 0; }\n")
        run("run", "--release")

        # A toolchain-owned module whose transitive source depends on a flag
        # must never be stamped as independent of the application's flags.
        for implementation in entries:
            shutil.copyfile(stage / "share/gti/stdlib" / implementation, stdlib / implementation)
        shutil.copyfile(manifest, root / "gti.toml")
        companion = stdlib / "std/detail/cmath_declarations.gti"
        with companion.open("a") as stream:
            stream.write("\n#ifdef NDEBUG\n#endif\n")
        run("build", "cmath", cwd=root,
            rejection="flag-independent source closure")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    verify(parser.parse_args().stage)
