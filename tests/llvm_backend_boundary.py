"""LLVM source ownership and includes; sealed public contracts stop traversal."""

from __future__ import annotations

from pathlib import Path
import re


CONTRACT_HEADERS = {
    "include/gti/backend.h",
    "include/gti/lowered_program.h",
    "include/gti/mir.h",
    "include/gti/target.h",
    "include/gti/diagnostic.h",
}
FORBIDDEN_NAME = re.compile(
    r"^(?:cpp_|ast(?:\.|_)|frontend(?:\.|_)|hir(?:\.|_)|semantic_|"
    r"optimizer(?:\.|_)|optimization|mir_lowering|mir_body_lowering|"
    r"lowered_program_builder|lowered_program_construction)"
)
FORBIDDEN_IDENTIFIER = re.compile(
    r"\b(?:BackendInput|FrontendResult|HirProgram|SemanticModel|Program|"
    r"OptimizationResult|LoweredProgramBuilder|CppBackend|CppEmitter|"
    r"CppMir\w*|CppProgram\w*)\b|\bsourceMir\b"
)
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.M)
COMMENT = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', re.S)


def code_without_comments(text: str) -> str:
    return COMMENT.sub(lambda m: m.group() if m.group().startswith('"') else " ", text)


def audit(root: Path) -> list[str]:
    root = root.resolve()
    failures = []
    cmake = code_without_comments((root / "CMakeLists.txt").read_text())
    cmake = re.sub(r'"(?:\\.|[^"\\])*"|#[^\n]*',
                   lambda m: m.group() if m.group().startswith('"') else " ", cmake)
    # LLVM ownership is deliberately literal and reviewable. A variable or
    # generator expression must not hide a source from this boundary gate.
    blocks = re.findall(
        r'\b(?:add_library|target_sources)\s*\(\s*"?gti_llvm_backend"?\s+([^)]*)\)',
        cmake,
    )
    compiled = set()
    for block in blocks:
        for token in re.findall(r'"[^"]*"|\S+', block):
            token = token.strip('"')
            if token in {"STATIC", "PRIVATE", "PUBLIC", "INTERFACE"}:
                continue
            if not re.fullmatch(r"[A-Za-z0-9_./-]+\.(?:cpp|cc|cxx|h|hpp|def)", token):
                failures.append(f"gti_llvm_backend: nonliteral source ownership: {token}")
                continue
            if token.endswith((".cpp", ".cc", ".cxx")):
                compiled.add(token)
    if not compiled:
        failures.append("gti_llvm_backend: no compiled source ownership")

    owned = {"include/gti/llvm_backend.h"}
    owned.update(p.relative_to(root).as_posix() for p in (root / "include/gti").glob("llvm_*.h"))
    owned.update(p.relative_to(root).as_posix() for p in (root / "src/compiler").glob("llvm_*.cpp"))
    owned.update(p.relative_to(root).as_posix() for p in (root / "src/compiler/llvm").rglob("*")
                 if p.suffix in {".cpp", ".cc", ".cxx", ".h", ".hpp", ".def"})
    owned.update(compiled)
    discovered_cpp = {p for p in owned if p.endswith((".cpp", ".cc", ".cxx"))}
    for missing in sorted(discovered_cpp - compiled):
        failures.append(f"{missing}: LLVM source is not owned by gti_llvm_backend")

    pending = [(name, name, name.startswith("include/")) for name in sorted(owned)]
    visited = set()
    while pending:
        name, origin, public = pending.pop()
        if (name, origin, public) in visited:
            continue
        visited.add((name, origin, public))
        path = root / name
        if not path.is_file():
            failures.append(f"{origin}: missing project include/source {name}")
            continue
        if name not in CONTRACT_HEADERS and (
            FORBIDDEN_NAME.match(path.name) or "optimization" in Path(name).parts
        ):
            failures.append(f"{origin}: forbidden LLVM dependency {name}")
            continue
        if name in CONTRACT_HEADERS:
            # These value contracts currently share headers with legacy types;
            # consumer code still cannot name frontend object authority.
            continue
        code = code_without_comments(path.read_text())
        if public and re.search(r"\bllvm\s*::|\bnamespace\s+llvm\b", code):
            failures.append(f"{origin}: public backend header exposes LLVM types in {name}")
        if match := FORBIDDEN_IDENTIFIER.search(code):
            failures.append(f"{origin}: forbidden LLVM authority {match.group()} in {name}")
        for include in INCLUDE.findall(code):
            if include.startswith(("llvm/", "clang/")):
                if public or include.startswith("clang/"):
                    failures.append(f"{origin}: forbidden exposed LLVM/Clang dependency {include}")
                continue
            candidates = [path.parent / include, root / "include" / include,
                          root / "src/compiler" / include]
            resolved = next((p.resolve() for p in candidates if p.is_file()), None)
            if resolved is None:
                if include.startswith("gti/"):
                    failures.append(f"{origin}: unresolved project include {include}")
                continue
            if not resolved.is_relative_to(root):
                failures.append(f"{origin}: project include escapes repository: {include}")
                continue
            pending.append((resolved.relative_to(root).as_posix(), origin, public))

    # Public contracts stop the authority walk because they currently cohabit
    # with legacy declarations. They do not stop the public LLVM exposure walk.
    public_pending = [root / name for name in sorted(owned) if name.startswith("include/")]
    public_visited = set()
    while public_pending:
        path = public_pending.pop()
        if path in public_visited or not path.is_file():
            continue
        public_visited.add(path)
        code = code_without_comments(path.read_text())
        name = path.relative_to(root).as_posix()
        if re.search(r"\bllvm\s*::|\bnamespace\s+llvm\b", code):
            failures.append(f"{name}: public contract exposes LLVM types")
        for include in INCLUDE.findall(code):
            if include.startswith(("llvm/", "clang/")):
                failures.append(f"{name}: public contract exposes LLVM/Clang include {include}")
            candidate = (root / "include" / include).resolve()
            if candidate.is_relative_to(root / "include") and candidate.is_file():
                public_pending.append(candidate)
    return sorted(set(failures))
