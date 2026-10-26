#!/usr/bin/env python3
"""Test-only reviewed raw-lane extension guards/codegen, not GTI wait admission."""

import argparse
from collections import Counter
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile


def run(argv: list[str], *, failure: str | None = None) -> str:
    result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    if failure is not None:
        if result.returncode == 0 or failure not in result.stderr:
            raise AssertionError(f"expected {failure}: {argv}\n{result.stderr[-2500:]}")
    elif result.returncode:
        raise AssertionError(f"{argv}: exit {result.returncode}\n"
                             f"{result.stdout[-2000:]}{result.stderr[-2500:]}")
    return result.stdout


def macros(compiler: str) -> str:
    result = subprocess.run([compiler, "-dM", "-E", "-x", "c++", "-"],
                            input="", capture_output=True, text=True, timeout=30,
                            check=True)
    return result.stdout


def check_clang(compiler: str, source: Path, work: Path, label: str) -> None:
    ir, obj = work / f"{label}.ll", work / f"{label}.o"
    flags = [compiler, "-std=c++20", "-O3", "-fno-strict-aliasing"]
    run([*flags, "-S", "-emit-llvm", str(source), "-o", str(ir)])
    run([*flags, "-c", str(source), "-o", str(obj)])
    module = ir.read_text()
    for bits in (8, 16, 32, 64):
        for name, order in (("relaxed", "monotonic"), ("acquire", "acquire"),
                            ("sequential", "seq_cst")):
            symbol = f"gti_test_lane_load{bits}_{name}"
            match = re.search(r"define[^\n]*@" + symbol +
                              r"\([^\n]*\)[^\n]*\{(.*?)^\}", module,
                              re.MULTILINE | re.DOTALL)
            if match is None:
                raise AssertionError(f"missing exact lane function {symbol}")
            body = match.group(1)
            loads = re.findall(r"\bload atomic i(\d+), ptr [^\n]*?"
                               r"\b(monotonic|acquire|seq_cst)\b[^\n]*?"
                               r"align (\d+)", body)
            if (loads != [(str(bits), order, str(bits // 8))] or
                    re.search(r"\bload (?!atomic\b)", body) or
                    "syncscope(" in body or "!tbaa" in body or
                    "__atomic_" in body):
                raise AssertionError(f"lane contract differs in {symbol}: {body}")
            if bits < 64 and not re.search(r"zext i" + str(bits) + r" .* to i64", body):
                raise AssertionError(f"lane bits not zero-extended in {symbol}")
    undefined = run(["nm", "-u", str(obj)])
    if "__atomic_" in undefined or "libatomic" in undefined:
        raise AssertionError(f"native atomic fallback: {undefined}")
    print(f"{label}: 12 exact atomic width/order/alignment loads; "
          "System scope, no ordinary lane loads/TBAA/libatomic", flush=True)


def check_gcc(compiler: str, source: Path, work: Path) -> None:
    dump, obj, assembly = work / "gcc.optimized", work / "gcc.o", work / "gcc.s"
    flags = [compiler, "-std=c++20", "-O3", "-fno-strict-aliasing"]
    run([*flags, f"-fdump-tree-optimized={dump}", "-c", str(source), "-o", str(obj)])
    expected = Counter((str(width), str(order))
                       for width in (1, 2, 4, 8) for order in (0, 2, 5))
    observed = Counter(re.findall(r"__atomic_load_([1248])\s*\([^,\n]+,\s*([025])\)",
                                  dump.read_text()))
    if observed != expected:
        raise AssertionError(f"GCC atomic width/order plan differs: {observed}")
    run([*flags, "-S", "-masm=intel", str(source), "-o", str(assembly)])
    machine = assembly.read_text()
    for bits, width in ((8, "BYTE"), (16, "WORD"), (32, "DWORD"), (64, "QWORD")):
        for order in ("relaxed", "acquire", "sequential"):
            symbol = f"gti_test_lane_load{bits}_{order}"
            match = re.search(r"^" + symbol + r":\n(.*?)^\s*\.size\s+" + symbol + r",",
                              machine, re.MULTILINE | re.DOTALL)
            if match is None:
                raise AssertionError(f"missing native GCC function {symbol}")
            memory = re.findall(r"\b(BYTE|WORD|DWORD|QWORD) PTR \[", match.group(1))
            if memory != [width]:
                raise AssertionError(f"GCC native lane width differs: {symbol}: {memory}")
    undefined = run(["nm", "-u", str(obj)])
    if "__atomic_" in undefined or "libatomic" in undefined:
        raise AssertionError(f"GCC native atomic fallback: {undefined}")
    print("native-gcc: 12 exact builtin width/order plans and native memory widths; "
          "no libatomic", flush=True)


def check_rejections(compiler: str, llvm: str, fixture: Path, work: Path) -> None:
    profile = fixture / "llvm_atomic_wait_lane_profile.h"
    for target in ("x86_64-apple-darwin", "aarch64-unknown-linux-gnu",
                   "x86_64-pc-windows-msvc"):
        run([llvm, f"--target={target}", "-E", "-x", "c++", str(profile)],
            failure="GTI_TEST_WAIT_LANE_UNREVIEWED_TARGET")
    incompatible = ("x86_64-unknown-linux-gnu" if sys.platform == "darwin"
                    else "aarch64-apple-darwin")
    run([llvm, f"--target={incompatible}", "-E", "-x", "c++", str(profile)],
        failure="GTI_TEST_WAIT_LANE_UNREVIEWED_COMPILER")
    run([llvm, "--target=x86_64-unknown-linux-gnux32", "-E", "-x", "c++", str(profile)],
        failure="GTI_TEST_WAIT_LANE_UNREVIEWED_ABI")
    for index, domain in enumerate(("bool", "std::int32_t", "float", "__uint128_t")):
        source = work / f"wrong-domain-{index}.cpp"
        source.write_text('#include "llvm_atomic_wait_lane_contract.h"\n'
                          f'auto wrong(const void *p) {{ return gti_test_wait_lane::load<{domain}>(p, 0); }}\n')
        run([compiler, "-std=c++20", "-O3", "-fno-strict-aliasing",
             "-I", str(fixture), "-fsyntax-only", str(source)],
            failure="GTI_TEST_WAIT_LANE_UNSIGNED_DOMAIN")
    driver = work / "guard-controls.cpp"
    driver.write_text(r'''#include "llvm_atomic_wait_lane_contract.h"
#include <array>
int main(int argc, char **argv) {
  alignas(8) std::uint64_t value = 7;
  if (argc == 1) {
    for (auto order : std::array<std::uint32_t, 3>{0, 1, 2}) {
      gti_test_wait_lane::require(gti_test_wait_lane::load<std::uint64_t>(&value, order) == 7);
    }
    return 0;
  }
  switch (argv[1][0]) {
  case 'n': (void)gti_test_wait_lane::load<std::uint32_t>(nullptr, 0); break;
  case 'm': (void)gti_test_wait_lane::load<std::uint32_t>(reinterpret_cast<void *>(std::uintptr_t(1)), 0); break;
  case 'o': (void)gti_test_wait_lane::load<std::uint64_t>(&value, 99); break;
  case 'w': gti_test_wait_lane::validateAddress(&value, 3); break;
  default: return 2;
  }
  return 3;
}
''')
    executable = work / "guard-controls"
    run([compiler, "-std=c++20", "-O3", "-fno-strict-aliasing", "-I", str(fixture),
         str(driver), "-o", str(executable)])
    run([str(executable)])
    for case in ("null", "misaligned", "order", "width"):
        result = subprocess.run([str(executable), case], capture_output=True, timeout=15)
        if result.returncode != -signal.SIGABRT:
            raise AssertionError(f"{case}: expected guard abort, got {result.returncode}")
    print("guard controls: unsupported targets/compiler/ABI and non-unsigned domains "
          "reject; four malformed runtime requests abort before lane access", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-cxx", default="clang++")
    args = parser.parse_args()
    fixture = args.root.resolve() / "tests/fixtures"
    source = fixture / "llvm_atomic_wait_lane_codegen.cpp"
    with tempfile.TemporaryDirectory(prefix="gti-lane-contract-") as temporary:
        work = Path(temporary)
        print("test-only native extension; -O3 -fno-strict-aliasing", flush=True)
        if "#define __clang__ " in macros(args.cxx):
            check_clang(args.cxx, source, work, "native-clang")
        else:
            check_gcc(args.cxx, source, work)
        check_clang(args.llvm_cxx, source, work, "llvm-clang")
        check_rejections(args.cxx, args.llvm_cxx, fixture, work)


if __name__ == "__main__":
    main()
