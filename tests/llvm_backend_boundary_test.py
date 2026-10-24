#!/usr/bin/env python3
"""Mutation controls for dynamically discovered LLVM helper dependencies."""

from pathlib import Path
import tempfile

from llvm_backend_boundary import audit


def write(root: Path, name: str, text: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="gti-llvm-boundary-") as raw:
        root = Path(raw)
        cmake = 'add_library("gti_llvm_backend" STATIC "src/compiler/llvm_backend.cpp" "src/compiler/llvm/helper.cpp")\n'
        write(root, "CMakeLists.txt", cmake)
        write(root, "include/gti/llvm_backend.h", '#include "gti/backend.h"\n')
        write(root, "include/gti/backend.h", 'class LoweredProgram;\n')
        write(root, "src/compiler/llvm_backend.cpp", '#include "gti/llvm_backend.h"\n')
        write(root, "src/compiler/llvm/helper.cpp", '#include "private.h"\n')
        write(root, "src/compiler/llvm/private.h", 'class VerifiedLoweredProgram;\n')
        assert not audit(root), audit(root)

        probes = [
            ('#include "../cpp_mir_body_emitter.h"\n', "forbidden LLVM dependency"),
            ('#include "../lowered_program_builder.h"\n', "forbidden LLVM dependency"),
            ('#include "gti/frontend.h"\n', "forbidden LLVM dependency"),
            ('#include "gti/hir.h"\n', "forbidden LLVM dependency"),
            ('#include "gti/semantic_model.h"\n', "forbidden LLVM dependency"),
            ('#include "../optimization/private.h"\n', "forbidden LLVM dependency"),
            ('SemanticModel* state;\n', "forbidden LLVM authority"),
        ]
        for name in ("src/compiler/cpp_mir_body_emitter.h", "src/compiler/lowered_program_builder.h",
                     "include/gti/frontend.h", "include/gti/hir.h", "include/gti/semantic_model.h"):
            write(root, name, "// intentionally forbidden\n")
        write(root, "src/compiler/optimization/private.h", "OptimizationResult state;\n")
        for text, expected in probes:
            write(root, "src/compiler/llvm/private.h", text)
            failures = audit(root)
            assert any(expected in failure and "helper.cpp" in failure for failure in failures), failures
        write(root, "src/compiler/llvm/private.h", "// SemanticModel is forbidden in code\n")
        assert not audit(root), audit(root)

        write(root, "src/compiler/llvm/unlisted.cpp", "int helper();\n")
        assert any("not owned" in failure for failure in audit(root))
        write(root, "CMakeLists.txt", cmake + 'target_sources(gti_llvm_backend PRIVATE "src/compiler/llvm/unlisted.cpp")\n')
        assert not audit(root), audit(root)
        write(root, "CMakeLists.txt", cmake + 'target_sources(gti_llvm_backend PRIVATE ${HIDDEN_SOURCES})\n')
        assert any("nonliteral" in failure for failure in audit(root))
        write(root, "CMakeLists.txt", cmake + 'target_sources(gti_llvm_backend PRIVATE "src/compiler/llvm/unlisted.cpp")\n')
        write(root, "include/gti/llvm_backend.h", '#include "gti/llvm_detail.h"\n')
        write(root, "include/gti/llvm_detail.h", '#include <llvm/IR/Module.h>\n')
        assert any("exposed LLVM" in failure for failure in audit(root))
    print("LLVM dependency discovery and transitive mutation controls passed")


if __name__ == "__main__":
    main()
