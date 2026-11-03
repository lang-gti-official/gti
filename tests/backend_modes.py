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
policy has one definition.
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


def native_modes(levels=LLVM_LEVELS, backends: str | None = None):
    """The (label, compiler flags, native flags) modes a harness runs."""
    selected = backends or selected_backends()
    modes = []
    if selected in ("llvm", "both"):
        modes += llvm_modes(levels)
    if selected in ("cpp", "both"):
        modes.append(CPP_PROFILE)
    return modes


def is_cpp_mode(flags) -> bool:
    return "--std" in flags
