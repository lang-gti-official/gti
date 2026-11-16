#!/usr/bin/env python3
"""Which execution modes a runtime harness exercises, from the environment.

LLVM is GTI's only tested backend. The frozen C++ backend's tests and CI were
removed (owner decision, 2026-11-16; docs/plans/test-suite-redesign.md); its
source stays and builds only with GTI_ENABLE_CPP_BACKEND. GTI_TEST_BACKENDS
therefore has one value, ``llvm``; any other value is a stale configuration
and fails loudly. The C++ helpers below (``cpp_modes``, ``keep_cpp``, ...)
remain only so existing harness call sites need no edit: under the
environment policy they plan no C++ mode. A manual oracle may still pass
``backends="cpp"`` explicitly; no registered test does.

GTI_TEST_LLVM_LEVELS selects the LLVM optimisation levels a harness runs when
it does not name its own: a comma-separated subset of O0, O1, O2 and O3. The
default is ``O0``, which is what pull requests and per-commit full validation
run. The daily slow tier (.github/workflows/slow.yml, through
``scripts/validation_checks.py shard --test-set``) sets it to add ``O3``. A
harness whose subject is an optimisation level names its levels explicitly
(``native_modes(levels=("O0", "O2", "O3"))``) and is unaffected.

Harness mode tables call ``native_modes`` instead of listing levels and flags
themselves, so this policy has one definition. A harness never spells
``--std``, a C++ standard or ``--backend cpp`` itself:
tests/execution_coverage_test.py rejects a runtime harness that does, because
a hard-coded table is how a fixture loses its LLVM execution modes without
anyone noticing.

A fixture the LLVM backend cannot execute yet is declared, never skipped:
``unsupported(name, flags, llvm_unsupported=...)`` returns the declared
limitation and ``check_unsupported`` asserts the compiler reports exactly it
(a ``GTI-B0003`` rejection) or, for a declared bug symptom, that the fixture
still fails, so a stale declaration is an error.
"""

from __future__ import annotations

import os

BACKENDS = ("llvm", "cpp", "both")
# The one C++ profile, for explicit manual oracles only (see above).
CPP_PROFILE = ("cpp-c++23-O3", ["--std", "c++23", "-O3"], [])
LEVELS = ("O0", "O1", "O2", "O3")
LEVELS_ENVIRONMENT = "GTI_TEST_LLVM_LEVELS"
DEFAULT_LEVELS = ("O0",)


def selected_backends() -> str:
    """The environment's backend policy: always LLVM."""
    value = os.environ.get("GTI_TEST_BACKENDS", "llvm")
    if value != "llvm":
        raise ValueError("GTI_TEST_BACKENDS must be llvm: the C++ backend's tests were removed "
                         "(docs/plans/test-suite-redesign.md)")
    return value


def selected_levels() -> tuple[str, ...]:
    """The LLVM levels GTI_TEST_LLVM_LEVELS selects (default O0)."""
    value = os.environ.get(LEVELS_ENVIRONMENT, "")
    if not value.strip():
        return DEFAULT_LEVELS
    levels = tuple(level.strip() for level in value.split(","))
    if not levels or any(level not in LEVELS for level in levels) or len(set(levels)) != len(levels):
        raise ValueError(f"{LEVELS_ENVIRONMENT} must be a comma-separated subset of " + ", ".join(LEVELS))
    return levels


def llvm_modes(levels=None):
    """LLVM execution modes: GTI_TEST_LLVM_LEVELS' levels unless `levels` names them."""
    return [(f"llvm-{level}", ["--backend", "llvm", f"-{level}"], [])
            for level in (selected_levels() if levels is None else levels)]


def cpp_modes(backends: str | None = None):
    """The C++ profile, only when a caller explicitly asks for the C++ backend."""
    selected = backends or selected_backends()
    return [CPP_PROFILE] if selected in ("cpp", "both") else []


def cpp_text_modes(backends: str | None = None):
    """The C++ profile without an optimisation level, for ``--emit-cpp`` text probes.

    Empty unless a caller explicitly asks for the C++ backend.
    """
    return [("cpp-c++23", ["--std", "c++23"], [])] if cpp_modes(backends) else []


