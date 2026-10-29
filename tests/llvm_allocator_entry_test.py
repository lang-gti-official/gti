#!/usr/bin/env python3
"""Observe selected public allocator entries through real production links.

This is a single-thread, bounded phase audit, not whole-program allocation or
runtime timing evidence. Native positive controls must pass before GTI counts
are accepted. The production GTI byte provider and runtime archive are retained.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess

from llvm_runtime_cost_test import CASES, validate_build_report

ENTRIES = ("malloc", "calloc", "realloc", "posix_memalign", "free")
PROTOTYPES = '''extern "C" {
  int gti_cost_iterations();
  void gti_cost_begin();
  int gti_cost_end(c_ulonglong checksum, c_ulonglong effects);
}
'''
PROVIDER = '''import std.memory;
int main() {
  int count = gti_cost_iterations(); mut uint64_t checksum = 7;
  mut std::detail::process_lease lease{};
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) {
    unsafe {
      auto allocated = lease.allocate(std::layout(uint64_t(257), uint64_t(256)));
      if (!allocated) { return 1; }
      auto region = allocated.value();
      if (region.address == nullptr || region.bytes != uint64_t(257) ||
          region.alignment != uint64_t(256)) { return 2; }
      lease.attach(region);
      checksum = (checksum * 33 + region.bytes) % 1000003;
      lease.release();
    }
  }
  return gti_cost_end(c_ulonglong(checksum), c_ulonglong(0));
}
'''
REJECTIONS = '''import std.memory;
int main() {
  int count = gti_cost_iterations();
  mut std::detail::process_lease lease{};
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) {
    unsafe {
      auto rejected = lease.allocate(std::layout(uint64_t(16), uint64_t(3)));
      if (rejected || rejected.error() != std::alloc_errc::invalid_alignment) { return 1; }
      auto zero = lease.allocate(std::layout(uint64_t(0), uint64_t(256)));
      if (!zero || zero.value().address != nullptr) { return 2; }
      lease.attach(zero.value());
      lease.release();
    }
  }
  return gti_cost_end(c_ulonglong(0), c_ulonglong(0));
}
'''


SHARED_OWNER = '''import std.memory;
int main() {
  int count = gti_cost_iterations(); mut uint64_t checksum = 7;
  gti_cost_begin();
  for (mut int i = 0; i < count; i += 1) {
    mut auto owner = std::make_shared<int>(257);
    auto observer = owner.downgrade();
    for (mut int j = 0; j < 10; j += 1) {
      auto alias = owner.clone();
      if (alias.use_count() != 2 || *alias != 257) { return 1; }
    }
    checksum = (checksum * 33 + uint64_t(*owner)) % 1000003;
    { auto promoted = observer.lock(); if (!promoted || *promoted != 257) { return 2; } }
    owner.reset();
    auto expired = observer.clone();
    if (!expired.expired() || expired.lock()) { return 3; }
  }
  return gti_cost_end(c_ulonglong(checksum), c_ulonglong(0));
}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checksum(name, count):
    value = 7
    for index in range(count):
        term = index if name == "arithmetic" else 7 * (index % 31 + 5) if name == "simd" else 257
        value = (value * 33 + term) % 1000003
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", type=Path)
    parser.add_argument("--gti-build-cache", type=Path)
    parser.add_argument("--cxx", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--native-controls-only", action="store_true",
                        help="partial mechanism evidence only; runs no GTI acceptance cases")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    assert 1 <= args.iterations <= 1000000
    if not args.native_controls_only:
        assert args.gti and args.gti_build_cache, "GTI audit requires its optimized compiler/cache"
        assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
        assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                         args.gti_build_cache.read_text(), re.M)
    system = platform.system()
    assert system in ("Darwin", "Linux"), "allocator audit needs a supported interposition mechanism"
    assert args.cxx.is_file()
    output.mkdir(parents=True)
    manifest = {
        "schema": 1, "status": "running", "commands": [], "native_controls": [], "cases": [],
        "platform": platform.platform(), "machine": platform.machine(),
        "gti_sha256": digest(args.gti) if args.gti else None,
        "native_compiler_sha256": digest(args.cxx),
        "gti_acceptance": "not run: native controls only" if args.native_controls_only else "pending",
        "compiler_build": None,
        "mechanism": "dyld interpose test dylib" if system == "Darwin" else "GNU-compatible linker --wrap",
        "scope": "attempted entries into five named public C allocator APIs during native begin/end markers",
        "limits": [
            "not successful allocation counts, live objects, byte totals, peak heap or whole-program no-allocation evidence",
            "startup, marker parsing/printing and work escaping beyond the observation phase are unobserved",
            "allocator-internal arenas, private/hidden entry points, direct mappings and custom pools may bypass all five APIs",
            "Linux wrap covers undefined references from participating linked objects, not calls internal to shared libraries",
            "dyld interposition has different image coverage; counts from different mechanisms are not normalized parity",
            "no C++ operator-new, emergency-pool or complete dynamic-library allocation audit is claimed",
            "single-thread profiles only; this audit establishes no concurrent runtime contract",
            "no runtime timing, common-target parity or speedup claim"],
        "iterations": args.iterations}

    def run(argv, *, env=None, kind="command"):
        path = output / f"command-{len(manifest['commands']):03}.log"
        result = subprocess.run([str(x) for x in argv], cwd=root, env=env,
                                text=True, capture_output=True, timeout=240)
        path.write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv], "exit": result.returncode,
                                     "log": path.name, "kind": kind})
        assert result.returncode == 0, str(path)
        return result

    def observe(binary, wanted, arguments=()):
        env = dict(os.environ, NO_COLOR="1", GTI_COLOR="never",
                   GTI_ALLOCATOR_AUDIT_ITERATIONS=str(args.iterations),
                   GTI_ALLOCATOR_AUDIT_CHECKSUM=str(wanted), GTI_ALLOCATOR_AUDIT_EFFECTS="0")
        if system == "Darwin":
            env["DYLD_INSERT_LIBRARIES"] = str(audit)
        result = run([binary, *arguments], env=env, kind="observation")
        assert not result.stderr
        row = json.loads(result.stdout)
        assert row["kind"] == "allocator-entry-observation" and row["begins"] == row["ends"] == 1
        assert row["checksum"] == wanted and row["effects"] == 0
        assert all(isinstance(row[name], int) and row[name] >= 0 for name in ENTRIES)
        return row

    try:
        fixtures = root / "tests/fixtures/llvm_optimization"
        source = fixtures / "allocator_entry_audit.cpp"
        common = [args.cxx, "-std=c++17", "-O2", "-fPIC"]
        if system == "Darwin":
            audit = output / "libgti_allocator_entry_audit.dylib"
            run([*common, "-dynamiclib", source, "-o", audit], kind="audit-build")
            native_inputs = [audit]
        else:
            audit = output / "allocator_entry_audit.o"
            run([*common, "-c", source, "-o", audit], kind="audit-build")
            native_inputs = [audit, *(f"-Wl,--wrap={name}" for name in ENTRIES)]
        manifest["audit_material"] = {"path": str(audit), "sha256": digest(audit),
                                      "source_sha256": digest(source),
                                      "native_inputs": [str(x) for x in native_inputs],
                                      "dyld_insert_libraries": str(audit) if system == "Darwin" else None}
        controls = output / "native-controls"
        run([*common, *(f"-fno-builtin-{name}" for name in ("malloc", "calloc", "realloc", "free")),
             fixtures / "allocator_entry_controls.cpp", *native_inputs, "-o", controls], kind="control-build")
        empty = observe(controls, 0, ["empty"])
        assert all(empty[name] == 0 for name in ENTRIES), "empty native phase observed allocator entries"
        manifest["native_controls"].append({"case": "empty", **empty})
        positive = observe(controls, 1, ["positive"])
        assert all(positive[name] >= minimum for name, minimum in
                   (("malloc", 1), ("calloc", 1), ("realloc", 1), ("posix_memalign", 1), ("free", 3))), "native interposition positive control failed"
        manifest["native_controls"].append({"case": "positive", **positive})
        if args.native_controls_only:
            assert len(manifest["native_controls"]) == 2 and not manifest["cases"]
            manifest["status"] = "passed-native-interposition-controls-only"
            print("PASS: 2 native interposition controls only; no GTI acceptance cases run")
            return
        cases = {"arithmetic": CASES["arithmetic"][1], "simd": CASES["simd"][1],
                 "process-provider": PROVIDER, "provider-rejections": REJECTIONS,
                 "shared-owner": SHARED_OWNER}
        for name, body in cases.items():
            path = output / f"{name}.gti"
            path.write_text(PROTOTYPES + body)
            wanted = 0 if name == "provider-rejections" else checksum(name, args.iterations)
            endpoints = [("cpp", 3), *(("llvm", level) for level in range(4))]
            if name == "shared-owner":
                # Public owning matrices separately require the exact C++
                # unsupported diagnostic; this audit measures LLVM allocations.
                endpoints = [("llvm", level) for level in range(4)]
            for backend, level in endpoints:
                binary = output / f"{name}-{backend}-O{level}"
                report_path = binary.with_suffix(".json")
                run([args.gti, path, "--backend", backend, f"-O{level}", "--cxx", args.cxx,
                     "--build-report", report_path, "-o", binary, "--", *native_inputs], kind="gti-build")
                facts = validate_build_report(json.loads(report_path.read_text()), manifest["gti_sha256"], binary)
                if manifest["compiler_build"] is None:
                    manifest["compiler_build"] = facts["compiler_build"]
                assert facts["compiler_build"] == manifest["compiler_build"]
                row = observe(binary, wanted)
                if name == "shared-owner":
                    assert row["posix_memalign"] == row["free"] == args.iterations, "shared owner must allocate/release exactly one combined region; clones allocate nothing"
                    assert all(row[entry] == 0 for entry in ("malloc", "calloc", "realloc")), "unexpected shared-owner allocator entry"
                elif name == "process-provider":
                    assert row["posix_memalign"] == row["free"] == args.iterations, "production provider requests/releases were not observed exactly"
                else:
                    assert all(row[entry] == 0 for entry in ENTRIES), "profile observed unexpected public allocator entries"
                manifest["cases"].append({"case": name, "backend": backend, "optimization": level,
                                          "report": report_path.name, "provenance": facts, "observation": row})
        assert len(manifest["native_controls"]) == 2 and len(manifest["cases"]) == 24
        assert digest(args.gti) == manifest["gti_sha256"] and digest(audit) == manifest["audit_material"]["sha256"]
        manifest["gti_acceptance"] = "24 bounded observed-entry profiles passed; capture limits apply"
        manifest["status"] = "passed-bounded-public-entry-audit"
        print("PASS: 2 mandatory native controls and 24 GTI public allocator-entry observations; capture limits retained")
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
