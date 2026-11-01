"""Owned inline invocation, allocation/failure oracles, and source rejection."""

from pathlib import Path
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, text=True, capture_output=True, timeout=120)


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-owned-inline-") as temporary:
        work = Path(temporary)
        for opt in ("O0", "O3"):
            for name in ("owned_inline_consumption", "owned_inline_callbacks"):
                binary = work / f"{name}-{opt}"
                command = [str(compiler), str(fixtures / f"{name}.gti"),
                           "--backend", "llvm", f"-{opt}", "-o", str(binary)]
                if name == "owned_inline_callbacks":
                    command += ["--", str(fixtures / "owned_inline_allocations.cpp")]
                compiled = run(command)
                assert compiled.returncode == 0, (command, compiled.stdout, compiled.stderr)
                result = run([str(binary)])
                expected = "drop 84\n" if name == "owned_inline_callbacks" else ""
                assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), result
                if name == "owned_inline_callbacks":
                    for arguments in (["fail"], ["later", "fail"]):
                        failed = run([str(binary), *arguments])
                        assert failed.returncode == 70 and failed.stdout == "drop 84\n", failed
                        assert "GTI-R0002" in failed.stderr and "division_by_zero" in failed.stderr, failed

            empty = work / "empty.gti"
            empty.write_text("import std.functional;\nint main() {\n"
                             "  std::move_only_function<int() &&> callback{};\n"
                             "  return std::move(callback)();\n}\n")
            binary = work / f"empty-{opt}"
            compiled = run([str(compiler), str(empty), "--backend", "llvm",
                            f"-{opt}", "-o", str(binary)])
            assert compiled.returncode == 0, compiled
            failed = run([str(binary)])
            assert failed.returncode == 70 and "empty_owner" in failed.stderr, failed

        source = (fixtures / "owned_inline_consumption.gti").read_text()
        cases = {
            "second_invocation": source.replace(
                "return result == 42 ? 0 : 2;",
                "return std::move(operation).invoke_once<int>(5);"),
            "borrowed_receiver": source.replace(
                "int result = std::move(operation).invoke_once<int>(5);",
                "mut command& borrowed = operation.get();\n"
                "  int result = std::move(borrowed)(5);"),
            "live_alias": source.replace(
                "int main() {", "void observe(command& value) {}\nint main() {").replace(
                "int result = std::move(operation).invoke_once<int>(5);",
                "command& borrowed = operation.get();\n"
                "  int result = std::move(operation).invoke_once<int>(5);\n"
                "  observe(borrowed);"),
            "named_borrow_escape": source.replace(
                "int redeem(int bonus) && { return this.value + bonus; }",
                "int& view() { return this.value; }").replace(
                "std::move(this.pending).redeem(5)",
                "std::move(this.pending).view()"),
            "wrong_result": source.replace(
                "int result = std::move(operation).invoke_once<int>(5);",
                "bool result = std::move(operation).invoke_once<bool>(5);").replace(
                "return result == 42 ? 0 : 2;", "return result ? 0 : 2;"),
        }
        for name, text in cases.items():
            fixture = work / f"{name}.gti"
            fixture.write_text(text)
            result = run([str(compiler), str(fixture), "--emit-mir", "-o", str(work / f"{name}.mir")])
            code = {"wrong_result": "GTI-S2046", "named_borrow_escape": "GTI-S2022",
                    "live_alias": "GTI-S2017"}.get(name, "GTI-S2018")
            assert result.returncode != 0 and f"error[{code}]" in result.stderr, (name, result)
            if name == "wrong_result":
                assert "Owned callable projection must return 'bool'" in result.stderr, result
    print("owned inline invocation passes LLVM O0/O3 allocation, cleanup, and rejection checks")


if __name__ == "__main__":
    main()
