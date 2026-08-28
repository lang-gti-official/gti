#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def includes(path: pathlib.Path) -> set[str]:
    return set(INCLUDE.findall(path.read_text(encoding="utf-8")))


def fail(message: str) -> None:
    raise SystemExit(f"MIR header boundary: {message}")


def main() -> None:
    root = pathlib.Path(sys.argv[1]).resolve()
    headers = {
        "model": root / "include/gti/mir.h",
        "editor": root / "include/gti/mir_editor.h",
        "effects": root / "include/gti/mir_effects.h",
        "lowering": root / "include/gti/mir_lowering.h",
        "verifier": root / "include/gti/mir_verifier.h",
    }
    if not all(path.is_file() for path in headers.values()):
        fail("model, editor, effects, lowering, and verifier headers must exist")

    forbidden_model_dependencies = {
        "gti/mir_editor.h",
        "gti/mir_effects.h",
        "gti/mir_lowering.h",
        "gti/mir_verifier.h",
    }
    if forbidden_model_dependencies & includes(headers["model"]):
        fail("mir.h must not depend on an operational MIR service")

    for name in ("editor", "effects", "lowering", "verifier"):
        if "gti/mir.h" not in includes(headers[name]):
            fail(f"{headers[name].name} must depend on the MIR model")

    if "gti/mir_verifier.h" not in includes(headers["editor"]):
        fail("the transactional editor must expose structured verification")
    if "gti/mir_effects.h" not in includes(headers["lowering"]):
        fail("program lowering must consume the separate MIR effect service")

    model_text = headers["model"].read_text(encoding="utf-8")
    forbidden_model_declarations = (
        "MirVerificationResult",
        "struct MirLoweringResult",
        "deriveMirDefinedFailureEffects",
        "verifyMirOptimizationCoherence",
        "class MirEditor",
    )
    for declaration in forbidden_model_declarations:
        if declaration in model_text:
            fail(f"mir.h still exports operational declaration {declaration}")

    obsolete = (
        root / "include/gti/optimization/effects.h",
        root / "include/gti/optimization/rewrite.h",
        root / "src/compiler/optimization/effects.cpp",
        root / "src/compiler/optimization/rewrite.cpp",
    )
    if any(path.exists() for path in obsolete):
        fail("optimizer-owned MIR effect/rewrite paths must not reappear")

    production_requirements = {
        "verifyMirProgram": "gti/mir_verifier.h",
        "MirLowerer": "gti/mir_lowering.h",
        "MirEditor": "gti/mir_editor.h",
        "deriveMirProgramEffectSummaries": "gti/mir_effects.h",
    }
    for directory in ("include", "src"):
        for path in (root / directory).rglob("*"):
            if path.suffix not in {".h", ".hpp", ".cc", ".cpp"}:
                continue
            text = path.read_text(encoding="utf-8")
            direct = includes(path)
            for symbol, owner in production_requirements.items():
                if symbol not in text:
                    continue
                if path == root / "include" / owner:
                    continue
                if path == headers["model"] and symbol == "MirLowerer":
                    continue
                if owner not in direct:
                    fail(f"{path.relative_to(root)} uses {symbol} without {owner}")


if __name__ == "__main__":
    main()