def cpp_variant_mode(label, level="O1", native=()):
    """A C++ profile variant; callers append it only when ``cpp_modes()`` is nonempty."""
    return (label, ["--std", CPP_PROFILE[1][1], f"-{level}"], list(native))


def native_modes(levels=None, backends: str | None = None):
    """The (label, compiler flags, native flags) modes a harness runs."""
    selected = backends or selected_backends()
    if selected not in BACKENDS:
        raise ValueError("backends must be one of " + ", ".join(BACKENDS))
    modes = []
    if selected in ("llvm", "both"):
        modes += llvm_modes(levels)
    modes += cpp_modes(selected)
    return modes


def is_cpp_mode(flags) -> bool:
    return "--std" in flags


def is_llvm_mode(flags) -> bool:
    flags = list(flags)
    return "--backend" in flags and flags[flags.index("--backend") + 1] == "llvm"


def optimization_level(flags) -> str:
    """The mode's ``O<n>`` level, for a native helper compiled alongside the fixture."""
    return next(flag[1:] for flag in flags if flag.startswith("-O"))


def cpp_standard(flags) -> str:
    """The C++ standard a C++ mode selects, for a host compiler run on its text."""
    flags = list(flags)
    return flags[flags.index("--std") + 1]


def keep_cpp(flags):
    """``--keep-cpp`` under the C++ profile; nothing under LLVM, which rejects it."""
    return ["--keep-cpp"] if is_cpp_mode(flags) else []


def unsupported(name, flags, llvm_unsupported=None, cpp_unsupported=None):
    """The declared limitation of `flags`' backend for fixture `name`, or None.

    Each declaration is one text for every fixture of the harness or a mapping
    from fixture name to text. A text naming a diagnostic code (``GTI-B0003``
    or another ``GTI-`` code) is an exact rejection; any other text is the
    observed symptom of a filed backend bug, which must still reproduce.
    """
    declared = cpp_unsupported if is_cpp_mode(flags) else llvm_unsupported
    if declared is None:
        return None
    if isinstance(declared, str):
        return declared
    return declared.get(name)


def is_rejection(limitation: str) -> bool:
    return "GTI-" in limitation


def backend_name(flags) -> str:
    return "C++" if is_cpp_mode(flags) else "LLVM"


def check_unsupported(built, limitation, name, label, flags):
    """Assert the compiler reported the declared rejection for `name` under `flags`.

    Returns True when the declaration is a rejection (the caller stops here)
    and False for a symptom declaration, whose fixture the caller must still
    build and run, failing if it unexpectedly passes (``record_symptom``).
    """
    if not is_rejection(limitation):
        return False
    assert built.returncode == 65 and "GTI-B0003" in built.stderr and limitation in built.stderr, \
        (name, label, built.returncode, built.stdout, built.stderr)
    print(f"RECORDED {backend_name(flags)} LIMITATION {name}/{label}: {limitation}", flush=True)
    return True


def record_symptom(limitation, name, label, flags, reproduced: bool):
    """A declared bug symptom must still reproduce; a passing fixture is a stale declaration."""
    assert reproduced, (f"{name}/{label}: the declared {backend_name(flags)} limitation no longer "
                        f"reproduces; remove the declaration: {limitation}")
    print(f"RECORDED {backend_name(flags)} LIMITATION {name}/{label}: {limitation}", flush=True)


def cpp_only_case(reason, name, label):
    """Record a case only the C++ profile can observe (a test provider that
    replaces the C++ text's allocation operator, say); the fixture itself
    still executes on every mode."""
    print(f"RECORDED C++-ONLY CASE {name}/{label}: {reason}", flush=True)


def mode_plan(names, modes, llvm_unsupported=None, cpp_unsupported=None):
    """Rows ``(fixture, label, plan)``; plan is ``execute`` or ``unsupported: <text>``."""
    rows = []
    for label, flags, _native in modes:
        for name in names:
            limitation = unsupported(name, flags, llvm_unsupported, cpp_unsupported)
            rows.append((name, label, "execute" if limitation is None else f"unsupported: {limitation}"))
    return rows


def print_mode_plan(rows):
    """``--list-modes`` output: one tab-separated row per fixture and mode, no compilation."""
    for name, label, plan in rows:
        print(f"{name}\t{label}\t{plan}")
