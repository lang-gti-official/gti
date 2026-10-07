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
source = package / "vendor/glfw"
build = output / "cmake"

if target.endswith("-windows"):
    raise SystemExit("the gti-glfw build script does not support Windows yet")

subprocess.run(
    [
        "cmake", "-S", source, "-B", build,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DGLFW_BUILD_DOCS=OFF",
        "-DGLFW_BUILD_TESTS=OFF",
        "-DGLFW_BUILD_EXAMPLES=OFF",
        "-DGLFW_BUILD_WAYLAND=OFF",
    ],
    check=True,
)
subprocess.run(
    ["cmake", "--build", build, "--config", "Release", "--parallel", jobs,
     "--target", "glfw"],
    check=True,
)

lines = ["version = 1", 'link-files = ["cmake/src/libglfw3.a"]']
if target.endswith("-macos"):
    lines.append('frameworks = ["Cocoa", "IOKit", "CoreFoundation"]')
elif target.endswith("-linux"):
    lines.append('libraries = ["m", "dl", "pthread"]')
else:
    raise SystemExit(f"unsupported GLFW target: {target}")

result.parent.mkdir(parents=True, exist_ok=True)
descriptor, temporary = tempfile.mkstemp(prefix=".gti-build-output-",
                                          dir=result.parent)
try:
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write("\n".join(lines) + "\n")
    os.replace(temporary, result)
finally:
    Path(temporary).unlink(missing_ok=True)
