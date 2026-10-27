#!/usr/bin/env python3
"""Stage distribution LLDB without installing packages or changing host policy."""

import argparse
from email.parser import Parser
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
from urllib.parse import unquote


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--package", default="lldb-22")
    args = parser.parse_args()
    root = args.root.resolve()
    evidence = args.evidence.resolve()
    assert not root.exists() and not evidence.exists()
    assert re.fullmatch(r"lldb-[0-9]+", args.package)
    root.mkdir(parents=True)
    evidence.mkdir(parents=True)
    downloads = root / "packages"
    downloads.mkdir()
    (root / "apt/archives/partial").mkdir(parents=True)
    (root / "log").mkdir()
    prefix = root / "sysroot"
    prefix.mkdir()
    manifest = {"status": "running", "commands": [], "packages": []}

    def run(argv, *, env=None, check=True):
        result = subprocess.run([str(x) for x in argv], cwd=downloads,
                                env=env, text=True, capture_output=True, timeout=180)
        log = f"command-{len(manifest['commands']):03}.log"
        (evidence / log).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv],
                                     "exit": result.returncode, "log": log})
        if check and result.returncode:
            raise RuntimeError(f"command failed: {evidence / log}")
        return result

    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    try:
        plan = run(["apt-get", "--print-uris", "--download-only", "--assume-yes",
                    "--no-install-recommends", "-o", "Debug::NoLocking=1",
                    "-o", f"Dir::State::extended_states={root / 'extended_states'}",
                    "-o", f"Dir::Cache={root / 'apt'}",
                    "-o", f"Dir::Log={root / 'log'}", "install", args.package])
        rows = [shlex.split(line) for line in plan.stdout.splitlines() if line.startswith("'")]
        assert rows, "package plan contains no staged debugger"
        for uri, filename, size, _ in rows:
            package, version, architecture = unquote(filename).removesuffix(".deb").rsplit("_", 2)
            assert re.fullmatch(r"[a-z0-9][a-z0-9+.-]*", package)
            selection = f"{package}:{architecture}={version}"
            metadata = run(["apt-cache", "show", selection]).stdout
            records = [Parser().parsestr(part) for part in metadata.strip().split("\n\n")]
            record = next(row for row in records if row["Package"] == package
                          and row["Version"] == version and row["Architecture"] == architecture)
            expected = record["SHA256"]
            assert expected and re.fullmatch(r"[0-9a-f]{64}", expected)
            before = set(downloads.glob("*.deb"))
            run(["apt-get", "download", selection])
            added = set(downloads.glob("*.deb")) - before
            assert len(added) == 1
            archive = added.pop()
            assert archive.stat().st_size == int(size) and digest(archive) == expected
            run(["dpkg-deb", "--extract", archive, prefix])
            manifest["packages"].append({"package": package, "version": version,
                                          "architecture": architecture, "uri": uri,
                                          "sha256": expected, "bytes": int(size)})
        major = args.package.removeprefix("lldb-")
        executable = prefix / f"usr/lib/llvm-{major}/bin/lldb"
        assert executable.is_file()
        libraries = [prefix / "usr/lib/x86_64-linux-gnu", prefix / f"usr/lib/llvm-{major}/lib"]
        python = sorted({path.parent.parent for path in prefix.rglob("lldb/__init__.py")})
        additions = {"LD_LIBRARY_PATH": ":".join(map(str, libraries)),
                     "PYTHONPATH": ":".join(map(str, python))}
        env = os.environ.copy()
        env.update(additions)
        manifest["environment"] = additions
        manifest["lldb_version"] = run([executable, "--version"], env=env).stdout.strip()
        manifest["executable"] = {"path": str(executable), "sha256": digest(executable)}
        dependencies = run(["ldd", executable], env=env).stdout
        assert "not found" not in dependencies
        resolved = sorted(set(re.findall(r"(/[^\s]+)\s+\(0x", dependencies)))
        manifest["loaded_libraries"] = [{"path": name, "sha256": digest(Path(name)),
                                         "staged": Path(name).is_relative_to(prefix)} for name in resolved]
        wrapper = root / "lldb"
        wrapper.write_text("#!/usr/bin/python3\nimport os,sys\nenv=os.environ.copy()\n"
                           + "env.update(" + repr(additions) + ")\n"
                           + "os.execve(" + repr(str(executable)) + ", ["
                           + repr(str(executable)) + ", *sys.argv[1:]], env)\n")
        wrapper.chmod(0o755)
        manifest["wrapper_sha256"] = digest(wrapper)
        compiler = shutil.which("clang-21") or shutil.which("clang")
        assert compiler, "native debugger smoke needs a C compiler"
        source = root / "debugger-smoke.c"
        source.write_text("int main(void) { volatile int value = 7; return value == 7 ? 0 : 1; }\n")
        binary = root / "debugger-smoke"
        run([compiler, "-g", "-O0", source, "-o", binary])
        smoke = run([wrapper, "--no-lldbinit", "--batch", binary,
                     "-o", "breakpoint set --name main", "-o", "run",
                     "-o", "thread backtrace", "-o", "continue"], check=False)
        transcript = smoke.stdout + smoke.stderr
        if any(reason in transcript for reason in ("personality set failed", "Operation not permitted",
                                                    "Not allowed to attach")):
            manifest["status"] = "blocked-host-debugger-permission"
            return 77
        assert smoke.returncode == 0 and "stop reason = breakpoint" in transcript
        assert "exited with status = 0" in transcript
        manifest["status"] = "passed-staged-debugger-smoke"
        manifest["wrapper"] = str(wrapper)
        print(json.dumps({"status": manifest["status"], "wrapper": str(wrapper),
                          "version": manifest["lldb_version"]}))
        return 0
    except BaseException as error:
        manifest["status"], manifest["error"] = "failed", str(error)
        raise
    finally:
        (evidence / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
