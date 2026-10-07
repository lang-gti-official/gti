"""Nominal null field construction and isolated native C declarations."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    sources = {
        "nullable_member": root / "fixtures/nullable_member_construction.gti",
        "unique_member": root / "coverage/reproducers/a36-unique-ptr-member-init-nullptr.gti",
        "native_strlen": root / "coverage/reproducers/a37-libc-strlen-prototype-conflict.gti",
        "native_qsort": root / "coverage/reproducers/a37-libc-qsort-link-name-conflict.gti",
        "nullable_example": root.parent / "examples/13-ownership.gti",
        "native_identity": root / "fixtures/native_declaration_identity.gti",
    }
    with tempfile.TemporaryDirectory(prefix="gti-native-identity-") as temporary:
        directory = Path(temporary)
        native = directory / "native.o"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c17", "-O2", "-c",
                        str(root / "fixtures/native_declaration_identity.c"),
                        "-o", str(native)], check=True, capture_output=True, text=True)
        subprocess.run([sys.argv[1], str(sources["native_identity"]),
                        "--emit-native-header", "-o", str(directory / "native.h")],
                       check=True, capture_output=True, text=True)
        header = (directory / "native.h").read_text(encoding="utf-8")
        if "__gti_c_fn_" in header or "gti_external_length(" not in header:
            raise AssertionError("bridge header must preserve public external C identity")
        consumer = directory / "consumer"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c17", "-I", str(directory),
                        "-I", str(root.parent / "runtime/include"),
                        str(root / "fixtures/native_declaration_identity_header.c"),
                        str(native), "-o", str(consumer)],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(consumer)], check=True, capture_output=True, text=True)
        for standard in ("c++20", "c++23"):
            consumer_cpp = directory / ("consumer-" + standard)
            subprocess.run([os.environ.get("CXX", "c++"), "-x", "c++",
                            "-std=" + standard, "-I", str(directory),
                            "-I", str(root.parent / "runtime/include"),
                            str(root / "fixtures/native_declaration_identity_header.c"),
                            "-x", "none", str(native), "-o", str(consumer_cpp)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(consumer_cpp)], check=True, capture_output=True, text=True)
        main(names=tuple(sources), fixture_paths=sources,
             native_inputs_by_name={"native_identity": [str(native)]},
             cases={"native_qsort": [((), (0, "1\n", ""))],
                    "nullable_example": [((), (0, "ownership worked\nwidget dropped\n", ""))]})
