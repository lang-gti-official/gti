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
source = package / "vendor/assimp"
build = output / "cmake"

if target.endswith("-windows"):
    raise SystemExit("the gti-assimp build script does not support Windows yet")

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
        f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={build / 'lib'}",
        "-DASSIMP_BUILD_TESTS=OFF",
        "-DASSIMP_BUILD_ASSIMP_TOOLS=OFF",
        "-DASSIMP_BUILD_SAMPLES=OFF",
        "-DASSIMP_INSTALL=OFF",
        "-DASSIMP_NO_EXPORT=ON",
        "-DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=ON",
        "-DASSIMP_BUILD_DRACO=OFF",
        "-DASSIMP_HUNTER_ENABLED=OFF",
        "-DASSIMP_DOUBLE_PRECISION=OFF",
        "-DASSIMP_WARNINGS_AS_ERRORS=OFF",
        "-DASSIMP_BUILD_ZLIB=OFF",
        "-DASSIMP_BUILD_MINIZIP=ON",
        f"-DZLIB_INCLUDE_DIR={zlib_source}",
        f"-DZLIB_LIBRARY={zlib_archive}",
        f"-DZLIB_LIBRARY_REL={zlib_archive}",
        f"-DZLIB_LIBRARY_DBG={zlib_archive}",
    ],
    check=True,
)
subprocess.run(
    ["cmake", "--build", build, "--config", "Release", "--parallel", jobs,
     "--target", "assimp"],
    check=True,
)

lines = [
    "version = 1",
    'include-dirs = ["cmake/include"]',
    'link-files = ["cmake/lib/libassimp.a"]',
]
if target.endswith("-linux"):
    lines.append('libraries = ["m", "pthread"]')
elif not target.endswith("-macos"):
    raise SystemExit(f"unsupported Assimp target: {target}")

result.parent.mkdir(parents=True, exist_ok=True)
descriptor, temporary = tempfile.mkstemp(prefix=".gti-build-output-",
                                          dir=result.parent)
try:
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write("\n".join(lines) + "\n")
    os.replace(temporary, result)
finally:
    Path(temporary).unlink(missing_ok=True)
