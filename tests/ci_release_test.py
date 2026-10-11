#!/usr/bin/env python3
"""Exercise exact release/channel admission and shared release packaging.

The Gitea workflows that drove them are archived under
docs/plans/archive/gitea-actions/; the scripts and CMake they called remain
the contracts a replacement workflow must satisfy.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile


def run(command, cwd, success=True):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=30, check=False)
    assert (result.returncode == 0) == success, (
        command, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


def source_contract(root):
    script = root / "scripts/resolve_release_source.py"
    with tempfile.TemporaryDirectory(prefix="gti-release-source-") as temporary:
        work = pathlib.Path(temporary)
        run(["git", "init", "--quiet", "-b", "main"], work)
        run(["git", "config", "user.name", "GTI Test"], work)
        run(["git", "config", "user.email", "test@example.invalid"], work)

        def commit(version, message):
            (work / "VERSION").write_text(version + "\n")
            (work / "note").write_text(message)
            run(["git", "add", "VERSION", "note"], work)
            run(["git", "commit", "--quiet", "-m", message], work)
            return run(["git", "rev-parse", "HEAD"], work)

        count = 0

        def query(sha, event="push", before="", mode="source", success=True):
            nonlocal count
            output = work / f"output-{count}"
            count += 1
            command = [sys.executable, str(script), mode, "--sha", sha,
                       "--event", event, "--before", before, "--output", str(output)]
            run(command, work, success)
            if not success:
                assert not output.exists(), "failed admission wrote runner outputs"
                return {}
            return dict(line.split("=", 1) for line in output.read_text().splitlines())

        first = commit("1.0.0", "first")
        assert query(first, before="0" * 40) == {
            "sha": first, "version": "1.0.0", "tag": "v1.0.0", "stable": "true"}
        run(["git", "tag", "v1.0.0"], work)
        internal = commit("1.0.0", "internal")
        assert query(internal, before=first)["stable"] == "false"
        assert query(internal, before="0" * 40)["stable"] == "false"
        # An older immutable tag must not block an ordinary same-version nightly,
        # but a manual stable rebuild for that different commit is refused.
        query(internal, event="workflow_dispatch", success=False)
        query(first, before=first, success=False)
        query("short", success=False)
        query(internal, before="--all", success=False)
        query(internal, before="f" * 40, success=False)
        (work / "VERSION").write_text("9.9.9\n")
        query(internal, before=first, success=False)
        (work / "VERSION").write_text("1.0.0\n")

        advanced = commit("1.1.0", "stable advance")
        assert query(advanced, before=first)["stable"] == "true", "multi-commit VERSION push"
        assert query(advanced, before=internal)["stable"] == "true"
        run(["git", "tag", "v1.1.0"], work)
        assert query(advanced, event="workflow_dispatch")["stable"] == "true"
        assert query(advanced, before="0" * 40)["stable"] == "true"
        downgraded = commit("1.0.9", "invalid downgrade")
        query(downgraded, before=advanced, success=False)
        invalid = commit("1.2.0\nBAD=value", "invalid version")
        query(invalid, before=downgraded, success=False)

        # Nightly admission is read-only, latest-main-only, and a stale source is
        # an ordinary skip rather than a false release failure.
        latest = commit("1.2.0", "latest")
        remote = work / "remote.git"
        run(["git", "init", "--bare", "--quiet", str(remote)], work)
        run(["git", "remote", "add", "origin", str(remote)], work)
        run(["git", "push", "--quiet", "origin", "main"], work)
        assert query(latest, mode="admit-nightly") == {"publish": "true"}
        run(["git", "checkout", "--quiet", "--detach", advanced], work)
        tags = run(["git", "show-ref", "--tags"], work)
        assert query(advanced, mode="admit-nightly") == {"publish": "false"}
        assert run(["git", "show-ref", "--tags"], work) == tags
        run(["git", "remote", "remove", "origin"], work)
        query(advanced, mode="admit-nightly", success=False)


def archive_contract(root, cmake):
    script = root / "cmake/PackageRelease.cmake"
    requirements = script.read_text().split("foreach(required_file", 1)[1].split("endforeach()", 1)[0]
    required_files = re.findall(r'"([^"\n]+)"', requirements.split("if(NOT EXISTS", 1)[0])
    assert "share/gti/VERSION" in required_files and "bin/gti" in required_files
    with tempfile.TemporaryDirectory(prefix="gti-shared-release-archive-") as temporary:
        work = pathlib.Path(temporary)
        stage = work / "stage"
        for relative in required_files:
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(f"staged fixture: {relative}\n".encode())
        version = (root / "VERSION").read_text().strip()
        (stage / "share/gti/VERSION").write_text(version + "\n")
        libclang = stage / "lib/gti/llvm/libclang.so.20.1"
        libclang.write_bytes(b"fixture libclang\n")
        libclang.with_name("libclang.so.20").symlink_to(libclang.name)

        def package(channel, tag, destination, build_id="", success=True, platform="linux-x64"):
            command = [str(cmake), f"-DGTI_SOURCE_DIR={root}", f"-DGTI_STAGE_DIR={stage}",
                       f"-DGTI_OUTPUT_DIR={destination}", f"-DGTI_PLATFORM={platform}",
                       f"-DGTI_CHANNEL={channel}", f"-DGTI_EXPECTED_TAG={tag}",
                       f"-DGTI_BUILD_ID={build_id}", "-P", str(script)]
            run(command, work, success)

        def contents(archive):
            checksum = archive.with_name(archive.name + ".sha256").read_text().split()
            assert checksum == [hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name]
            result = {}
            with tarfile.open(archive) as stream:
                for member in stream.getmembers():
                    if member.isfile():
                        result[member.name.removeprefix("./")] = stream.extractfile(member).read()
                    elif member.issym():
                        result[member.name.removeprefix("./")] = ("symlink", member.linkname)
            return result

        stable_directory = work / "stable"
        package("stable", f"v{version}", stable_directory)
        stable_archive = stable_directory / f"gti-v{version}-linux-x64.tar.gz"
        stable_bytes = stable_archive.read_bytes()
        stable = contents(stable_archive)
        assert "share/gti/BUILD_ID" not in stable
        sha = "a" * 40
        nightly_directory = work / "nightly"
        package("nightly", "nightly", nightly_directory, sha)
        nightly = contents(nightly_directory / "gti-nightly-linux-x64.tar.gz")
        assert nightly.pop("share/gti/BUILD_ID") == (sha + "\n").encode()
        # On macOS libarchive can emit AppleDouble attributes for the newly
        # created identity stamp. They belong to that stamp, not the payload.
        nightly.pop("share/gti/._BUILD_ID", None)
        differences = {name: (stable.get(name), nightly.get(name))
                       for name in stable.keys() | nightly.keys()
                       if stable.get(name) != nightly.get(name)}
        assert not differences, ("channels did not archive the same validated payload", differences)
        assert stable_archive.read_bytes() == stable_bytes, "nightly rewrote the immutable archive"
        assert stable["lib/gti/llvm/libclang.so.20"] == ("symlink", "libclang.so.20.1")
        rejected = work / "rejected"
        package("stable", f"v{version}", rejected, success=False, platform="darwin-arm64")
        assert not rejected.exists(), "macOS stage without runtime verification produced an archive"
        (stage / "share/gti/host-runtime.json").write_text('{"schema":1,"libraries":[]}\n')
        package("stable", f"v{version}", work / "darwin", platform="darwin-arm64")
        assert (work / "darwin" / f"gti-v{version}-darwin-arm64.tar.gz").is_file()
        package("stable", "v99.99.99", rejected, success=False)
        package("nightly", "nightly", rejected, "not-a-sha", success=False)
        (stage / "lib/libgti_runtime.a").unlink()
        package("stable", f"v{version}", rejected, success=False)
        assert not rejected.exists(), "invalid stage/tag produced release assets"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("--cmake", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    source_contract(root)
    if args.cmake:
        archive_contract(root, args.cmake.resolve())
    print("Exact source/version/tag checks, stale-nightly admission, and shared release packaging passed")


if __name__ == "__main__":
    main()
