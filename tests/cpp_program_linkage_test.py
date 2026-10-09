#!/usr/bin/env python3
"""Check private program linkage and unchanged native C symbol linkage."""

from pathlib import Path
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run([str(arg) for arg in command], text=True,
                            capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{command}\n{result.stdout}{result.stderr}")
    return result


def main():
    if len(sys.argv) != 5:
        raise SystemExit("usage: cpp_program_linkage_test.py <gti> <cxx> <root> <runtime>")
    compiler, cxx, repository, runtime = map(lambda arg: Path(arg).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="gti-private-linkage-") as temporary:
        root = Path(temporary)
        harness = root / "native.cpp"
        harness.write_text('''
extern "C" int native_identity(int value) { return value; }
int first_entry();
int second_entry();
int main() { return first_entry() == 7 && second_entry() == 11 ? 0 : 1; }
''')
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O3"):
                objects = []
                for name, value in (("first", 7), ("second", 11)):
                    # Both units deliberately contain the same ordinary GTI
                    # identities. Native C names, in contrast, share one host
                    # definition. Renaming only hosted main supplies two test
                    # entry points; it does not change GTI symbol identities.
                    source = root / f"{name}.gti"
                    source.write_text(f'''
extern "C" {{ int native_identity(int value); }}
mut int local_value = {value};
int identity(int value) {{ return value; }}
int main() {{ return native_identity(identity(local_value)); }}
''')
                    generated = root / f"{name}.cpp"
                    run([compiler, source, f"-{level}", "--std", standard,
                         "--emit-cpp", "-o", generated])
                    text = generated.read_text()
                    if text.count("int main() {") != 1:
                        raise RuntimeError("missing exact no-argument hosted adapter")
                    generated.write_text(text.replace("int main() {", f"int {name}_entry() {{"))
                    obj = root / f"{name}.o"
                    run([cxx, f"-std={standard}", f"-{level}",
                         "-D__gti_strict_ieee754=1",
                         "-I", repository / "runtime/include",
                         "-I", repository / "vendor/expected_lite/include",
                         "-c", generated, "-o", obj])
                    objects.append(obj)
                executable = root / f"linkage-{standard}-{level}"
                run([cxx, f"-std={standard}", f"-{level}", harness,
                     *objects, runtime, "-pthread", "-o", executable])
                result = run([executable])
                if result.stdout or result.stderr:
                    raise RuntimeError(f"unexpected output: {result.stdout}{result.stderr}")
    print("Private GTI identities and native C linkage pass O0/O3, C++20/C++23")


if __name__ == "__main__":
    main()
