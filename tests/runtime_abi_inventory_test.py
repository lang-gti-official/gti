#!/usr/bin/env python3
"""Inventory real runtime C declarations through Clang, not a C type parser.

This is validation/reporting tooling, never an LLVM-backend dependency or a
source of GTI meaning. The exact platform-specific report stays in the build.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


# These are ownership/failure families, not native-lowering admission. Headers
# remain the authority for each operation's argument and resource contract.
POLICIES = {
    "c_abi.h": ("borrowed records", "no functions", "none"),
    "charconv.h": ("call-bounded input/output buffers", "explicit status", "none"),
    "chrono.h": ("call-bounded output records", "explicit status", "none"),
    "embedding.h": ("image-owned context; borrowed descriptors until unload", "generated lifecycle and export status; output publication only on success", "retained optional failure observer; no unwind through host boundary"),
    "cthread.h": ("opaque handles; explicit create/destroy/join", "status or documented terminal contract", "C callbacks; explicit attachment and containment"),
    "environment.h": ("call-bounded views and output buffers", "explicit status", "none"),
    "filesystem.h": ("explicit opaque directory handles and borrowed buffers", "explicit status", "none"),
    "http.h": ("call-bounded request and output buffers", "explicit status", "none"),
    "math.h": ("scalar values", "IEEE values; no GTI failure", "none"),
    "memory.h": ("raw native allocation authority; exact matching release family", "status/null; see individual header contract", "none"),
    "random.h": ("call-bounded output buffers", "explicit status", "none"),
    "runtime.h": ("borrowed buffers and explicit descriptor/process/socket handles", "status/count or documented terminal contract", "none"),
    "runtime_execution.h": ("attached TLS lifetime; borrowed failure records", "contract violation terminates; cleanup uses failure ABI", "registered TLS cleanup; no unwind across C"),
    "runtime_failure.h": ("call/scope-bounded records and pinned descriptors", "report status or terminal exit/abort; legacy managed handoff may unwind", "observer containment; managed boundary required"),
    "runtime_thread.h": ("explicit task/state/stop/synchronization authority", "status and copied failure records", "task callbacks with versioned failure protocol"),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--clang", default="clang")
    args = parser.parse_args()
    include = args.root / "runtime/include"
    headers = sorted((include / "gti").glob("*.h"))
    assert {path.name for path in headers} == set(POLICIES), "runtime header ownership inventory drift"
    source = "".join(f'#include "gti/{path.name}"\n' for path in headers)
    command = [args.clang, "-x", "c", "-std=c11", "-fparse-all-comments", "-I", str(include),
               "-Xclang", "-ast-dump=json", "-fsyntax-only", "-"]
    result = subprocess.run(command, input=source, text=True, capture_output=True, check=True)
    ast = json.loads(result.stdout)
    symbols = {}

    def comments(node):
        return ([node["text"]] if node.get("kind") == "TextComment" else []) + [
            text for child in node.get("inner", []) for text in comments(child)]

    def walk(node):
        name = node.get("name", "")
        if node.get("kind") == "FunctionDecl" and name.startswith("gti_rt_"):
            owners = [path for path in headers if re.search(r"\b" + re.escape(name) + r"\s*\(", path.read_text())]
            assert len(owners) == 1, f"ambiguous runtime C declaration ownership: {name}"
            owner = owners[0]
            ownership, failure, callback = POLICIES[owner.name]
            parameters = [child for child in node.get("inner", []) if child.get("kind") == "ParmVarDecl"]
            version = re.search(r"_v(\d+)$", name)
            entry = {
                "symbol": name,
                "header": f"gti/{owner.name}",
                "signature": node["type"]["qualType"],
                "parameters": [{"name": param.get("name", ""),
                                "type": param["type"].get("desugaredQualType", param["type"]["qualType"])}
                               for param in parameters],
                "abi_version": int(version.group(1)) if version else "legacy-unversioned",
                "ownership": ownership,
                "failure": failure,
                "callbacks": callback,
                "contract_comments": comments(node),
                "linkage": "C",
                "llvm_instruction_lowering": "pending",
            }
            assert name not in symbols or symbols[name]["signature"] == entry["signature"], name
            symbols[name] = entry
        for child in node.get("inner", []):
            walk(child)

    walk(ast)
    preprocessed = subprocess.run([args.clang, "-x", "c", "-std=c11", "-I", str(include), "-E", "-P", "-"],
                                  input=source, text=True, capture_output=True, check=True).stdout
    expected = set(re.findall(r"\b(gti_rt_\w+)\s*\(", preprocessed))
    assert set(symbols) == expected, f"runtime C inventory drift: missing={sorted(expected - set(symbols))}, extra={sorted(set(symbols) - expected)}"
    assert len(symbols) > 100, "runtime inventory unexpectedly empty"
    # Generated per-image entry points are declared publicly but are not
    # runtime archive services. Keep the distinction explicit in this inventory.
    embedding = (include / "gti/embedding.h").read_text()
    for name in ("gti_get_module_descriptor_v1", "gti_context_create_v1",
                 "gti_context_destroy_v1"):
        assert re.search(r"\b" + name + r"\s*\(", embedding)
        assert name not in symbols
    args.output.write_text(json.dumps({"schema": 1, "command": command,
                                      "services": [symbols[key] for key in sorted(symbols)]}, indent=2) + "\n")
    print(f"Inventoried {len(symbols)} exact C signatures with ownership/failure/callback contracts")


if __name__ == "__main__":
    main()
