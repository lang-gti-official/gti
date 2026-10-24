#!/usr/bin/env python3
"""Exercise exact release/channel admission and the shared-build GitHub workflow contracts."""

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


def workflow_contract(root):
    release = (root / ".gitea/workflows/release.yml").read_text()
    ci = (root / ".gitea/workflows/ci.yml").read_text()
    assert not (root / ".github/workflows").exists() or not list((root / ".github/workflows").glob("*.yml"))
    assert "branches:\n      - main" in release and "paths:" not in release
    assert release.count("-DGTI_RELEASE_BUILD=ON") == 1, "channels must share one configure"
    assert release.count('run: cmake --build build --config Release --parallel "${GTI_JOBS}"') == 1
    assert release.count("name: Test build tree") == 1, "channels must share full CTest"
    assert release.count("name: Test installed toolchain") == 1
    assert "cancel-in-progress: false" in release
    assert "group: toolchain-${{ gitea.sha }}" in release, "new main must not cancel stable"
    package = release.split("  package:\n", 1)[1].split("  publish-stable:\n", 1)[0]
    stable = release.split("  publish-stable:\n", 1)[1].split("  publish-nightly:\n", 1)[0]
    nightly = release.split("  publish-nightly:\n", 1)[1]
    assert "runs-on: ubuntu-latest" in package and "Linux-X64) platform=linux-x64" in package
    assert "macos-15" not in package and "macOS-ARM64)" not in package, "macOS is explicitly manual"
    manual = (root / "docs/architecture/build-and-driver.md").read_text()
    assert "### Manual macOS release publication" in manual and "host-runtime.json" in manual
    assert "if: needs.metadata.outputs.stable == 'true'" in stable
    assert "refs/tags/${GTI_RELEASE_TAG}" in stable and "--force origin refs/tags/" not in stable
    assert "tag_name: ${{ needs.metadata.outputs.tag }}" in stable
    assert "target_commitish: ${{ needs.metadata.outputs.sha }}" in stable
    assert "name: gti-release-package" in stable
    assert package.index("-DGTI_OUTPUT_DIR=${{ gitea.workspace }}/dist/stable") < package.index("-DGTI_CHANNEL=nightly")
    assert "path: dist/stable/*" in package and "path: dist/nightly/*" in package
    assert 'mktemp -d "${GTI_RUNNER_TEMP}/gti-stage.XXXXXXXX"' in package
    assert package.count("-DGTI_STAGE_DIR=${{ env.GTI_STAGE_DIRECTORY }}") == 2
    assert "-DGTI_BUILD_ID=${{ needs.metadata.outputs.sha }}" in package
    assert "group: gti-nightly-publication" in nightly
    assert "name: gti-nightly-package" in nightly
    assert nightly.index("Download nightly archive") < nightly.index("admit-nightly") < nightly.index("git tag --force nightly")
    assert nightly.count("if: steps.admission.outputs.publish == 'true'") == 3
    assert nightly.index("name: Publish nightly release") < nightly.index("scripts/sync_release_assets.py")
    assert "prerelease: true" in nightly and "prerelease: false" in stable
    assert "releases: write" in stable and "releases: write" in nightly and "code: write" in nightly
    assert "permissions:\n  contents: read" in release
    assert "documentation-index:" not in ci, "documentation must reuse the native compiler"
    assert "build/gti_docgen --stdlib stdlib" in ci
    for contents, profiles in ((ci, ("native", "sanitizers")), (release, ("release",))):
        assert contents.count("bash scripts/configure_ci_cache.sh") == len(profiles)
        assert contents.count("https://gitea.com/actions/cache/restore@2cdf405574d6ef1f33a1d12acccd3ae82f47b3f2") == len(profiles)
        assert contents.count("https://gitea.com/actions/cache/save@2cdf405574d6ef1f33a1d12acccd3ae82f47b3f2") == len(profiles)
        assert contents.count("if: always() && steps.cache.outputs.cache-primary-key") == len(profiles)
        assert "ccache --show-stats" in contents and "save-always:" not in contents
        for profile in profiles:
            prefix = f"gti-ccache-v1-${{{{ runner.os }}}}-${{{{ runner.arch }}}}-{profile}-"
            assert prefix in contents
        assert "path: ${{ env.CCACHE_DIR }}" in contents
        assert "CMakeFiles/gti_driver.dir/src/driver/build.cpp.o" in contents
        assert "${{ github." not in contents and "GH_TOKEN" not in contents and "gh release" not in contents


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
        llvm_backend_archive = "libLLVMCore.a"
        (stage / "share/gti/llvm-backend-archives.txt").write_text(
            llvm_backend_archive + "\n")
        (stage / "lib/gti/llvm" / llvm_backend_archive).write_bytes(
            b"fixture LLVM backend archive\n")
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
    workflow_contract(root)
    source_contract(root)
    if args.cmake:
        archive_contract(root, args.cmake.resolve())
    print("Shared release workflow, exact source/version/tag checks, and stale-nightly admission passed")


if __name__ == "__main__":
    main()
