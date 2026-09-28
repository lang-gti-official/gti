import os
from pathlib import Path
import subprocess
import tempfile


def required(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"missing required build environment {name}")
    return value


package = Path(required("GTI_BUILD_PACKAGE_DIR"))
output = Path(required("GTI_BUILD_OUT_DIR"))
result = Path(required("GTI_BUILD_OUTPUT"))
target = required("GTI_BUILD_TARGET")
jobs = required("GTI_BUILD_JOBS")
zlib_source = package / "vendor/zlib"
zlib_build = output / "zlib"
source = package / "vendor/libgit2"
build = output / "cmake"

if target.endswith("-windows"):
    raise SystemExit("the gti-libgit2 build script does not support Windows yet")

subprocess.run(
    [
        "cmake", "-S", zlib_source, "-B", zlib_build,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DZLIB_BUILD_SHARED=OFF",
        "-DZLIB_BUILD_STATIC=ON",
        "-DZLIB_BUILD_TESTING=OFF",
        "-DZLIB_INSTALL=OFF",
    ],
    check=True,
)
subprocess.run(
    ["cmake", "--build", zlib_build, "--config", "Release", "--parallel",
     jobs, "--target", "zlibstatic"],
    check=True,
)
zlib_archive = zlib_build / "libz.a"

subprocess.run(
    [
        "cmake", "-S", source, "-B", build,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_TESTS=OFF",
        "-DBUILD_CLI=OFF",
        "-DBUILD_EXAMPLES=OFF",
        "-DBUILD_FUZZERS=OFF",
        "-DEXPERIMENTAL_SHA256=OFF",
        "-DUSE_THREADS=ON",
        "-DUSE_SSH=OFF",
        "-DUSE_HTTPS=OFF",
        "-DUSE_GSSAPI=OFF",
        "-DUSE_NTLMCLIENT=OFF",
        "-DUSE_ICONV=OFF",
        "-DUSE_SHA1=CollisionDetection",
        "-DUSE_SHA256=Builtin",
        "-DUSE_HTTP_PARSER=builtin",
        "-DREGEX_BACKEND=builtin",
        "-DUSE_BUNDLED_ZLIB=OFF",
        f"-DENABLE_REPRODUCIBLE_BUILDS={'ON' if target.endswith('-linux') else 'OFF'}",
        f"-DZLIB_INCLUDE_DIR={zlib_source}",
        f"-DZLIB_LIBRARY={zlib_archive}",
    ],
    check=True,
)
subprocess.run(
    ["cmake", "--build", build, "--config", "Release", "--parallel", jobs,
     "--target", "libgit2package"],
    check=True,
)

lines = ["version = 1", 'link-files = ["cmake/libgit2.a"]']
if target.endswith("-linux"):
    lines.append('libraries = ["pthread"]')
elif not target.endswith("-macos"):
    raise SystemExit(f"unsupported libgit2 target: {target}")

result.parent.mkdir(parents=True, exist_ok=True)
descriptor, temporary = tempfile.mkstemp(prefix=".gti-build-output-",
                                          dir=result.parent)
try:
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write("\n".join(lines) + "\n")
    os.replace(temporary, result)
finally:
    Path(temporary).unlink(missing_ok=True)
