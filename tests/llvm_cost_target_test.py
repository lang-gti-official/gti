#!/usr/bin/env python3
"""Actual-input target-policy controls for the bounded macOS comparator.

No controlled timing is performed. A default-comparator mismatch remains a
negative control; unsupported or unproven normalization never falls back.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

from llvm_runtime_cost_test import CASES, DECLARATIONS, validate_build_report


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", type=Path, required=True)
    parser.add_argument("--gti-build-cache", type=Path, required=True)
    parser.add_argument("--cxx", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=tuple(CASES), default="simd")
    parser.add_argument("--mismatch-controls-only", action="store_true",
                        help="partial actual-input negative controls; no normalized comparator acceptance")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    output.mkdir(parents=True)
    manifest = {"schema": 1, "status": "running", "commands": [], "builds": [],
                "gti_sha256": digest(args.gti), "real_native_compiler_sha256": digest(args.cxx),
                "probe_sha256": digest(args.probe), "runtime_timing": "not performed",
                "build_observation_contract": "observer/save-temp/build-report collection adds overhead; not bare compiler cost",
                "profile_scope": "declared native user-object/executable target/ABI/FP/mode properties; not all implementation policy"}

    def run(argv, *, allowed=(0,)):
        result = subprocess.run([str(x) for x in argv], cwd=root, text=True,
                                capture_output=True, timeout=240)
        path = output / f"command-{len(manifest['commands']):03}.log"
        path.write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv], "exit": result.returncode,
                                     "log": path.name})
        assert result.returncode in allowed, str(path)
        return result

    def actual_input(name, suffix):
        record = json.loads((output / "observed" / name / "record.json").read_text())
        assert record["exit"] == 0 and not record["changed_inputs"]
        assert record["compiler"]["sha256"] == manifest["real_native_compiler_sha256"]
        rows = [row for row in record["inputs"] + record["outputs"]
                if Path(row["original"]).suffix == suffix and row.get("retained")]
        assert len(rows) == 1, (name, suffix, rows)
        path = output / "observed" / name / rows[0]["retained"]
        assert digest(path) == rows[0]["sha256"]
        return path

    def codegen_selection(name):
        text = (output / "observed" / name / "stderr.log").read_text()
        rows = []
        for line in text.splitlines():
            try:
                argv = shlex.split(line)
            except ValueError:
                continue
            if "-cc1" not in argv or not any(flag in argv for flag in ("-emit-llvm-bc", "-emit-obj", "-S")):
                continue
            cpu = [argv[index + 1] for index, item in enumerate(argv[:-1]) if item == "-target-cpu"]
            features = [argv[index + 1] for index, item in enumerate(argv[:-1]) if item == "-target-feature"]
            assert len(cpu) == 1, "actual native codegen has no unique observed CPU"
            rows.append({"argv": argv, "cpu": cpu[0], "features": ",".join(features)})
        assert rows, "no actual C++ codegen selection retained"
        assert len({(row["cpu"], row["features"]) for row in rows}) == 1, "native codegen stages disagree"
        return rows[-1], rows

    try:
        lifetime = json.loads(run([args.probe, "--metadata-lifetime"]).stdout)
        assert lifetime["metadata_json_round_trip"] and lifetime["mc_object_destroyed_before_serialization"]
        assert lifetime["raw_feature_requests_differ"] and lifetime["normalized_effective_features_equal"]
        manifest["mc_metadata_controls"] = lifetime
        reference = json.loads(run([args.probe, "--factory"]).stdout)
        (output / "factory.json").write_text(json.dumps(reference, indent=2) + "\n")
        assert reference["factory_machine_count"] == 1 and reference["instrumentation"] == "none" and not reference["debug_info"]
        assert reference["triple"].startswith(("arm64-apple-darwin", "aarch64-apple-darwin")), "normalization currently requires the bounded macOS arm64 profile"
        assert reference["effective_subtarget"]["cpu"] == "generic", "unsupported observed native CPU profile"
        wrapper = output / "native-tools/clang++"
        wrapper.parent.mkdir()
        wrapper.write_text(f"#!{sys.executable}\n" +
                           "import sys\n" + f"sys.path.insert(0, {str(root / 'tests')!r})\n" +
                           "from llvm_cost_compiler_observer import observe\n" +
                           f"raise SystemExit(observe({str(args.cxx.absolute())!r}, {str(output / 'observed')!r}, sys.argv[1:]))\n")
        wrapper.chmod(0o755)
        manifest["observer"] = {"launcher": str(wrapper), "launcher_sha256": digest(wrapper),
                                "implementation_sha256": digest(root / "tests/llvm_cost_compiler_observer.py"),
                                "python": sys.executable, "python_sha256": digest(Path(sys.executable))}
        family, body = CASES[args.case]
        oracle = output / "oracle"
        run([args.cxx, "-x", "c", "-std=c17", "-O2", root / "tests/fixtures/llvm_optimization/runtime_cost_oracle.c", "-o", oracle])
        expected = json.loads(run([oracle, str(family), "10000"]).stdout)
        source = output / f"{args.case}.gti"
        declarations = DECLARATIONS.replace("COUNT", "10000").replace("CHECKSUM", str(expected["checksum"])).replace("EFFECTS", str(expected["effects"]))
        source.write_text(declarations + body + "\n")
        normalized_flags = [f"--target={reference['triple']}", "-mcpu=generic", "-fPIC",
                            "-mcmodel=small", "-fno-fast-math", "-ffp-contract=off"]
        builds = [("llvm.bin", "llvm", ["-v", "-Wl,-t"]),
                  ("cpp-default.bin", "cpp", ["-save-temps=obj", "-v", "-Wl,-t"])]
        if not args.mismatch_controls_only:
            builds.append(("cpp-normalized.bin", "cpp",
                           [*normalized_flags, "-save-temps=obj", "-v", "-Wl,-t"]))
        for name, backend, native in builds:
            binary, report = output / name, output / (name + ".json")
            run([args.gti, source, "--backend", backend, "-O3", "--cxx", wrapper,
                 "--build-report", report, "-o", binary, "--", *native])
            facts = validate_build_report(json.loads(report.read_text()), manifest["gti_sha256"], binary)
            assert facts["compiler_build"]["state"] == "clean" and facts["gti_source_head"] == reference["compiler_commit"], "probe and actual producer source are not bound"
            record = json.loads((output / "observed" / name / "record.json").read_text())
            outputs = [row for row in record["outputs"] if row["original"].endswith("/" + name)]
            assert len(outputs) == 1 and outputs[0]["sha256"] == facts["artifact"]["sha256"], "observed native output differs from actual published artifact"
            result = run([binary])
            assert not result.stdout and not result.stderr
            manifest["builds"].append({"name": name, "backend": backend, "optimization": 3,
                                       "native_arguments": native, "provenance": facts,
                                       "report": report.name, "observer": f"observed/{name}/record.json"})
        comparisons = []
        for name in (["cpp-default.bin"] if args.mismatch_controls_only else
                     ["cpp-default.bin", "cpp-normalized.bin"]):
            selection, stages = codegen_selection(name)
            inputs = {"cpp_bitcode": str(actual_input(name, ".bc")),
                      "cpp_object": str(actual_input(name, ".o")),
                      "llvm_object": str(actual_input("llvm.bin", ".o")),
                      "cpp_executable": str(output / name), "llvm_executable": str(output / "llvm.bin"),
                      "cpp_default_cpu": selection["cpu"], "cpp_default_features": selection["features"],
                      "actual_codegen_stages": stages}
            path = output / (name + "-target-inputs.json")
            path.write_text(json.dumps(inputs, indent=2) + "\n")
            result = run([args.probe, "--compare", path], allowed=(0, 1))
            facts = json.loads(result.stdout)
            target_report = output / (name + "-target.json")
            target_report.write_text(json.dumps(facts, indent=2) + "\n")
            comparisons.append({"case": name, "exit": result.returncode,
                                "report": target_report.name, "errors": facts.get("errors", []),
                                "normalization_accepted": facts["normalization_accepted"]})
        manifest["comparisons"] = comparisons
        assert comparisons[0]["exit"] == 1 and not comparisons[0]["normalization_accepted"], "default mismatch negative control unexpectedly passed"
        assert "effective MC features differ after dependency normalization" in comparisons[0]["errors"], "actual mismatch control did not establish feature normalization"
        if args.mismatch_controls_only:
            manifest["normalized_timing_acceptance"] = "not accepted: partial mismatch controls only; explicit target/CPU path remains prerequisite"
            manifest["status"] = "passed-actual-input-mismatch-controls-only"
            print("PASS: 2 actual executable checks and default target-mismatch control; normalized timing remains unaccepted")
            return
        if comparisons[1]["exit"] != 0 or not comparisons[1]["normalization_accepted"]:
            manifest["status"] = "failed-or-unproven-target-profile"
            raise AssertionError("actual normalized inputs do not establish the declared target profile; no timing accepted")
        assert digest(args.gti) == manifest["gti_sha256"] and digest(args.cxx) == manifest["real_native_compiler_sha256"]
        manifest["status"] = "passed-bounded-target-profile-controls"
        print("PASS: actual-input default mismatch rejected and bounded normalized target profile verified; no timing performed")
    except BaseException as error:
        if manifest["status"] == "running":
            manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
