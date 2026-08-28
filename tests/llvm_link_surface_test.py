#!/usr/bin/env python3

import subprocess
import sys
import tempfile
from pathlib import Path


def run(checker: str, graph: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="gti-llvm-link-surface-") as value:
        build = Path(value)
        (build / "build.ninja").write_text(graph)
        return subprocess.run(
            [sys.executable, checker, str(build)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: llvm_link_surface_test.py CHECKER")

    checker = sys.argv[1]
    bundled_graph = """\
build _deps/llvm/libLLVMCore.a: CXX_STATIC_LIBRARY_LINKER LLVMCore.o
  LINK_LIBRARIES = _deps/llvm/libLLVMSupport.a
build libgti_compiler.a: CXX_STATIC_LIBRARY_LINKER gti.o
build gti: CXX_EXECUTABLE_LINKER main.o | libgti_compiler.a
  LINK_LIBRARIES = libgti_compiler.a libLLVMSupport.a libLLVMTargetParser.a libLLVMDemangle.a
"""
    result = run(checker, bundled_graph)
    if result.returncode != 0:
        raise AssertionError(
            "bundled LLVM's private Ninja graph must not fail GTI's link gate:\n"
            + result.stdout
            + result.stderr
        )

    bundled_order_dependencies_graph = """\
build _deps/llvm/libLLVMSupportBlake3.a: CXX_STATIC_LIBRARY_LINKER blake3.o
build _deps/llvm/libLLVMTableGenBasic.a: CXX_STATIC_LIBRARY_LINKER tablegen.o
build _deps/llvm/libLLVMTableGen.a: CXX_STATIC_LIBRARY_LINKER tablegen-main.o
build gti: CXX_EXECUTABLE_LINKER main.o | libgti_compiler.a || _deps/llvm/libLLVMSupportBlake3.a _deps/llvm/libLLVMTableGenBasic.a _deps/llvm/libLLVMTableGen.a
  LINK_LIBRARIES = libgti_compiler.a libLLVMSupport.a libLLVMTargetParser.a libLLVMDemangle.a
"""
    result = run(checker, bundled_order_dependencies_graph)
    if result.returncode != 0:
        raise AssertionError(
            "bundled LLVM order-only dependencies are not linked libraries:\n"
            + result.stdout
            + result.stderr
        )

    forbidden_graph = """\
build gti: CXX_EXECUTABLE_LINKER main.o
  LINK_LIBRARIES = libLLVMSupport.a libLLVMCore.a
"""
    result = run(checker, forbidden_graph)
    if result.returncode == 0 or "LLVMCore" not in result.stderr:
        raise AssertionError(
            "a forbidden library in a GTI Ninja link rule must fail:\n"
            + result.stdout
            + result.stderr
        )

    libclang_graph = """\
build _deps/llvm/libclangAST.a: CXX_STATIC_LIBRARY_LINKER clangAST.o
build gti: CXX_EXECUTABLE_LINKER main.o
  COMMAND = /usr/bin/clang++ -o gti main.o
  LINK_LIBRARIES = libgti_bindgen.a /usr/lib/llvm-18/lib/libclang-18.so.18.1 libLLVMSupport.a
"""
    result = run(checker, libclang_graph)
    if result.returncode != 0:
        raise AssertionError(
            "libclang, the compiler driver path, and clang's private build "
            "graph must all pass GTI's link gate:\n"
            + result.stdout
            + result.stderr
        )

    forbidden_clang_graph = """\
build gti: CXX_EXECUTABLE_LINKER main.o
  LINK_LIBRARIES = libclang.dylib libclangAST.a libclang-cpp.so.18
"""
    result = run(checker, forbidden_clang_graph)
    if (
        result.returncode == 0
        or "clangAST" not in result.stderr
        or "clang-cpp" not in result.stderr
    ):
        raise AssertionError(
            "a Clang component library in a GTI Ninja link rule must fail:\n"
            + result.stdout
            + result.stderr
        )

    print("LLVM Ninja link-surface filtering passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
