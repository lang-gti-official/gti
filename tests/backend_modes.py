#!/usr/bin/env python3
"""Which backends a runtime harness exercises, from the environment.

GTI's shipped backend is LLVM. The C++ backend is deprecated and frozen; it
runs on its own schedule under one profile (the shipped default's standard
at -O3), as rustc tests its non-primary codegen backends: never per change,
never as a per-feature differential against the primary backend.

GTI_TEST_BACKENDS selects the set: ``llvm`` (the default, and what every CI
gate and full validation run uses), ``cpp`` (the scheduled C++ job) or
``both`` (local differential runs). Harness mode tables call ``native_modes``
instead of listing standards and optimisation levels themselves, so this
policy has one definition. A harness never spells ``--std``, a C++ standard
or ``--backend cpp`` itself: tests/execution_coverage_test.py rejects a
runtime harness that does, because a hard-coded table is how a fixture loses
its LLVM execution modes without anyone noticing.

Assertions on the emitted C++ text belong to the C++ profile only: guard them
with ``is_cpp_mode(flags)`` and request the text with ``keep_cpp(flags)``,
which is empty for an LLVM mode (the LLVM backend rejects ``--keep-cpp``).

A fixture the LLVM backend cannot execute yet is declared, never skipped:
``unsupported(name, flags, llvm_unsupported=...)`` returns the declared
limitation and ``check_unsupported`` asserts the compiler reports exactly it
(a ``GTI-B0003`` rejection) or, for a declared bug symptom, that the fixture
still fails, so a stale declaration is an error. The C++ profile keeps that
fixture's execution evidence until the LLVM backend accepts it.
"""

from __future__ import annotations

import os

BACKENDS = ("llvm", "cpp", "both")
# The one C++ profile. c++23 is the shipped default's standard; -O3 is the
# production configuration. Other standards and levels, no-elide and the
# GCC-instrumented "sanitized" mode are not run anywhere: the backend is
# frozen, so they could only report drift in a configuration nothing ships.
CPP_PROFILE = ("cpp-c++23-O3", ["--std", "c++23", "-O3"], [])
LLVM_LEVELS = ("O0", "O3")


def selected_backends() -> str:
    value = os.environ.get("GTI_TEST_BACKENDS", "llvm")
    if value not in BACKENDS:
        raise ValueError("GTI_TEST_BACKENDS must be one of " + ", ".join(BACKENDS))
    return value


def llvm_modes(levels=LLVM_LEVELS):
    """LLVM execution modes: the unoptimised and the production level."""
    return [(f"llvm-{level}", ["--backend", "llvm", f"-{level}"], []) for level in levels]


def cpp_modes(backends: str | None = None):
    """The C++ profile, only when GTI_TEST_BACKENDS selects the C++ backend."""
    selected = backends or selected_backends()
    return [CPP_PROFILE] if selected in ("cpp", "both") else []


def cpp_text_modes(backends: str | None = None):
    """The C++ profile without an optimisation level, for ``--emit-cpp`` text probes.

    Empty unless GTI_TEST_BACKENDS selects the C++ backend: a probe of the
    emitted text proves nothing about the shipped backend.
    """
    return [("cpp-c++23", ["--std", "c++23"], [])] if cpp_modes(backends) else []


def cpp_variant_mode(label, level="O1", native=()):
    """A C++ profile variant for a harness's own C++-text check (thread sanitizer
    evidence or host-macro hygiene that only the C++ text carries); the caller
    appends it only when ``cpp_modes()`` is nonempty."""
    return (label, ["--std", CPP_PROFILE[1][1], f"-{level}"], list(native))


def native_modes(levels=LLVM_LEVELS, backends: str | None = None):
    """The (label, compiler flags, native flags) modes a harness runs."""
    selected = backends or selected_backends()
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
