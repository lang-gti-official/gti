"""Run the actual library producer on each stdlib source and retain per-unit outcomes."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def inventory(compiler, library, output):
    with tempfile.TemporaryDirectory(prefix="gti-package-coverage-") as temp:
        project = Path(temp)
        sources = project / "stdlib"
        shutil.copytree(library, sources)
        env = dict(os.environ, GTI_STDLIB_PATH=str(sources))
        units = sorted(p.relative_to(sources) for p in sources.rglob("*.gti"))
        targets = {}
        manifest = ['manifest-version = 1', '[package]', 'name = "gti-std"', 'version = "1.0.0"']
        for unit in units:
            # Retain ordinary public module names, and qualify nested private/test targets.
            parts = unit.with_suffix("").parts
            name = "_".join(parts[1:] if parts[0] == "std" and len(parts) > 1 else parts)
            if name in targets:
                raise ValueError(f"duplicate coverage target {name}: {unit}, {targets[name]}")
            targets[name] = unit

        rows = []
        for name, unit in targets.items():
            target = [f'[targets."{name}"]', 'kind = "gti-library"', f'root = "stdlib/{unit.as_posix()}"']
            # A one-library project automatically registers its root as a user
            # package module. Use the same multi-target ownership as the real
            # toolchain producer; only the named target is actually built.
            other = "array" if name == "cmath" else "cmath"
            target += [f'[targets.{other}]', 'kind = "gti-library"', f'root = "stdlib/std/{other}.gti"']
            (project / "gti.toml").write_text("\n".join(manifest + target) + "\n")
            cmd = [str(compiler), "build", name, "--release", "--offline"]
            result = subprocess.run(cmd, cwd=project, env=env, text=True, capture_output=True)
            log = result.stdout + result.stderr
            artifact = list((project / "build/gti/release").glob(f"*/{name}.gtilib"))
            if result.returncode == 0 and len(artifact) != 1:
                raise RuntimeError(f"{unit}: successful producer did not publish exactly one artifact")
            reason = next((line.strip() for line in log.splitlines() if
                           "compiled template:" in line or "gti: compiled library:" in line or
                           "error[" in line or line.startswith("gti:")), "")
            if result.returncode and not reason:
                reason = next((line for line in reversed(log.splitlines()) if line.strip()), "no diagnostic")
            if "valid GTI program: " in reason:
                reason = reason.split("valid GTI program: ", 1)[1]
            elif "error[" in reason:
                reason = reason[reason.index("error["):]
            rows.append(dict(unit=unit.as_posix(), test=unit.parts[0] == "tests",
                             result="failed" if result.returncode else "built",
                             diagnostic=reason, log=log if result.returncode else ""))
        output.write_text(json.dumps(rows, indent=2) + "\n")
        for (kind, diagnostic), count in Counter((r["result"], r["diagnostic"]) for r in rows).most_common():
            print(count, kind, diagnostic)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--stdlib", type=Path, default=Path(__file__).resolve().parents[1] / "stdlib")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    inventory(args.compiler.resolve(), args.stdlib.resolve(), args.output.resolve())
