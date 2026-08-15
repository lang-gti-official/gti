#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


ASSERTIONS = r"""

namespace gti_failure_descriptor_compile_test {

constexpr bool source_is_fixture(::gti_c_string_view source) {
  constexpr char expected[] = "failure_descriptor_backend.gti";
  if (source.length != sizeof(expected) - 1) {
    return false;
  }
  for (std::uint64_t index = 0; index < source.length; ++index) {
    if (source.data[index] != expected[index]) {
      return false;
    }
  }
  return true;
}

constexpr bool descriptor_is_exact() {
  constexpr auto &artifact =
      ::gti_internal::backend::__gti_failure_artifact_descriptor_v1;
  if (artifact.abi_version != GTI_FAILURE_ABI_VERSION_V1 ||
      artifact.reserved != 0 || artifact.sites == nullptr ||
      artifact.site_count < 2 || artifact.sites_reserved != 0 ||
      artifact.canonical_descriptor == nullptr ||
      artifact.canonical_descriptor_size < 24) {
    return false;
  }
  constexpr char prefix[] = "GTI-FAILURE-ARTIFACT-V1";
  for (std::size_t index = 0; index < sizeof(prefix) - 1; ++index) {
    if (artifact.canonical_descriptor[index] !=
        static_cast<std::uint8_t>(prefix[index])) {
      return false;
    }
  }
  if (artifact.canonical_descriptor[sizeof(prefix) - 1] != 0) {
    return false;
  }
  bool nonzero_identity = false;
  for (std::uint8_t byte : artifact.artifact_identity) {
    nonzero_identity = nonzero_identity || byte != 0;
  }

  bool addition = false;
  bool division = false;
  for (std::uint32_t index = 0; index < artifact.site_count; ++index) {
    const ::gti_failure_site_descriptor_v1 &site = artifact.sites[index];
    if (!source_is_fixture(site.logical_source) || site.reserved != 0) {
      continue;
    }
    if (site.line == 2 && site.start == 65 && site.end == 66 &&
        site.outcomes != nullptr && site.outcome_count == 1 &&
        site.outcomes[0].code == GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1 &&
        site.outcomes[0].detail == GTI_FAILURE_DETAIL_ADDITION_V1 &&
        site.outcomes[0].reserved == 0) {
      addition = true;
    }
    if (site.line == 6 && site.start == 145 && site.end == 146 &&
        site.outcomes != nullptr && site.outcome_count == 2 &&
        site.outcomes[0].code == GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1 &&
        site.outcomes[0].detail == GTI_FAILURE_DETAIL_DIVISION_V1 &&
        site.outcomes[0].reserved == 0 &&
        site.outcomes[1].code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 &&
        site.outcomes[1].detail ==
            GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1 &&
        site.outcomes[1].reserved == 0) {
      division = true;
    }
  }
  return nonzero_identity && addition && division;
}

static_assert(descriptor_is_exact());

} // namespace gti_failure_descriptor_compile_test
"""


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: failure_descriptor_backend_compile_test.py "
            "<gti> <source> <cxx> <source-root>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    cxx = pathlib.Path(sys.argv[3]).resolve()
    source_root = pathlib.Path(sys.argv[4]).resolve()
    runtime_include = source_root / "runtime" / "include"

    with tempfile.TemporaryDirectory(prefix="gti-failure-descriptor-") as temp:
        root = pathlib.Path(temp)
        for standard in ("c++20", "c++23"):
            generated = root / f"descriptor-{standard}.cpp"
            emission = run(
                [
                    str(compiler),
                    str(source),
                    "-O0",
                    "--std",
                    standard,
                    "--emit-cpp",
                    "-o",
                    str(generated),
                ]
            )
            if emission.returncode != 0:
                return fail(emission)
            text = generated.read_text(encoding="utf8")
            if text.count("#include <gti/runtime_failure.h>") != 1:
                sys.stderr.write(
                    f"{standard} did not include exactly one failure ABI header\n"
                )
                return 1
            if text.count("__gti_failure_artifact_descriptor_v1 = {") != 1:
                sys.stderr.write(
                    f"{standard} did not emit exactly one artifact descriptor\n"
                )
                return 1
            generated.write_text(text + ASSERTIONS, encoding="utf8")

            compiled = run(
                [
                    str(cxx),
                    f"-std={standard}",
                    "-D__gti_strict_ieee754=1",
                    "-I",
                    str(runtime_include),
                    "-c",
                    str(generated),
                    "-o",
                    str(root / f"descriptor-{standard}.o"),
                ]
            )
            if compiled.returncode != 0:
                return fail(compiled)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
