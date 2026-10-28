"""Backend-neutral compile/execute results; representation checks stay clients."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import platform
import subprocess
import sys


def host_default_backend() -> str:
    """The backend `gti` builds with on this host when none is selected."""
    machine = platform.machine().lower()
    promoted = ((sys.platform == "darwin" and machine in {"arm64", "aarch64"}) or
                (sys.platform.startswith("linux") and machine in {"x86_64", "amd64"}))
    return "llvm" if promoted else "cpp"


@dataclass(frozen=True)
class Endpoint:
    backend: str
    optimization: str
    standard: str | None = None
    keep_cpp: bool = False

    def __post_init__(self) -> None:
        if self.backend not in {"cpp", "llvm"}:
            raise ValueError(f"unknown execution backend: {self.backend}")
        if self.optimization not in {"O0", "O1", "O2", "O3"}:
            raise ValueError(f"unknown optimization: {self.optimization}")
        if self.backend == "llvm" and (self.standard is not None or self.keep_cpp):
            raise ValueError("native LLVM endpoints cannot request C++ representation")
        if self.standard is not None and self.standard not in {"c++20", "c++23"}:
            raise ValueError(f"unknown C++ standard: {self.standard}")


@dataclass(frozen=True)
class ExecutionResult:
    command: tuple[str, ...]
    compile_returncode: int
    returncode: int
    stdout: str
    stderr: str
    generated_cpp: str | None = None
    error: str = ""

    @property
    def observable(self) -> tuple[int, str, str]:
        return self.returncode, self.stdout, self.stderr


def plan_compile_command(gti: Path, source: Path, executable: Path, endpoint: Endpoint,
                         *, concurrent: bool = False) -> list[str]:
    command = [str(gti), str(source), "--backend", endpoint.backend,
               f"-{endpoint.optimization}"]
    if concurrent:
        command += ["--execution-profile", "concurrent"]
    if endpoint.standard:
        command += ["--std", endpoint.standard]
    if endpoint.keep_cpp:
        command += ["--keep-cpp"]
    command += ["-o", str(executable)]
    return command


def execute(gti: Path, root: Path, case: Path, source: Path, endpoint: Endpoint,
            env: dict[str, str], *, concurrent: bool = False,
            build_timeout: int = 240, run_timeout: int = 15) -> ExecutionResult:
    case.mkdir(parents=True, exist_ok=True)
    executable = case / ("program.exe" if os.name == "nt" else "program")
    if executable.exists():
        raise ValueError("an execution case must use a fresh artifact destination")
    command = plan_compile_command(gti, source, executable, endpoint, concurrent=concurrent)
    try:
        built = subprocess.run(command, cwd=root, env=env, text=True,
                               capture_output=True, check=False, timeout=build_timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return ExecutionResult(tuple(command), -1, -1, "", "", error=f"build failed: {error}")
    if built.returncode != 0 or not executable.is_file():
        detail = built.stderr.strip() or built.stdout.strip() or "no executable published"
        return ExecutionResult(tuple(command), built.returncode, -1,
                               built.stdout, built.stderr, error=f"build failed: {detail}")
    generated = None
    if endpoint.keep_cpp:
        path = Path(f"{executable}.gti.cpp")
        if not path.is_file():
            return ExecutionResult(tuple(command), built.returncode, -1, built.stdout,
                                   built.stderr, error="build failed: no retained C++ artifact")
        generated = path.read_text(encoding="utf-8", errors="replace")
    try:
        ran = subprocess.run([str(executable)], cwd=root, env=env, text=True,
                             capture_output=True, check=False, timeout=run_timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return ExecutionResult(tuple(command), built.returncode, -1, "", "",
                               generated, f"execution failed: {error}")
    return ExecutionResult(tuple(command), built.returncode, ran.returncode,
                           ran.stdout, ran.stderr, generated)
